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


#define     CLK_SYS         (288000000)   
#define     CLK_PIO         (CLK_SYS / 2)
#define     CLK_I2S_IO      (48000)
#define     CLK_I2S_OUT     (96000)

int main() 
{
    // Set a 132.000 MHz system clock to more evenly divide the audio frequencies
    
    // set_sys_clock_pll(1440000000, 5, 1);            // This is nicely 30,000 x 48000
    
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(144000, true);
    stdio_init_all();

    uint freq=288000, vco, postdiv1, postdiv2;
    int ret = check_sys_clock_khz(freq, &vco, &postdiv1, &postdiv2);
    printf("Clock details %d %d %d %d %d\n", freq, ret, vco, postdiv1, postdiv2);


    printf("System Clock: %lu\n", clock_get_hz(clk_sys));

    // Init GPIO LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Setting up a PIO to be a slave at 48kHz, and then to double this and create a
    // clock suitable for 96kHz.  

    #define I2S_BASE         6


    // Bidirection slave clocked with the fast clock
    uint8_t sm0    = pio_claim_unused_sm(pio0_hw, true);
    uint    offset = pio_add_program    (pio0_hw, &i2s_duplex_program);
    pio_sm_set_clkdiv_int_frac          (pio0_hw, sm0, 1, 0);
    i2s_duplex_init                     (pio0_hw, sm0, offset, I2S_BASE);

    // Double rate master creating its own clock
    uint8_t sm1    = pio_claim_unused_sm(pio1_hw, true);
    offset         = pio_add_program    (pio1_hw, &i2s_double_program);
    i2s_double_init                     (pio1_hw, sm1, offset, I2S_BASE);
    pio_sm_set_clkdiv_int_frac          (pio1_hw, sm1, 1, 0);

//  dma_double_buffer_init(i2s, dma_handler);
    
    pio_enable_sm_mask_in_sync(pio0_hw, (1u << sm0));
    pio_enable_sm_mask_in_sync(pio1_hw, (1u << sm1));

    while (1)
    {
        gpio_put(LED_PIN, 1);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }

}

