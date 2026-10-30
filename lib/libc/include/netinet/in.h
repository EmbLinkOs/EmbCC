/* <netinet/in.h> -- Internet addresses (src/posix/inet.c) */
#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr { in_addr_t s_addr; };

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct in6_addr {
    union {
        uint8_t __u8[16];
        uint16_t __u16[8];
        uint32_t __u32[4];
    } __in6_u;
};
#define s6_addr   __in6_u.__u8
#define s6_addr16 __in6_u.__u16
#define s6_addr32 __in6_u.__u32

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct ip_mreq { struct in_addr imr_multiaddr, imr_interface; };
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; unsigned ipv6mr_interface; };

extern const struct in6_addr in6addr_any, in6addr_loopback;
#define IN6ADDR_ANY_INIT      { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }

#define IPPROTO_IP     0
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_IPV6   41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW    255

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

#define IP_TOS             1
#define IP_TTL             2
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define IPV6_V6ONLY         26

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (!(a)->s6_addr32[0] && !(a)->s6_addr32[1] && !(a)->s6_addr32[2] && \
     !(a)->s6_addr32[3])
#define IN6_IS_ADDR_LOOPBACK(a) \
    (!(a)->s6_addr32[0] && !(a)->s6_addr32[1] && !(a)->s6_addr32[2] && \
     (a)->s6_addr[12] == 0 && (a)->s6_addr[13] == 0 && \
     (a)->s6_addr[14] == 0 && (a)->s6_addr[15] == 1)
#define IN6_IS_ADDR_MULTICAST(a) ((a)->s6_addr[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a) \
    ((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a) \
    ((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0xc0)
#define IN6_IS_ADDR_V4MAPPED(a) \
    (!(a)->s6_addr32[0] && !(a)->s6_addr32[1] && \
     (a)->s6_addr[8] == 0 && (a)->s6_addr[9] == 0 && \
     (a)->s6_addr[10] == 0xff && (a)->s6_addr[11] == 0xff)
#define IN6_IS_ADDR_V4COMPAT(a) \
    (!(a)->s6_addr32[0] && !(a)->s6_addr32[1] && !(a)->s6_addr32[2] && \
     !IN6_IS_ADDR_UNSPECIFIED(a) && !IN6_IS_ADDR_LOOPBACK(a))

uint32_t htonl(uint32_t v);
uint16_t htons(uint16_t v);
uint32_t ntohl(uint32_t v);
uint16_t ntohs(uint16_t v);

/* as the compiler's byte swaps, so a constant converts at compile time */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define htonl(v) __builtin_bswap32(v)
#define htons(v) __builtin_bswap16(v)
#else
#define htonl(v) ((uint32_t)(v))
#define htons(v) ((uint16_t)(v))
#endif
#define ntohl(v) htonl(v)
#define ntohs(v) htons(v)

#ifdef __cplusplus
}
#endif

#endif
