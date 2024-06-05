#include <math.h>
#include <stdio.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "i2s.pio.h"
#include "histogram.hpp"
#include "upsample.h"
#include "deinterleave.h"

#ifndef PICO_DEFAULT_LED_PIN
#warning blink example requires a board with a regular LED
#else
const uint LED_PIN = PICO_DEFAULT_LED_PIN;
#endif


extern "C" {
#include "port_common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "httpServer.h"
}


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


// TODO THIS IS WORKING FOR 16 BUT NOT FOR 8 - Some sort of block addressing issue

#define ISR_BLOCK    4           // Number of samples at 48kHz that we lump into each ISR call

int32_t   audio_i2s[1][2][ISR_BLOCK][2] __attribute__((aligned(2*2*ISR_BLOCK*4))) = { };    // Single line of normal rate I2S
int32_t   audio_tdm[1][2][ISR_BLOCK][8] __attribute__((aligned(2*8*ISR_BLOCK*4))) = { };    // One 8 ch TDM injest
int32_t   audio_out[4][2][ISR_BLOCK][4] __attribute__((aligned(2*4*ISR_BLOCK*4))) = { };    // Outut four lines of double rate I2S
int32_t   audio_int[1][2][ISR_BLOCK][8] __attribute__((aligned(2*8*ISR_BLOCK*4))) = { };    // Interleaved I2S from the i2s_four_in
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

    // Deinterleave data from I2S four pin, into the tdm buffer     // About 2us per LRCLK @300MHz
    for (int n=0; n<ISR_BLOCK; n++)
    {
        uint32_t w0 = deinterleave4(audio_int[0][block][n][0]);
        uint32_t w1 = deinterleave4(audio_int[0][block][n][1]);
        uint32_t w2 = deinterleave4(audio_int[0][block][n][2]);
        uint32_t w3 = deinterleave4(audio_int[0][block][n][3]);

        audio_tdm[0][block][n][0] = ((  w0 & 0xFF000000 )    ) + ((  w1 & 0xFF000000 )>>8 ) + ((  w2 & 0xFF000000 )>>16) + ((  w3 & 0xFF000000 )>>24);
        audio_tdm[0][block][n][2] = ((  w0 & 0x00FF0000 )<<8 ) + ((  w1 & 0x00FF0000 )    ) + ((  w2 & 0x00FF0000 )>>8 ) + ((  w3 & 0x00FF0000 )>>16);
        audio_tdm[0][block][n][4] = ((  w0 & 0x0000FF00 )<<16) + ((  w1 & 0x0000FF00 )<<8 ) + ((  w2 & 0x0000FF00 )    ) + ((  w3 & 0x0000FF00 )>>8);
        audio_tdm[0][block][n][6] = ((  w0 & 0x000000FF )<<24) + ((  w1 & 0x000000FF )<<16) + ((  w2 & 0x000000FF )<<8 ) + ((  w3 & 0x000000FF ));

        w0 = deinterleave4(audio_int[0][block][n][4]);
        w1 = deinterleave4(audio_int[0][block][n][5]);
        w2 = deinterleave4(audio_int[0][block][n][6]);
        w3 = deinterleave4(audio_int[0][block][n][7]);

        audio_tdm[0][block][n][1] = ((  w0 & 0xFF000000 )    ) + ((  w1 & 0xFF000000 )>>8 ) + ((  w2 & 0xFF000000 )>>16) + ((  w3 & 0xFF000000 )>>24);
        audio_tdm[0][block][n][3] = ((  w0 & 0x00FF0000 )<<8 ) + ((  w1 & 0x00FF0000 )    ) + ((  w2 & 0x00FF0000 )>>8 ) + ((  w3 & 0x00FF0000 )>>16);
        audio_tdm[0][block][n][5] = ((  w0 & 0x0000FF00 )<<16) + ((  w1 & 0x0000FF00 )<<8 ) + ((  w2 & 0x0000FF00 )    ) + ((  w3 & 0x0000FF00 )>>8);
        audio_tdm[0][block][n][7] = ((  w0 & 0x000000FF )<<24) + ((  w1 & 0x000000FF )<<16) + ((  w2 & 0x000000FF )<<8 ) + ((  w3 & 0x000000FF ));
    }

    /* Move the single channel I2S data into the TDM buffers
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
    */

    // Move all of the TDM data into the I2S data buffers and filter    // About 6us per LRCLK at @300MHz
    for (int n = 0; n < 8; n++)
    {
        int32_t *pbuf = audio_buf[n];
        int32_t *pin  = &audio_tdm[0][block][0][n];
        for (int m=0; m<FILTER2X_TAPS-1; m++) pbuf[m]                 = pbuf[m+ISR_BLOCK];  // Move the FIR buffer along
        for (int m=0; m<ISR_BLOCK; m++)       pbuf[m+FILTER2X_TAPS-1] = pin[8*m] >> 8;      // Scale down and add new data
        filter2x(pbuf+FILTER2X_TAPS-1, &audio_out[n/2][block][0][n%2], ISR_BLOCK, 2);       // Filter and place into 2X buffer
        //for (int m=0; m<ISR_BLOCK; m++) audio_out[n/2][block][m][n%2] = pin[8*m];
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


#define FLASH_TARGET_OFFSET (1792*1024)                                                         //++ Starting Flash Storage location after 1.8MB ( of the 2MB )
const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);      //++ Pointer pointing at the Flash Address Location

void core1(void);

int main()
{
    set_sys_clock_khz(133000,false);
    stdio_init_all();                                                                           //++ Initialize rp2040
    sleep_ms(10);

    uint32_t flash_data[FLASH_PAGE_SIZE] = {};
    uint32_t foffset = (2044*1024);
    typedef struct 
    {
        uint32_t    magic;
        uint32_t    loads;
    } flash_header_t;
    flash_header_t *local;
    local = (flash_header_t *)flash_data;

    memcpy(flash_data,(const void*)(XIP_BASE + foffset),sizeof(flash_header_t));    
    if (local->magic != 0x12345678) memset(flash_data,0,sizeof(flash_header_t));
    local->magic = 0x12345678;
    local->loads++;
    
    uint32_t interrupts = save_and_disable_interrupts();
//    flash_range_erase(foffset, FLASH_SECTOR_SIZE);
    restore_interrupts(interrupts);   

    sleep_ms(100);

    interrupts = save_and_disable_interrupts();
//    flash_range_program(foffset,(const uint8_t *)flash_data, FLASH_PAGE_SIZE);
    restore_interrupts(interrupts);   


    vreg_set_voltage(REG_VOLTAGE);
    stdio_init_all();
    
    uint vco, postdiv1, postdiv2;
    int ret = check_sys_clock_khz(CLK_SYS/1000, &vco, &postdiv1, &postdiv2);
    printf("\n\nCHECKING CLOCK    %10ld %d %d %d %d\n", CLK_SYS, ret, vco, postdiv1, postdiv2);
    sleep_ms(100);

    set_sys_clock_khz(CLK_SYS/1000, false);
    stdio_init_all();

    sleep_ms(100);

//    multicore_launch_core1(&core1);

    printf("\n\n\n\n");
    printf("BOOT NUMBER                 %10ld\n",local->loads);
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
    //uint offset = pio_add_program (pio0, &i2s_in_program);
    //i2s_in_init(pio0, 0, offset, I2S_LRCLK, I2S_DI0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    //dma_setup  (0, pio0, 0, IN,  2*ISR_BLOCK, (int32_t *)audio_i2s[0],  true);          // Interrupt each time receive block is done

    //uint offset = pio_add_program (pio0, &tdm_in_program);
    //tdm_in_init(pio0, 0, offset, I2S_LRCLK, I2S_DI0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    //dma_setup  (0, pio0, 0, IN,  8*ISR_BLOCK, (int32_t *)audio_tdm[0],  true);          // Interrupt each time receive block is done

    uint offset = pio_add_program (pio0, &i2s_four_in_program);
    i2s_four_in_init(pio0, 0, offset, I2S_LRCLK, I2S_DI0, CLK_PIO_DIV_N, CLK_PIO_DIV_F);
    dma_setup  (0, pio0, 0, IN,  8*ISR_BLOCK, (int32_t *)audio_int[0],  true);          // Interrupt each time receive block is done


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

    

    
    
    static wiz_NetInfo g_net_info =
    {
        .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
        .ip = {10, 0, 0, 99},                     // IP address
        .sn = {255, 255, 0, 0},                    // Subnet Mask
        .gw = {10, 0, 0, 1},                     // Gateway
        .dns = {8, 8, 8, 8},                         // DNS server
        .dhcp = NETINFO_STATIC                       // DHCP enable/disable
    };

#define index_page  "<!DOCTYPE html>"\
                    "<html lang=\"en\">"\
                    "<head>"\
                        "<meta charset=\"UTF-8\">"\
                        "<title>HTTP Server Example</title>"\
                    "</head>"\
                    "<body>"\
                        "<h1>Hello, World!</h1>"\
                    "</body>"\
                    "Well, I'll be damned.  A web server on the other core."\
                    "</html>"


    #define ETHERNET_BUF_MAX_SIZE (1024 * 2)
    #define HTTP_SOCKET_MAX_NUM 1
    static uint8_t g_http_send_buf[ETHERNET_BUF_MAX_SIZE] = {};
    static uint8_t g_http_recv_buf[ETHERNET_BUF_MAX_SIZE] = {};
    static uint8_t g_http_socket_num_list[HTTP_SOCKET_MAX_NUM] = {0};
    wizchip_spi_initialize();
    wizchip_cris_initialize();

    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(g_net_info);

    httpServer_init(g_http_send_buf, g_http_recv_buf, HTTP_SOCKET_MAX_NUM, g_http_socket_num_list);

    //
    print_network_information(g_net_info);          // This will stall waiting for a network

    char web_preamble[] = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><title>HTTP Server Example</title></head><body><h1>STATISTICS</h1><pre>";
    char web_close[]    = "</pre></body></html>"; 

    char page[8000];
    char tmp[3000];
    
    while (1)
    {
        for (int i = 0; i < HTTP_SOCKET_MAX_NUM; i++)
        {
            sprintf(page,"%s\nTime %lld\n",web_preamble,isr_call.now());
            isr_call.text(20, tmp);
            sprintf(page+strlen(page),"%s\n", tmp);
            isr_exec.text(20, tmp);
            sprintf(page+strlen(page),"%s\n\n%s", tmp,web_close);
            reg_httpServer_webContent((unsigned char*)"index.html", (unsigned char*) page);
            httpServer_run(i);
        }
    }

    int64_t time = isr_call.now();
    char str[8000];
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

        httpServer_run(0);

    }
}

