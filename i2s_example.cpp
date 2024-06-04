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


static int32_t Table_Deinterleave4[256] = {
   0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101, 0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101,
   0x00000002, 0x00000003, 0x00000102, 0x00000103, 0x00010002, 0x00010003, 0x00010102, 0x00010103, 0x01000002, 0x01000003, 0x01000102, 0x01000103, 0x01010002, 0x01010003, 0x01010102, 0x01010103,
   0x00000200, 0x00000201, 0x00000300, 0x00000301, 0x00010200, 0x00010201, 0x00010300, 0x00010301, 0x01000200, 0x01000201, 0x01000300, 0x01000301, 0x01010200, 0x01010201, 0x01010300, 0x01010301,
   0x00000202, 0x00000203, 0x00000302, 0x00000303, 0x00010202, 0x00010203, 0x00010302, 0x00010303, 0x01000202, 0x01000203, 0x01000302, 0x01000303, 0x01010202, 0x01010203, 0x01010302, 0x01010303,
   0x00020000, 0x00020001, 0x00020100, 0x00020101, 0x00030000, 0x00030001, 0x00030100, 0x00030101, 0x01020000, 0x01020001, 0x01020100, 0x01020101, 0x01030000, 0x01030001, 0x01030100, 0x01030101,
   0x00020002, 0x00020003, 0x00020102, 0x00020103, 0x00030002, 0x00030003, 0x00030102, 0x00030103, 0x01020002, 0x01020003, 0x01020102, 0x01020103, 0x01030002, 0x01030003, 0x01030102, 0x01030103,
   0x00020200, 0x00020201, 0x00020300, 0x00020301, 0x00030200, 0x00030201, 0x00030300, 0x00030301, 0x01020200, 0x01020201, 0x01020300, 0x01020301, 0x01030200, 0x01030201, 0x01030300, 0x01030301,
   0x00020202, 0x00020203, 0x00020302, 0x00020303, 0x00030202, 0x00030203, 0x00030302, 0x00030303, 0x01020202, 0x01020203, 0x01020302, 0x01020303, 0x01030202, 0x01030203, 0x01030302, 0x01030303,
   0x02000000, 0x02000001, 0x02000100, 0x02000101, 0x02010000, 0x02010001, 0x02010100, 0x02010101, 0x03000000, 0x03000001, 0x03000100, 0x03000101, 0x03010000, 0x03010001, 0x03010100, 0x03010101,
   0x02000002, 0x02000003, 0x02000102, 0x02000103, 0x02010002, 0x02010003, 0x02010102, 0x02010103, 0x03000002, 0x03000003, 0x03000102, 0x03000103, 0x03010002, 0x03010003, 0x03010102, 0x03010103,
   0x02000200, 0x02000201, 0x02000300, 0x02000301, 0x02010200, 0x02010201, 0x02010300, 0x02010301, 0x03000200, 0x03000201, 0x03000300, 0x03000301, 0x03010200, 0x03010201, 0x03010300, 0x03010301,
   0x02000202, 0x02000203, 0x02000302, 0x02000303, 0x02010202, 0x02010203, 0x02010302, 0x02010303, 0x03000202, 0x03000203, 0x03000302, 0x03000303, 0x03010202, 0x03010203, 0x03010302, 0x03010303,
   0x02020000, 0x02020001, 0x02020100, 0x02020101, 0x02030000, 0x02030001, 0x02030100, 0x02030101, 0x03020000, 0x03020001, 0x03020100, 0x03020101, 0x03030000, 0x03030001, 0x03030100, 0x03030101,
   0x02020002, 0x02020003, 0x02020102, 0x02020103, 0x02030002, 0x02030003, 0x02030102, 0x02030103, 0x03020002, 0x03020003, 0x03020102, 0x03020103, 0x03030002, 0x03030003, 0x03030102, 0x03030103,
   0x02020200, 0x02020201, 0x02020300, 0x02020301, 0x02030200, 0x02030201, 0x02030300, 0x02030301, 0x03020200, 0x03020201, 0x03020300, 0x03020301, 0x03030200, 0x03030201, 0x03030300, 0x03030301,
   0x02020202, 0x02020203, 0x02020302, 0x02020303, 0x02030202, 0x02030203, 0x02030302, 0x02030303, 0x03020202, 0x03020203, 0x03020302, 0x03020303, 0x03030202, 0x03030203, 0x03030302, 0x03030303 };

uint32_t inline deinterleave4(uint32_t x)
{
    return Table_Deinterleave4[x & 0xFF] | (Table_Deinterleave4[(x >> 8) & 0xFF] << 2) | (Table_Deinterleave4[(x >> 16) & 0xFF] << 4) | (Table_Deinterleave4[(x >> 24) & 0xFF] << 6);
}


// TODO THIS IS WORKING FOR 16 BUT NOT FOR 8 - Some sort of block addressing issue

#define ISR_BLOCK    4           // Number of samples at 48kHz that we lump into each ISR call

int32_t   audio_i2s[4][2][ISR_BLOCK][2] __attribute__((aligned(2*2*ISR_BLOCK*4))) = { };    // Single line of normal rate I2S
int32_t   audio_tdm[1][2][ISR_BLOCK][8] __attribute__((aligned(2*8*ISR_BLOCK*4))) = { };    // One 8 ch TDM injest
int32_t   audio_out[4][2][ISR_BLOCK][4] __attribute__((aligned(2*4*ISR_BLOCK*4))) = { };    // Outut four lines of double rate I2S
//int32_t   audio_int[1][2][ISR_BLOCK][8] __attribute__((aligned(2*8*ISR_BLOCK*4))) = { };    // Interleaved I2S from the i2s_four_in
int32_t   audio_buf[8][ISR_BLOCK+FILTER2X_TAPS-1] = { };                                    // 8 channels of FIR buffer

using namespace DAES67;

Histogram   isr_call("ISR Call Time", 0, 0.0001);
Histogram   isr_exec("ISR Exec Time", 0, 0.0001);


// Called when a full block of data has been written into audio_tdm
static void dma_handler(void) 
{
    dma_hw->ints0 = 1u;                             // No rush for this, and should never re-enter
    int64_t time = isr_call.time();                 // Mark the ISR call time and setup for
    isr_exec.start(time);                           // measuring execution time

    int block = (void *)dma_hw->ch[2].read_addr >= &audio_out[0][1][0][0];       // Determine which double buffer to use

/*
    // Deinterleave data from I2S four pin, into the tdm buffer     // About 1.5us per LRCLK @300MHz
    uint32_t *pin  = (uint32_t *)&audio_int[0][block][0][0];
    uint8_t  *pout = (uint8_t  *)&audio_tdm[0][block][0][0];
    uint32_t word;
    for (int n=0; n<ISR_BLOCK; n++)
    {
        word = deinterleave4(*pin++);
        *(pout    ) = (word    )&0xFF;
        *(pout + 8) = (word>> 8)&0xFF;
        *(pout +16) = (word>>16)&0xFF;
        *(pout +24) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 1) = (word    )&0xFF;
        *(pout + 9) = (word>> 8)&0xFF;
        *(pout +17) = (word>>16)&0xFF;
        *(pout +25) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 2) = (word    )&0xFF;
        *(pout +10) = (word>> 8)&0xFF;
        *(pout +18) = (word>>16)&0xFF;
        *(pout +26) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 3) = (word    )&0xFF;
        *(pout +11) = (word>> 8)&0xFF;
        *(pout +19) = (word>>16)&0xFF;
        *(pout +27) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 4) = (word    )&0xFF;
        *(pout +12) = (word>> 8)&0xFF;
        *(pout +20) = (word>>16)&0xFF;
        *(pout +28) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 5) = (word    )&0xFF;
        *(pout +13) = (word>> 8)&0xFF;
        *(pout +21) = (word>>16)&0xFF;
        *(pout +29) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 6) = (word    )&0xFF;
        *(pout +14) = (word>> 8)&0xFF;
        *(pout +22) = (word>>16)&0xFF;
        *(pout +30) = (word>>24)&0xFF;
        word = deinterleave4(*pin++);
        *(pout + 7) = (word    )&0xFF;
        *(pout +15) = (word>> 8)&0xFF;
        *(pout +23) = (word>>16)&0xFF;
        *(pout +31) = (word>>24)&0xFF;
        pout += 32;
    }
*/


    // Move the I2S data into the TDM buffers
    {
        int32_t *pin  = audio_i2s[0][block][0];
        int32_t *pout = audio_tdm[0][block][0];
        for (int n = 0; n < ISR_BLOCK; n++)
        {
            *pout++ = *pin++;
            *pout++ = *pin++;
            pout+=6;
        }
    }

    // Move all of the TDM data into the I2S data buffers and filter    // About 6us per LRCLK at @300MHz
    for (int n = 0; n < 8; n++)
    {
        int32_t *pbuf = audio_buf[n];
        int32_t *pin  = &audio_tdm[0][block][0][n];
        for (int m=0; m<FILTER2X_TAPS-1; m++) pbuf[m]                 = pbuf[m+ISR_BLOCK];  // Move the FIR buffer along
        for (int m=0; m<ISR_BLOCK; m++)       pbuf[m+FILTER2X_TAPS-1] = pin[8*m] >> 8;      // Scale down and add new data
        filter2x(pbuf+FILTER2X_TAPS-1, &audio_out[n/2][block][0][n%2], ISR_BLOCK, 2);       // Filter and place into 2X buffer
        //for (int m=0; m<ISR_BLOCK; m++) audio_out[n/2][block][0][n%2] = pin[8*m];
    }        
    



    //audio_out[0][0][0][0] = 0xFFFFFFFF;           // Debugging marker
    
    isr_exec.time();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Create a DMA pair to manage a double buffered transfer
//
// Worth some notes here on RP2040
// - It is not possible to self chain DMAs, thus if only using single DMAs per PIO, you need to retrigger
//   in the interrupt
// - For most cases of I2S or TDM, the interrupt does not happen fast enough to miss the first address
//   increment of DMA, so this will skip samples
// - When using chained DMAs, the first data DMA can use a ring, however I expereinced issues with a 
//   ring size of 128, and thus have disabled it.  Leading to use a two word control block.
//
typedef enum { IN, OUT } dma_dir_t;
void dma_setup(int dma, pio_hw_t *pio, int sm, dma_dir_t dir, int block, int32_t *data, bool interrupt = false)
{
    static int32_t __aligned(8) Trigger[12][2];                     // Set of addresses to keep as trigger

    dma_channel_config c = dma_channel_get_default_config(dma);     // First DMA does the data transfer
    channel_config_set_read_increment    (&c,  dir == OUT);
    channel_config_set_write_increment   (&c, dir == IN );
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_dreq              (&c, pio_get_dreq(pio, sm, dir == OUT));
    channel_config_set_chain_to          (&c, dma+1);                       
    if (dir==OUT) dma_channel_configure(dma, &c, &pio->txf[sm], data, block, false);
    else          dma_channel_configure(dma, &c, data, &pio->rxf[sm], block, false);

    Trigger[dma+1][0] = (int32_t)data;                              // The addresses of the double buffer
    Trigger[dma+1][1] = (int32_t)(data + block);

    c = dma_channel_get_default_config   (dma+1);                   // The second DMA does the control block
    channel_config_set_read_increment    (&c, true);                // updating the address after each data
    channel_config_set_write_increment   (&c, false);               // set.  Addresses should be continuous
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);         // and effectice ring of 2 x block
    channel_config_set_ring              (&c, false, 3);
    if (dir==OUT) dma_channel_configure  (dma+1, &c, &dma_hw->ch[dma].al3_read_addr_trig,  Trigger[dma+1], 1, false);
    else          dma_channel_configure  (dma+1, &c, &dma_hw->ch[dma].al2_write_addr_trig, Trigger[dma+1], 1, false);
    dma_channel_set_irq0_enabled(dma, interrupt);
}

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
    uint offset = pio_add_program (pio0, &i2s_in_program);
    i2s_in_init(pio0, 0, offset, I2S_LRCLK, I2S_DI0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    dma_setup  (0, pio0, 0, IN,  2*ISR_BLOCK, (int32_t *)audio_i2s[0],  true);          // Interrupt each time receive block is done

    // PIO1 is responsible for the output double rate I2S
    offset = pio_add_program  (pio1, &i2s_double_out_program);
    i2s_double_out_init       (pio1, 0, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    i2s_double_out_init       (pio1, 1, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO1, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    i2s_double_out_init       (pio1, 2, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO2, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    i2s_double_out_init       (pio1, 3, offset, I2S_BCLK, I2S_2X_BCLK, I2S_2X_DO3, CLK_PIO_DIV_N, CLK_PIO_DIV_F);

    dma_setup(2, pio1, 0, OUT, 4*ISR_BLOCK, (int32_t *)audio_out[0]);                   // Dual data and control DMAs
    dma_setup(4, pio1, 1, OUT, 4*ISR_BLOCK, (int32_t *)audio_out[1]);
    dma_setup(6, pio1, 2, OUT, 4*ISR_BLOCK, (int32_t *)audio_out[2]);
    dma_setup(8, pio1, 3, OUT, 4*ISR_BLOCK, (int32_t *)audio_out[3]);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    irq_set_priority(DMA_IRQ_0, 0);                     // Make this the highest priority
    dma_start_channel_mask(0b1010101010);               // Start all of the data DMAs

    while ( gpio_get(I2S_LRCLK));                       // Wait for LR Clk to be low
    while (!gpio_get(I2S_LRCLK));                       // Wait for a rising edge - machine sync on first fall
    pio_enable_sm_mask_in_sync(pio0_hw, 0b0001);
    pio_enable_sm_mask_in_sync(pio1_hw, 0b1111);   

    printf("BUFFERS  I2S%p  TDM%p  OUT%p  BUF%p\n", audio_i2s, audio_tdm, audio_out, audio_buf);

    uint32_t r1 = dma_hw->ch[2].read_addr - (int32_t)audio_out, w1 = dma_hw->ch[0].write_addr - (int32_t)audio_i2s;   
    sleep_ms(1);
    uint32_t r2 = dma_hw->ch[2].read_addr - (int32_t)audio_out, w2 = dma_hw->ch[0].write_addr - (int32_t)audio_i2s;   
    sleep_ms(1);
    uint32_t r3 = dma_hw->ch[2].read_addr - (int32_t)audio_out, w3 = dma_hw->ch[0].write_addr - (int32_t)audio_i2s;   
    sleep_ms(1);
    uint32_t r4 = dma_hw->ch[2].read_addr - (int32_t)audio_out, w4 = dma_hw->ch[0].write_addr - (int32_t)audio_i2s;   
    printf("DMA ADDRESS  %3ld  %3ld  %ld\n",w1/2/4,r1/4/4,(r1/4/4-w1/2/4));
    printf("DMA ADDRESS  %3ld  %3ld  %ld\n",w2/2/4,r2/4/4,(r2/4/4-w2/2/4));
    printf("DMA ADDRESS  %3ld  %3ld  %ld\n",w3/2/4,r3/4/4,(r3/4/4-w3/2/4));
    printf("DMA ADDRESS  %3ld  %3ld  %ld\n",w4/2/4,r4/4/4,(r4/4/4-w4/2/4));


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

        for (int n=0; n<20; n++)
        {
            uint32_t r = dma_hw->ch[2].read_addr - (int32_t)audio_out, w = dma_hw->ch[0].write_addr - (int32_t)audio_i2s;   
            printf("DMA ADDRESS  %3ld  %3ld  %ld\n",w/2/4,r/4/4,(r/4/4-w/2/4));

        }

    }

}

