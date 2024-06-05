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
#include "hardware/flash.h"
#include "i2s.pio.h"
#include "histogram.hpp"

using namespace DAES67;

extern "C" {
#include "port_common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "httpServer.h"
}

#define ETHERNET_BUF_MAX_SIZE (1024 * 2)
#define HTTP_SOCKET_MAX_NUM 4

static wiz_NetInfo g_net_info =
    {
        .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
        .ip = {10, 0, 0, 99},                     // IP address
        .sn = {255, 255, 0, 0},                    // Subnet Mask
        .gw = {10, 0, 0, 1},                     // Gateway
        .dns = {8, 8, 8, 8},                         // DNS server
        .dhcp = NETINFO_STATIC                       // DHCP enable/disable
};

static uint8_t g_http_send_buf[ETHERNET_BUF_MAX_SIZE] = {};
static uint8_t g_http_recv_buf[ETHERNET_BUF_MAX_SIZE] = {};
static uint8_t g_http_socket_num_list[HTTP_SOCKET_MAX_NUM] = {0, 1, 2, 3};


extern Histogram   isr_call;
extern Histogram   isr_exec;

void core1(void)
{
    printf("**** CORE1 IS ALIVE  \n");
    sleep_ms(2000);

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
}
