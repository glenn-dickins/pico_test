//////////////////////////////////////////////////////////////////////
// A routine for testing the UDP interface data rate on W5500
//
// TL;DR was able to get about 25Mbps effective from UDP traffic, and
// depending on size handle about 8000pps.  This is more than enough for
// AES67. 
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
//
// The main time is in retrieving the data from the W5500.
// Using wiz_recv_ignore, it was feasible to handle about 7000pps incoming.
// If only taking 48 samples and 2ch data from a packet, it easily handles 4000pps.
// Likely that it will be possible to manage 2x2 flows if done correctly.
//
//
//
// PACKET TIMES
// 2|                X
// 8|                X                                                                   |Packet Times   |
// 0|                XX                                                                  |N         40000|
// 4|                XX                                                                  |mean  3.229E-04|
// 3|               xXX                                                                  |std   1.013E-05|
//  |               XXX                                                                  |mode  3.214E-04|
//  |               XXX                                                                  |min   2.510E-04|
//  |               XXX                                                                  |max   1.611E-03|
//  |               XXX
//  |               XXX
//  |             xXXXX
//  |             XXXXX
//  |             XXXXXx
//  |             XXXXXX
//  |             XXXXXX
//  |            xXXXXXX Xx
//  |            XXXXXXXXXXX
// 1|            XXXXXXXXXXX                                                          _
//   -----------------------------------------------------------------------------------------------------
//   0.0                                          0.0010                                            0.0020

// PACKET SIZES
// 2|                                                          X_
// 4|                                                          XX                        |Packet Size    |
// 0|                                                          XX                        |N         40001|
// 0|                                                          XX                        |mean  1.168E+03|
// 0|                                                          XX                        |std   0.000E+00|
//  |                                                          XX                        |mode  1.165E+03|
//  |                                                          XX                        |min   1.168E+03|
//  |                                                          XX                        |max   1.168E+03|
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
//  |                                                          XX
// 1|                                                          XX
//   -----------------------------------------------------------------------------------------------------
//   0.0                                            1000                                              2000


#include "histogram.hpp"

extern "C" {
#include "port_common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "socket.h"
}


using namespace DAES67;

#define SOCK 5

int check_rsr()
{

    uint8_t req[3] = {0x00, 0x26, ((4*SOCK+1)<<3) + 0};      // Read two bytes from RSR register
    uint16_t val = 0;

//    WIZCHIP_CRITICAL_ENTER();
    WIZCHIP.CS._select();
    spi_write_blocking(SPI_PORT, req,   3);
    spi_read_blocking(SPI_PORT, 0x00, (uint8_t *)&val,2);
    WIZCHIP.CS._deselect();
//    WIZCHIP_CRITICAL_EXIT();

    return ((val&0xFF)<<8) | (val>>8);
}




void udp_test(uint8_t ip[4], int port)
{
    printf("LISTENING TO  %d.%d.%d.%d:%d\n",ip[0],ip[1],ip[2],ip[3],port);
    int sock=0;
    if (0)
    {
        sock = socket(SOCK, Sn_MR_UDP, 5000, SF_IO_NONBLOCK);
    }
    else
    {
        uint8_t multicast_mac[6] = {0x01, 0x00, 0x5E, ip[1], ip[2], ip[3]};
        setSn_MR(SOCK, Sn_MR_UDP);
        setSn_DHAR(SOCK, multicast_mac);	
    	setSn_DIPR(SOCK, ip);
    	setSn_DPORT(SOCK, port);
        sock = socket(SOCK, Sn_MR_UDP, port, SF_IO_NONBLOCK | Sn_MR_MULTI);
    }

    printf("SOCKET OPEN     %d\n",sock);
    printf("SOCKET STATUS   %d\n\n\n",getSn_SR(5));

    Histogram  Times("Packet Times",0,.001);
    Histogram  Sizes("Packet Size",0,1000);
    char buf[2048];
    char    str[8000];
    int64_t last = Times.now();
    while(1)
    {
        uint8_t addr[4];
        uint16_t port;
        int len = check_rsr();
        if (len>100)
        {
            uint16_t ptr = getSn_RX_RD(SOCK);
            uint32_t addrsel = ((uint32_t)ptr << 8) + (WIZCHIP_RXBUF_BLOCK(SOCK) << 3);
            uint8_t req[3] = { addrsel>>16, addrsel>>8, addrsel};

            WIZCHIP.CS._select();
            spi_write_blocking(SPI_PORT, req,   3);
//          spi_read_blocking(SPI_PORT, 0x00, (uint8_t *)buf,16*2*3);       // Simulate getting 2ch of data
            spi_read_blocking(SPI_PORT, 0x00, (uint8_t *)buf,len);       // Get all data
            WIZCHIP.CS._deselect();
            ptr += len;
            setSn_RX_RD(SOCK,ptr);
            setSn_CR(SOCK,Sn_CR_RECV);
            Times.time();
            Sizes.add(len);
        }
        if (Times.now() - last > 20000000000)
        {
            last = Times.now();    
            printf("ELAPSED TIME %10lld us\n",time_us_64());
            Times.text(15, str);
            printf("PACKET TIMES\n%s\n", str);
            Sizes.text(15, str);
            printf("PACKET SIZES\n%s\n", str);
            Times.reset();
            Sizes.reset();
        }
        sleep_us(5);

    }
}