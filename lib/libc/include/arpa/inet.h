/* <arpa/inet.h> -- Internet addresses as text (src/posix/inet.c) */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t   inet_addr(const char *s);
int         inet_aton(const char *s, struct in_addr *out);
char       *inet_ntoa(struct in_addr a);
const char *inet_ntop(int family, const void *__restrict src,
                      char *__restrict dst, socklen_t n);
int         inet_pton(int family, const char *__restrict src,
                      void *__restrict dst);
in_addr_t   inet_network(const char *s);
in_addr_t   inet_lnaof(struct in_addr in);
in_addr_t   inet_netof(struct in_addr in);
struct in_addr inet_makeaddr(in_addr_t net, in_addr_t host);

#ifdef __cplusplus
}
#endif

#endif
