/* <netpacket/packet.h> -- Linux's link-level socket addresses, which
 * getifaddrs gives each interface's hardware address in. */
#ifndef _NETPACKET_PACKET_H
#define _NETPACKET_PACKET_H

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_ll {
    unsigned short sll_family;
    unsigned short sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[8];
};

#define PACKET_HOST      0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING  4

#ifdef __cplusplus
}
#endif

#endif
