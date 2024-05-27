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
#include "i2s.h"
#include "pico/stdlib.h"

// I2C defines
// This example uses I2C0 on GPIO4 (SDA) and GPIO5 (SCL) running at 100KHz.
// Connect the codec I2C control to this. (Codec-specific customization is
// not part of this example.)
#define I2C_PORT i2c0
#define I2C_SDA  4
#define I2C_SCL  5

#ifndef PICO_DEFAULT_LED_PIN
#warning blink example requires a board with a regular LED
#else
const uint LED_PIN = PICO_DEFAULT_LED_PIN;
#endif

static __attribute__((aligned(8))) pio_i2s i2s;

static void process_audio(const int32_t* input, int32_t* output, size_t num_frames) {
    // Just copy the input to the output

    static int x;
    for (size_t i = 0; i < num_frames; i++) 
    {
        output[2*i]   = input[2*i];
        output[2*i+1] = input[2*i+1];
        output[2*i]   = 0b00000000000000000000000000000001;
        output[2*i+1] = 0b11111111111111111111111111111110;
    }
}

static void dma_i2s_in_handler(void) {
    /* We're double buffering using chained TCBs. By checking which buffer the
     * DMA is currently reading from, we can identify which buffer it has just
     * finished reading (the completion of which has triggered this interrupt).
     */
    if (*(int32_t**)dma_hw->ch[i2s.dma_ch_in_ctrl].read_addr == i2s.input_buffer) {
        // It is inputting to the second buffer so we can overwrite the first
        process_audio(i2s.input_buffer, i2s.output_buffer, AUDIO_BUFFER_FRAMES);
    } else {
        // It is currently inputting the first buffer, so we write to the second
        process_audio(&i2s.input_buffer[STEREO_BUFFER_SIZE], &i2s.output_buffer[STEREO_BUFFER_SIZE], AUDIO_BUFFER_FRAMES);
    }
    dma_hw->ints0 = 1u << i2s.dma_ch_in_data;  // clear the IRQ
}

int main() {
    // Set a 132.000 MHz system clock to more evenly divide the audio frequencies
    
    //set_sys_clock_pll(1536000000, 6, 2);
    
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(288000, true);
    stdio_init_all();

    uint freq=288000, vco, postdiv1, postdiv2;
    int ret = check_sys_clock_khz(freq, &vco, &postdiv1, &postdiv2);
    printf("Clock details %d %d %d %d %d\n", freq, ret, vco, postdiv1, postdiv2);


    const i2s_config i2s_config_default = {48000, 2048, 32, 10, 6, 7, 8, true};


    printf("System Clock: %lu\n", clock_get_hz(clk_sys));
/*
    uint vco, postdiv1, postdiv2;
    for (int freq=129000; freq<=144000; freq+=1000)
    {
        int ret = check_sys_clock_khz(freq, &vco, &postdiv1, &postdiv2);
        printf("Clock details %d %d %d %d %d\n", freq, ret, vco, postdiv1, postdiv2);
        set_sys_clock_khz(freq, true);
        printf("System Clock: %lu\n", clock_get_hz(clk_sys));
    } 
    sleep_ms(10000);
*/
    // Init GPIO LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // I2C Initialisation. Using it at 100Khz.
    i2c_init(I2C_PORT, 100 * 1000);

    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_set_pulls(I2C_SDA, true, false);
    gpio_set_pulls(I2C_SCL, true, false);
    gpio_set_drive_strength(I2C_SDA, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(I2C_SCL, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(I2C_SDA, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(I2C_SCL, GPIO_SLEW_RATE_FAST);

    // Here, do whatever you need to set up your codec for proper operation.
    // Some codecs require register configuration over I2C, for example.

    // Note: it is usually best to configure the codec here, and then enable it
    //       after starting the I2S clocks, below.

    i2s_program_start_slaved(pio0, &i2s_config_default, dma_i2s_in_handler, &i2s);

    // Enable the (already configured) codec here.

    puts("i2s_example started.");

    // Blink the LED so we know we started everything correctly.


    volatile int32_t X[32] = { };
    int32_t Y1[32] = { 13668795,    15196667,     2130485,    15323904,    10609228,     1636456,     4672425,     9175149,    16064299,    16188143,     2644309,    16283845,    16058597,     8143252,    13426478,     2380458,     7075980,    15363493,    13291033,    16097612,    11001503,      599143,    14246026,    15669806,    11387286,    12712770,
                       12467694,     6580477,    10997094 ,    2872036,    11845488,      534067, };
    int32_t Y2[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 26, 28, 29, 30, 31, 32 };

    while(1)
    {
        for (int32_t n=0; n<400000; n++)
        {
            int32_t  Z1 = X[0] * Y1[0];
            int32_t  Z2 = X[0] * Y2[0]; 
            Z1 += X[1] * Y1[1];
            Z2 += X[1] * Y2[1]; 
            Z1 += X[2] * Y1[2];
            Z2 += X[2] * Y2[2]; 
            Z1 += X[3] * Y1[3];
            Z2 += X[3] * Y2[3]; 
            Z1 += X[4] * Y1[4];
            Z2 += X[4] * Y2[4]; 
            Z1 += X[5] * Y1[5];
            Z2 += X[5] * Y2[5]; 
            Z1 += X[6] * Y1[6];
            Z2 += X[6] * Y2[6]; 
            Z1 += X[7] * Y1[7];
            Z2 += X[7] * Y2[7]; 
            Z1 += X[8] * Y1[8];
            Z2 += X[8] * Y2[8]; 
            Z1 += X[9] * Y1[9];
            Z2 += X[9] * Y2[9]; 
            Z1 += X[10] * Y1[10];
            Z2 += X[10] * Y2[10]; 
            Z1 += X[11] * Y1[11];
            Z2 += X[11] * Y2[11]; 
            Z1 += X[12] * Y1[12];
            Z2 += X[12] * Y2[12]; 
            Z1 += X[13] * Y1[13];
            Z2 += X[13] * Y2[13]; 
            Z1 += X[14] * Y1[14];
            Z2 += X[14] * Y2[14]; 
            Z1 += X[15] * Y1[15];
            Z2 += X[15] * Y2[15]; 
            Z1 += X[16] * Y1[16];
            Z2 += X[16] * Y2[16]; 
            Z1 += X[17] * Y1[17];
            Z2 += X[17] * Y2[17]; 
            Z1 += X[18] * Y1[18];
            Z2 += X[18] * Y2[18]; 
            Z1 += X[19] * Y1[19];
            Z2 += X[19] * Y2[19]; 
            Z1 += X[20] * Y1[20];
            Z2 += X[20] * Y2[20]; 
            Z1 += X[21] * Y1[21];
            Z2 += X[21] * Y2[21]; 
            Z1 += X[22] * Y1[22];
            Z2 += X[22] * Y2[22]; 
            Z1 += X[23] * Y1[23];
            Z2 += X[23] * Y2[23]; 
            Z1 += X[24] * Y1[24];
            Z2 += X[24] * Y2[24]; 
            Z1 += X[25] * Y1[25];
            Z2 += X[25] * Y2[25]; 
            Z1 += X[26] * Y1[26];
            Z2 += X[26] * Y2[26]; 
            Z1 += X[27] * Y1[27];
            Z2 += X[27] * Y2[27]; 
            Z1 += X[28] * Y1[28];
            Z2 += X[28] * Y2[28]; 
            Z1 += X[29] * Y1[29];
            Z2 += X[29] * Y2[29]; 
            Z1 += X[30] * Y1[30];
            Z2 += X[30] * Y2[30]; 
            Z1 += X[31] * Y1[31];
            Z2 += X[31] * Y2[31]; 
            X[0] = Z1;
            X[1] = Z2;
        }
        gpio_put(LED_PIN, 1);
        sleep_ms(50);
        gpio_put(LED_PIN, 0);
    }
    return 0;
}
