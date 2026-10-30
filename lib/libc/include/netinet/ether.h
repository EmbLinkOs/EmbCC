/* <netinet/ether.h> -- Ethernet addresses as text, and /etc/ethers
 * (src/posix/misc2.c). */
#ifndef _NETINET_ETHER_H
#define _NETINET_ETHER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif
struct ether_addr { uint8_t ether_addr_octet[ETH_ALEN]; } __attribute__((packed));

char *ether_ntoa(const struct ether_addr *a);
char *ether_ntoa_r(const struct ether_addr *a, char *buf);
struct ether_addr *ether_aton(const char *s);
struct ether_addr *ether_aton_r(const char *s, struct ether_addr *a);
int ether_line(const char *line, struct ether_addr *a, char *host);
int ether_ntohost(char *host, const struct ether_addr *a);
int ether_hostton(const char *host, struct ether_addr *a);

#ifdef __cplusplus
}
#endif

#endif
