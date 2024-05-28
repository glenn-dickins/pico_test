/* i2s_examples.c
 *
 * Author: Daniel Collins
 * Date:   2022-02-25
 *
 * Copyright (c) 2022 Daniel Collins
 *
 * This file is part of rp2040_i2s_example.
 *
 * rp2040_i2s_example is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 as published by the
 * Free Software Foundation.
 *
 * rp2040_i2s_example is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * rp2040_i2s_example. If not, see <https://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "i2s.pio.h"


#ifndef PICO_DEFAULT_LED_PIN
#warning blink example requires a board with a regular LED
#else
const uint LED_PIN = PICO_DEFAULT_LED_PIN;
#endif


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// This set of parameters is useful configuration, with notes
//
// Overclocking to 288MHz is stable, and still allows the use of flash.
// Details and profiling at https://www.youtube.com/watch?v=G2BuoFNLoDM
// The regulator setting slightly higher at 1.2V ensures the 288MHz is stable
// 288MHz is a useful ratio for I2S, as it allows for 48kHz audio with a 6000x oversampling
// 
// The PIO execution is set so that 8 waits (or idle of 7) gives us a half bit clock at 96kHz.
// With this match to 48kHz, we therefore infer the bitclock from the internal 48kHz, and use
// instruction groups with 8 effective cycles.  The PIO then need only sync at each falling edge 
// of the LRCLK.  This would give us the ability to track incoming sources with up to 1000ppm error.
//
// The PIO clock is a divider of 2.9275 which is close enough to 3, but gives us a nice dithering of 
// the jitter.  The core PIO instructions are then running at either 2 or 3 system cycles (144Mhz or 
// 96Mhz) though on average it is close to 96Mhz being 98.304MHz.   This is faster than the 50Mhz resampling 
// that will be in the following Nexalist amplifier, so we are not adding any effective jitter in that case.
// For any other amplifier or DAC, we end up with jitter at about 4ms rms (10ms uniform) which is better
// going to give us SIG/THD of 70dB or more at 1kHz. 
// https://troll-audio.com/articles/time-resolution-of-digital-audio/
//
// 

#define     REG_VOLTAGE     VREG_VOLTAGE_1_20                               // Voltage regulator setting            
//#define     CLK_SYS         (288000000L)                                    // The system clock frequency
//#define     CLK_SYS           (295200000L)
#define     CLK_SYS             (196800000L)

#define     CLK_I2S         (48000)                                         // Single rate I2S frequency
#define     CLK_PIO         (2*CLK_I2S*64*2*16)                             // PIO execution rate (8 cycles each half bit of 2XI2S)
#define     CLK_PIO_DIV_N   ((int)(CLK_SYS/CLK_PIO))                        // PIO clock divider integer part
#define     CLK_PIO_DIF_F   ((int)(((CLK_SYS%CLK_PIO)*256LL+128)/CLK_PIO))  // PIO clock divider fractional part

int main() 
{
    vreg_set_voltage(REG_VOLTAGE);
    stdio_init_all();
    
    uint vco, postdiv1, postdiv2;
    int ret = check_sys_clock_khz(CLK_SYS/1000, &vco, &postdiv1, &postdiv2);
    printf("\n\nCHECKING CLOCK    %10ld %d %d %d %d\n", CLK_SYS, ret, vco, postdiv1, postdiv2);

    set_sys_clock_khz(CLK_SYS/1000, true);
    stdio_init_all();

    printf("\n\n\n\n");
    printf("SYSTEM CLOCK DESIRED:       %10ld\n", CLK_SYS);
    printf("SYSTEM CLOCK ACTUAL:        %10ld\n\n", clock_get_hz(clk_sys));

    // Init GPIO LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Setting up a PIO to be a slave at 48kHz, and then to double this and create a
    // clock suitable for 96kHz.  

    #define I2S_BASE         6

    printf("SETTING UP I2S\n");
    printf("I2S CLOCK DESIRED:          %10d\n", CLK_I2S);
    printf("PIO CLOCK DESIRED:          %10d\n", CLK_PIO);
    printf("PIO CLOCK DIVIDER:        %2d + %3d/256\n", CLK_PIO_DIV_N, CLK_PIO_DIF_F);
    printf("PIO CLOCK ACTUAL:           %10lld\n", (int64_t)(clock_get_hz(clk_sys) / ((float)CLK_PIO_DIV_N + ((float)CLK_PIO_DIF_F / 256.0f))));
    
    // Bidirection slave clocked with the fast clock
    uint8_t sm0    = pio_claim_unused_sm(pio0_hw, true);
    uint    offset = pio_add_program    (pio0_hw, &i2s_duplex_program);
    i2s_duplex_init                     (pio0_hw, sm0, offset, 12);
    pio_sm_set_clkdiv_int_frac          (pio0_hw, sm0, CLK_PIO_DIV_N, CLK_PIO_DIF_F);

    // Double rate clock generation
    uint8_t sm1    = pio_claim_unused_sm(pio1_hw, true);
    offset         = pio_add_program    (pio1_hw, &i2s_double_clk_program);
    i2s_double_clk_init                 (pio1_hw, sm1, offset, I2S_BASE);
    pio_sm_set_clkdiv_int_frac          (pio1_hw, sm1, CLK_PIO_DIV_N, CLK_PIO_DIF_F);

    // Double rate master data out
    uint8_t sm2    = pio_claim_unused_sm(pio1_hw, true);
    offset         = pio_add_program    (pio1_hw, &i2s_double_program);
    i2s_double_init                     (pio1_hw, sm2, offset, I2S_BASE);
    pio_sm_set_clkdiv_int_frac          (pio1_hw, sm2, CLK_PIO_DIV_N, CLK_PIO_DIF_F);




//  dma_double_buffer_init(i2s, dma_handler);
    
    pio_enable_sm_mask_in_sync(pio0_hw, (1u << sm0));
    pio_enable_sm_mask_in_sync(pio1_hw, (1u << sm1) | (1u << sm2));

    while (1)
    {
        gpio_put(LED_PIN, 1);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }

}

