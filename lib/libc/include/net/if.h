/* <net/if.h> -- network interfaces by name and index (src/posix/ifaddrs.c),
 * and the request structures of the interface ioctls, laid out as
 * Linux's kernel reads them. */
#ifndef _NET_IF_H
#define _NET_IF_H

#include <features.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IF_NAMESIZE 16

struct if_nameindex {
    unsigned if_index;
    char *if_name;
};

unsigned if_nametoindex(const char *name);
char    *if_indextoname(unsigned index, char *name);
struct if_nameindex *if_nameindex(void);
void     if_freenameindex(struct if_nameindex *list);

#if defined(__EMB_GNU)
#define IFNAMSIZ IF_NAMESIZE

#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_DEBUG       0x4
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_NOTRAILERS  0x20
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_ALLMULTI    0x200
#define IFF_MASTER      0x400
#define IFF_SLAVE       0x800
#define IFF_MULTICAST   0x1000
#define IFF_PORTSEL     0x2000
#define IFF_AUTOMEDIA   0x4000
#define IFF_DYNAMIC     0x8000

struct ifmap {
    unsigned long mem_start, mem_end;
    unsigned short base_addr;
    unsigned char irq, dma, port;
};

struct ifreq {
    union { char ifrn_name[IFNAMSIZ]; } ifr_ifrn;
    union {
        struct sockaddr ifru_addr, ifru_dstaddr, ifru_broadaddr, ifru_netmask,
                        ifru_hwaddr;
        short ifru_flags;
        int ifru_ivalue, ifru_mtu;
        struct ifmap ifru_map;
        char ifru_slave[IFNAMSIZ], ifru_newname[IFNAMSIZ];
        char *ifru_data;
    } ifr_ifru;
};
#define ifr_name      ifr_ifrn.ifrn_name
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_metric    ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu
#define ifr_map       ifr_ifru.ifru_map
#define ifr_slave     ifr_ifru.ifru_slave
#define ifr_data      ifr_ifru.ifru_data
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_bandwidth ifr_ifru.ifru_ivalue
#define ifr_qlen      ifr_ifru.ifru_ivalue
#define ifr_newname   ifr_ifru.ifru_newname

struct ifconf {
    int ifc_len;
    union { char *ifcu_buf; struct ifreq *ifcu_req; } ifc_ifcu;
};
#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req
#endif

#ifdef __cplusplus
}
#endif

#endif
