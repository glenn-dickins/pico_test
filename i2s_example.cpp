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
#include "histogram.hpp"
#include "arm_math.h"
#include "upsample.h"

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
// The PIO execution is set so that 16 waits (or idle of 15) gives us a half bit clock at 96kHz.
// With this match to 48kHz, we therefore infer the bitclock from the internal 48kHz, and use
// instruction groups with 8 effective cycles.  The PIO then need only sync at each falling edge 
// of the LRCLK.  This would give us the ability to track incoming sources with up to 1000ppm error.
//
// The PIO clock is a divider of 1.4638 which is close enough to 1.5, but gives us a nice dithering of 
// the jitter.  The core PIO instructions are then running at either 1 or 2 system cycles (288Mhz or 
// 144Mhz) though on average it is close to 144Mhz being 197MHz.   This is faster than the 50Mhz resampling 
// that will be in the following Nexalist amplifier, so we are not adding any effective jitter in that case.
// For any other amplifier or DAC, we end up with jitter at about 2ms rms (7ms uniform) which is 
// going to give us SIG/THD of 80dB or more at 1kHz. 
// https://troll-audio.com/articles/time-resolution-of-digital-audio/
//
// 

#define     REG_VOLTAGE     VREG_VOLTAGE_1_25                               // Voltage regulator setting            
#define     CLK_SYS         (288000000L)                                    // The system clock frequency
//#define   CLK_SYS         (196800000L)                                    // This is slightly less jitter (PIO div is 1.00)
#define     CLK_I2S         (48000)                                         // Single rate I2S frequency
#define     CLK_PIO         (2*CLK_I2S*64*2*16)                             // PIO execution rate (8 cycles each half bit of 2XI2S)
#define     CLK_PIO_DIV_N   ((int)(CLK_SYS/CLK_PIO))                        // PIO clock divider integer part
#define     CLK_PIO_DIV_F   ((int)(((CLK_SYS%CLK_PIO)*256LL+128)/CLK_PIO))  // PIO clock divider fractional part



// DMA Setup
#define ISR_BLOCK      4           // Number of samples at 48kHz that we lump into each ISR call
//int32_t audio_i2s[4][2][ISR_BLOCK][2] __attribute__((aligned(2*4*ISR_BLOCK*4))) = { };     // Four 2 ch I2S injest
int32_t   audio_tdm[1][2][ISR_BLOCK][8] __attribute__((aligned(2*8*ISR_BLOCK*4))) = { };     // One 8 ch TDM injest
int32_t   audio_out[4][2][ISR_BLOCK][4] __attribute__((aligned(2*4*ISR_BLOCK*4))) = { };
int32_t   audio_buf[8][ISR_BLOCK+FILTER2X_TAPS-1] = { };                                     // 8 channels of FIR buffer

using namespace DAES67;

Histogram   isr_call("ISR Call Time", 0, 0.0001);
Histogram   isr_exec("ISR Exec Time", 0, 0.0001);



int interrupt = 0;
static void dma_handler(void) 
{
    dma_start_channel_mask(0b000000011111);         // Restart this as soon as possible !!
    dma_hw->ints0 = 1u;                             // No rush for this, and should never re-enter
    int64_t time = isr_call.time();                 // Mark the ISR call time and setup for
    isr_exec.start(time);                           // measuring execution time

    int block = (void *)dma_hw->ch[0].read_addr != audio_out;       // Determine which double buffer to use

    // Move all of the TDM data into the I2S data buffers
    for (int n = 0; n < 8; n++)
    {
        int32_t *pbuf = audio_buf[n];
        int32_t *pin  = &audio_tdm[0][block][0][n];
        for (int m=0; m<FILTER2X_TAPS-1; m++) pbuf[m]                 = pbuf[m+ISR_BLOCK];  // Move the FIR buffer along
        for (int m=0; m<ISR_BLOCK; m++)       pbuf[m+FILTER2X_TAPS-1] = pin[8*m] >> 8;      // Scale down and add new data
        filter2x_b(pbuf+FILTER2X_TAPS-1, &audio_out[n/2][block][0][n%2], ISR_BLOCK, 2);       // Filter and place into 2X buffer
    }        

    isr_exec.time();
}

typedef enum { IN, OUT } dma_dir_t;
int dma_setup(int dma, pio_hw_t *pio, int sm, dma_dir_t dir, int block, int32_t *data, bool interrupt = false)
{
    dma_channel_config c = dma_channel_get_default_config(dma);
    channel_config_set_read_increment(&c,  dir == OUT);
    channel_config_set_write_increment(&c, dir == IN );
    channel_config_set_ring(&c, dir == IN, log2(block*2*sizeof(int32_t)));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, dir == OUT));
    if (dir==OUT) dma_channel_configure(dma, &c, &pio->txf[sm], data, block, false);
    else          dma_channel_configure(dma, &c, data, &pio->rxf[sm], block, false);
    dma_channel_set_irq0_enabled(dma, interrupt);
    return dma;
}


void core1(void) { while(1); };

int main() 
{
    vreg_set_voltage(REG_VOLTAGE);
    stdio_init_all();
    
    uint vco, postdiv1, postdiv2;
    int ret = check_sys_clock_khz(CLK_SYS/1000, &vco, &postdiv1, &postdiv2);
    printf("\n\nCHECKING CLOCK    %10ld %d %d %d %d\n", CLK_SYS, ret, vco, postdiv1, postdiv2);
    sleep_ms(200);

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

    #define I2S_BCLK        2
    #define I2S_LRCLK       3
    #define I2S_DI0         4
    #define I2S_DI1         5
    #define I2S_DI2         6
    #define I2S_DI3         7
    #define I2S_DI4         8
    #define I2S_DI5         9
    #define I2S_DI6        10
    #define I2S_DI7        11
    #define I2S_2X_BCLK    12
    #define I2S_2X_LRCLK   13
    #define I2S_2X_DO0     14
    #define I2S_2X_DO1     15
    #define I2S_2X_DO2     16
    #define I2S_2X_DO3     17


    printf("SETTING UP I2S\n");
    printf("I2S CLOCK DESIRED:          %10d\n", CLK_I2S);
    printf("PIO CLOCK DESIRED:          %10d\n", CLK_PIO);
    printf("PIO CLOCK DIVIDER:        %2d + %3d/256\n", CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    printf("PIO CLOCK ACTUAL:           %10lld\n", (int64_t)(clock_get_hz(clk_sys) / ((float)CLK_PIO_DIV_N + ((float)CLK_PIO_DIV_F / 256.0f))));
    
    // PIO0 is responsible for the input I2S or TDM
    uint offset = pio_add_program (pio0, &tdm_in_program);
    tdm_in_init(pio0, 0, offset, I2S_LRCLK, I2S_DI0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);

    // PIO1 is responsible for the output double rate I2S
    offset = pio_add_program  (pio1, &i2s_double_out_program);
    i2s_double_out_init       (pio1, 0, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    i2s_double_out_init       (pio1, 1, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO1, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    i2s_double_out_init       (pio1, 2, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO2, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    i2s_double_out_init       (pio1, 3, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO3, CLK_PIO_DIV_N, CLK_PIO_DIV_F);

    dma_setup(0, pio1, 0, OUT, 4*ISR_BLOCK,  (int32_t *)audio_out[0],  true);
    dma_setup(1, pio1, 1, OUT, 4*ISR_BLOCK,  (int32_t *)audio_out[1]);
    dma_setup(2, pio1, 2, OUT, 4*ISR_BLOCK,  (int32_t *)audio_out[2]);
    dma_setup(3, pio1, 3, OUT, 4*ISR_BLOCK,  (int32_t *)audio_out[3]);

    dma_setup(4, pio0, 0, IN,  8*ISR_BLOCK, ((int32_t *)audio_tdm[0])+2);                   // The address offset here aligns the TDM channels

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    irq_set_priority(DMA_IRQ_0, 0);                                         // Make this the highest priority

    sleep_ms(200);      // Allow clocks to settle

    dma_start_channel_mask    (        0b11111);   // Start DMA first to make sure that the PIO has data
    pio_enable_sm_mask_in_sync(pio0_hw, 0b0001);
    pio_enable_sm_mask_in_sync(pio1_hw, 0b1111);

    char str[8000];
    int64_t time = isr_call.now();
    while(1)
    {
        gpio_put(LED_PIN, 1);
        sleep_ms(2300);
        gpio_put(LED_PIN, 0);
        sleep_ms(2299);
    
        printf("Time passed %lld\n",isr_call.now()-time);
        time = isr_call.now();
        isr_call.text(20, str);
        printf("%s\n", str);
        isr_exec.text(20, str);
        printf("%s\n\n", str);

    }

}

