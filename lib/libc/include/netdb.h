/* <netdb.h> -- names to addresses (src/posix/netdb.c, resolv.c). A
 * name is looked up in /etc/hosts, then by the OS's own resolver where
 * it has one (EmbLinkOS), else by asking the DNS servers /etc/resolv.conf
 * names. */
#ifndef _NETDB_H
#define _NETDB_H

#include <features.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype, h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;                 /* network order */
    char *s_proto;
};

struct protoent {
    char *p_name;
    char **p_aliases;
    int p_proto;
};

struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_V4MAPPED    0x0008
#define AI_ALL         0x0010
#define AI_ADDRCONFIG  0x0020
#define AI_NUMERICSERV 0x0400

#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NOFQDN      4
#define NI_NAMEREQD    8
#define NI_DGRAM       16
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

#define EAI_BADFLAGS (-1)
#define EAI_NONAME   (-2)
#define EAI_AGAIN    (-3)
#define EAI_FAIL     (-4)
#define EAI_FAMILY   (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE  (-8)
#define EAI_MEMORY   (-10)
#define EAI_SYSTEM   (-11)
#define EAI_OVERFLOW (-12)

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA

extern int h_errno;

int              getaddrinfo(const char *__restrict node,
                             const char *__restrict service,
                             const struct addrinfo *__restrict hints,
                             struct addrinfo **__restrict res);
void             freeaddrinfo(struct addrinfo *ai);
const char      *gai_strerror(int code);
int              getnameinfo(const struct sockaddr *__restrict sa,
                             socklen_t salen, char *__restrict host,
                             socklen_t hostlen, char *__restrict serv,
                             socklen_t servlen, int flags);
struct hostent  *gethostbyname(const char *name);
struct hostent  *gethostbyaddr(const void *addr, socklen_t len, int type);
struct servent  *getservbyname(const char *name, const char *proto);
struct servent  *getservbyport(int port, const char *proto);
struct protoent *getprotobyname(const char *name);
struct protoent *getprotobynumber(int proto);
#if defined(__EMB_GNU)
struct hostent  *gethostbyname2(const char *name, int family);
const char      *hstrerror(int code);
void             herror(const char *msg);
#endif

#ifdef __cplusplus
}
#endif

#endif
