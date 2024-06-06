//////////////////////////////////////////////////////////////////////
// A routine for testing the UDP interface data rate on W5500
//
// To create the stream to send to the device, use these.  Not sure why
// the pps needs to be a bit higher, but these hit the 1ms packet rate.
// These will send about 10,000 packets of the appropriate size and rate.
//
// 2ch AES67 Stream     sudo nice --20 iperf -u -c 10.0.0.99 -p 5000 -b 1100pps -n 2960kB -l 296 --no-udp-fin
// 4ch AES67 Stream     sudo nice --20 iperf -u -c 10.0.0.99 -p 5000 -b 1150pps -n 5840kB -l 584 --no-udp-fin
// 6ch AES67 Stream     sudo nice --20 iperf -u -c 10.0.0.99 -p 5000 -b 1150pps -n 8800kB -l 880 --no-udp-fin
// 8ch AES67 Stream     sudo nice --20 iperf -u -c 10.0.0.99 -p 5000 -b 1150pps -n 11600kB -l 1160 --no-udp-fin
//
// With the RP2040 at 288Mhz and the SPI at 36MHz, we should be able to comfortably to 6ch and maybe 8ch
// Here is the output for 8ch, though it is dropping an occasional packet
//
//
// PACKET TIMES
// 3|                                               _X
// 2|                                               XX     .                             |Packet Times   |
// 5|                                               XX     X                             |N         10000|
// 4|                                               XX     X.                            |mean  0.997E-03|
//  |                                               XX    XXX                            |std   6.066E-05|
//  |                                               XX  xXXXX                            |mode  9.545E-04|
//  |                                              XXXX XXXXX                            |min   8.910E-04|
//  |                                              XXXX XXXXX                            |max   1.137E-03|
//  |                                              XXXX.XXXXX_
//  |                                              XXXXXXXXXXX
//  |                                              XXXXXXXXXXX
//  |                                              XXXXXXXXXXX
//  |                                              XXXXXXXXXXX
//  |                                             XXXXXXXXXXXX
//  |                                             XXXXXXXXXXXX
//  |                                             XXXXXXXXXXXXX
//  |                                             XXXXXXXXXXXXX
// 1|                                            _XXXXXXXXXXXXX
//   -----------------------------------------------------------------------------------------------------
//   0.0                                          0.0010                                            0.0020

// PACKET SIZES
// 1|                                                          X
// 0|                                                          X                         |Packet Size    |
// 0|                                                          X                         |N         10001|
// 0|                                                          X                         |mean  1.160E+03|
// 1|                                                          X                         |std   0.000E+00|
//  |                                                          X                         |mode  1.160E+03|
//  |                                                          X                         |min   1.160E+03|
//  |                                                          X                         |max   1.160E+03|
//  |                                                          X
//  |                                                          X
//  |                                                          X
//  |                                                          X
//  |                                                          X
//  |                                                          X
//  |                                                          X
//  |                                                          X
//  |                                                          X
// 1|                                                          X
//   -----------------------------------------------------------------------------------------------------
//   0.0                                            1000                                              2000
//                                         1000                                              2000
//
// Validated that the W5500 is running at 100Mbs, because if the link is forced to 10FDX from switch, we get this
//
// 
// PACKET TIMES
// 3|                                                X
// 2|                                               XX     _                             |Packet Times   |
// 7|                                               XX     X                             |N         10000|
// 0|                                               XX    .X.                            |mean  0.998E-03|
//  |                                               XX   .XXX                            |std   7.846E-05|
//  |                                               XX   XXXX                            |mode  9.557E-04|
//  |                                              XXXx  XXXX                            |min   7.470E-04|
//  |                                              XXXX _XXXX                            |max   1.850E-03|
//  |                                      .       XXXX XXXXX
//  |                                      X       XXXX_XXXXX.
//  |                                      X       XXXXXXXXXXX
//  |                                      X  _X X XXXXXXXXXXX
//  |                                     .X. XXXXXXXXXXXXXXXXx
//  |                                     XXXXXXXXXXXXXXXXXXXXXx  X
//  |                                     XXXXXXXXXXXXXXXXXXXXXX  X xx  X
//  |                                     XXXXXXXXXXXXXXXXXXXXXX  X XX  X    .                  X
//  |                                     XXXXXXXXXXXXXXXXXXXXXXX_XXXXX XXX__XXX    _           X
// 1|                                     XXXXXXXXXXXXXXXXXXXXXXXXXXXXX_XXXXXXXX   _X_    _   __X__
//   -----------------------------------------------------------------------------------------------------
//   0.0                                          0.0010                                            0.0020



#include "histogram.hpp"

extern "C" {
#include "port_common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "socket.h"
}


using namespace DAES67;

void udp_test(void)
{
    wizchip_spi_initialize();           // NOTE MAKE SURE TO PATCH THIS TO BE 36Mhz not 5Mhz SPI
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    
    static wiz_NetInfo net_info = { .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, 
                                    .ip = {10, 0, 0, 99}, 
                                    .sn = {255, 255, 0, 0}, .gw = {10, 0, 0, 1}, .dns { 1, 1, 1, 1 },
                                    .dhcp = NETINFO_STATIC };
    network_initialize(net_info);
    ctlnetwork(CN_GET_NETINFO, (void *)&net_info);
    printf("IP ADDRESS        %d.%d.%d.%d\n", net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    printf("SPI BAUDRATE                %10d\n\n",   spi_get_baudrate(spi0));

    printf("SOCKET OPEN     %d\n",socket(5, Sn_MR_UDP, 5000, SF_IO_NONBLOCK));
    printf("SOCKET STATUS   %d\n",getSn_SR(5));

    Histogram  Times("Packet Times",0,.002);
    Histogram  Sizes("Packet Size",0,2000);
    uint32_t n=0;
    uint32_t D=0;
    char buf[2048];
    char str[8000];
    int64_t last = Times.now();
    while(1)
    {
        uint8_t addr[4];
        uint16_t port;
        int ret = recvfrom(5, (unsigned char *)buf, sizeof(buf), addr, &port);
        if (ret>0)
        {
            Times.time();
            Sizes.add(ret);
            n++;
            D+=ret;
        }
        if (Times.now() - last > 20000000000)
        {
            last = Times.now();    
            Times.text(20, str);
            printf("PACKET TIMES\n%s\n", str);
            Sizes.text(20, str);
            printf("PACKET SIZES\n%s\n", str);
            Times.reset();
            Sizes.reset();
        }
    }
}