////////////////////////////////////////////////////////////////////////////////////////
// Simple code to snoop for Dante multicast streams
//

#include "histogram.hpp"

extern "C" {
#include "port_common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "socket.h"
}


using namespace DAES67;

#define MDNS_TX 2
#define MDNS_RX 3




// Construct a mdns query for services to respond
int mdns_query(const char* name, uint8_t *buf, int len)               
{
    if (len < 18 + strlen(name)) { return 0; };
    
    int n = 0;
    buf[n++] = 0;       // Transaction ID
    buf[n++] = 0;
    buf[n++] = 0x00;    // Standard query
    buf[n++] = 0x00;
    buf[n++] = 0x00;    // Questions
    buf[n++] = 0x01;    // One question
    buf[n++] = 0x00;    // No answer RRs
    buf[n++] = 0x00;
    buf[n++] = 0x00;    // No authority RRs
    buf[n++] = 0x00;
    buf[n++] = 0x00;    // No additional RRs
    buf[n++] = 0x00;

    const char *pos = name;
    while (*pos != '\0') {
        const char *start = pos;
        while (*pos != '.' && *pos != '\0')
            pos++;
        int len = pos - start;
        buf[n++] = len;
        memcpy(buf + n, start, len);
        n += len;
        if (*pos == '.')
            pos++;
    }
    buf[n++] = 0;       // End of name
    buf[n++] = 0x00;    // Question: Type (PTR)
    buf[n++] = 0x0C;    
    buf[n++] = 0x00;    // Question: Class (multicast responses)
    buf[n++] = 0x01;

    return n;
}

int mdns_response(uint8_t *buf, int len, char* name)
{
    typedef struct {
        uint16_t transaction_id;
        uint16_t flags;
        uint16_t questions;
        uint16_t answer_rrs;
        uint16_t authority_rrs;
        uint16_t additional_rrs;
    } dns_header_t;
    dns_header_t *header = (dns_header_t *)buf;

    if (header->transaction_id != 0) return 0;
    if (header->flags != 0x0084)     return 0;
    if (header->questions != 0)      return 0;
    if (header->answer_rrs == 0)     return 0;

    if (memcmp(buf+12, "\x0d\x5f\x6e\x65\x74\x61\x75\x64\x69\x6f\x2d\x61\x72\x63",14)) return 0;

    uint8_t n = buf[48];
    if (n+1>len-48) return 0;
    memcpy(name, buf+49, n);
    name[n] = 0;
    return n;
}


struct DanteDevice {
    char name[256];
    uint8_t ip[4];
    uint8_t mcast_ip[4];
    uint16_t mcast_port;
};
DanteDevice dante_devices[64];


// This mod to allow the use of the polled burst spi_read and spi_write gives a 2X
// boost in the SPI bandwidth and potential data rates to and from network.
static void wizchip_read_burst(uint8_t *pBuf, uint16_t len)
{
    uint8_t tx_data = 0xFF;
    spi_read_blocking(SPI_PORT, tx_data, pBuf, len);
}

static void wizchip_write_burst(uint8_t *pBuf, uint16_t len)
{
    spi_write_blocking(SPI_PORT, pBuf, len);
}


void dante_test(void)
{
    wizchip_spi_initialize();           // NOTE MAKE SURE TO PATCH THIS TO BE 36Mhz not 5Mhz SPI
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    
    // Enable the burst read and write functions
    WIZCHIP.IF.SPI._read_burst   = wizchip_read_burst;
    WIZCHIP.IF.SPI._write_burst  = wizchip_write_burst;

    static wiz_NetInfo net_info = { .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, 
                                    .ip = {10, 0, 0, 99}, 
                                    .sn = {255, 255, 0, 0}, .gw = {10, 0, 0, 1}, .dns { 1, 1, 1, 1 },
                                    .dhcp = NETINFO_STATIC };
    network_initialize(net_info);
    ctlnetwork(CN_GET_NETINFO, (void *)&net_info);
    printf("IP ADDRESS        %d.%d.%d.%d\n", net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    printf("SPI BAUDRATE                %10d\n\n",   spi_get_baudrate(spi0));


    // First send the MDNS query to get all Dante devices to respond
    uint8_t packet[2048];
    uint8_t multicast_ip[4] = {224,0,0,251};
    uint8_t multicast_mac[6] = {0x01, 0x00, 0x5E, 0, 0, 251};
    setSn_MR(MDNS_TX, Sn_MR_UDP);
    setSn_DHAR(MDNS_TX, multicast_mac);	
    setSn_DIPR(MDNS_TX, multicast_ip);
    setSn_DPORT(MDNS_TX, 5353);
    socket(MDNS_TX, Sn_MR_UDP, 5353, Sn_MR_MULTI | SF_IO_NONBLOCK);
    int len = mdns_query("_netaudio-arc._udp.local", packet, sizeof(packet));
    sendto(MDNS_TX, packet, len, multicast_ip, 5353);

    // Now listen for responses
    int n = 0;
    int64_t start = time_us_64();
    do
    {
        uint16_t port = 5353;
        uint8_t  ip[4] = {224,0,0,251};
        char name[256];
        int len = recvfrom(MDNS_TX, packet, sizeof(packet), ip, &port);
        if (len>0)
        {
            if (mdns_response(packet, len, name))
            {
                strcpy(dante_devices[n].name, name);
                memcpy(dante_devices[n].ip, ip, sizeof(ip));
                n++;
            }
        }
    } while((time_us_64()-start)<250000 && n<64);

    for (int i=0; i<n; i++)
    {
       printf("FOUND %02d %-20s at %d.%d.%d.%d\n", i+1, 
            dante_devices[i].name, dante_devices[i].ip[0], dante_devices[i].ip[1], dante_devices[i].ip[2], dante_devices[i].ip[3]);
    }

    close(MDNS_TX);
    printf("\n\n");

    // Now we have a list of devices, query each one to see if it has a multicast stream
    for (int i=0; i<n; i++)
    {
        uint16_t port;
        uint8_t  ip[4];
        uint8_t query[] = { 0x27, 0x29, 0x00, 0x10, 0x09, 0x35, 0x22, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 };
        socket(MDNS_RX, Sn_MR_UDP, 1000, SF_IO_NONBLOCK);
        printf("Querying %s\n", dante_devices[i].name);
        sendto(MDNS_RX, query, sizeof(query), dante_devices[i].ip, 4440);
        sleep_ms(20);
        int len = recvfrom(MDNS_RX, packet, sizeof(packet), ip, &port);
        char *p = (char *)packet;
        if (len>0)
        {
            while(len-- && *p++ != 239);
            if (len>0)
            {
                if (*p==255)
                {
                    printf("FOUND MULTICAST %s at %d.%d.%d.%d\n", dante_devices[i].name, 239, 255, p[1], p[2]);
                    dante_devices[i].mcast_ip[0] = 239;
                    dante_devices[i].mcast_ip[1] = 255;
                    dante_devices[i].mcast_ip[2] = p[1];
                    dante_devices[i].mcast_ip[3] = p[2];
                    dante_devices[i].mcast_port = (p[-3]<<8) + p[-2];
                }
            }
        }
        close(MDNS_RX);
    }

}
