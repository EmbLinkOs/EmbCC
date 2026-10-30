/* <ifaddrs.h> -- every interface's addresses (src/posix/ifaddrs.c). */
#ifndef _IFADDRS_H
#define _IFADDRS_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    union {
        struct sockaddr *ifu_broadaddr;
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
    void *ifa_data;
};
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

int  getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);

#ifdef __cplusplus
}
#endif

#endif
