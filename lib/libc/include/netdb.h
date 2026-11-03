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
#define NETDB_INTERNAL (-1)   /* see errno */
#define NETDB_SUCCESS  0

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

/* the databases one entry at a time (src/posix/netdb2.c) */
struct netent {
    char *n_name;
    char **n_aliases;
    int n_addrtype;
    uint32_t n_net;            /* host order */
};
void             sethostent(int stayopen);
struct hostent  *gethostent(void);
void             endhostent(void);
void             setservent(int stayopen);
struct servent  *getservent(void);
void             endservent(void);
void             setprotoent(int stayopen);
struct protoent *getprotoent(void);
void             endprotoent(void);
void             setnetent(int stayopen);
struct netent   *getnetent(void);
void             endnetent(void);
struct netent   *getnetbyname(const char *name);
struct netent   *getnetbyaddr(uint32_t net, int type);

#if defined(__EMB_GNU)
struct hostent  *gethostbyname2(const char *name, int family);
const char      *hstrerror(int code);
void             herror(const char *msg);
int gethostbyname_r(const char *__restrict name, struct hostent *__restrict out,
                    char *__restrict buf, size_t len,
                    struct hostent **__restrict res, int *__restrict herr);
int gethostbyname2_r(const char *__restrict name, int family,
                     struct hostent *__restrict out, char *__restrict buf,
                     size_t len, struct hostent **__restrict res,
                     int *__restrict herr);
int gethostbyaddr_r(const void *__restrict addr, socklen_t alen, int type,
                    struct hostent *__restrict out, char *__restrict buf,
                    size_t len, struct hostent **__restrict res,
                    int *__restrict herr);
int gethostent_r(struct hostent *__restrict out, char *__restrict buf,
                 size_t len, struct hostent **__restrict res,
                 int *__restrict herr);
int getservbyname_r(const char *__restrict name, const char *__restrict proto,
                    struct servent *__restrict out, char *__restrict buf,
                    size_t len, struct servent **__restrict res);
int getservbyport_r(int port, const char *__restrict proto,
                    struct servent *__restrict out, char *__restrict buf,
                    size_t len, struct servent **__restrict res);
int getservent_r(struct servent *__restrict out, char *__restrict buf,
                 size_t len, struct servent **__restrict res);
int getprotobyname_r(const char *__restrict name, struct protoent *__restrict out,
                     char *__restrict buf, size_t len,
                     struct protoent **__restrict res);
int getprotobynumber_r(int num, struct protoent *__restrict out,
                       char *__restrict buf, size_t len,
                       struct protoent **__restrict res);
int getprotoent_r(struct protoent *__restrict out, char *__restrict buf,
                  size_t len, struct protoent **__restrict res);

/* GNU's codes besides POSIX's, and getaddrinfo_a's */
#define EAI_NODATA      (-5)
#define EAI_ADDRFAMILY  (-9)
#define EAI_INPROGRESS  (-100)
#define EAI_CANCELED    (-101)
#define EAI_NOTCANCELED (-102)
#define EAI_ALLDONE     (-103)
#define EAI_INTR        (-104)
#define EAI_IDN_ENCODE  (-105)

/* getaddrinfo_a's request (src/posix/gai_a.c): the getaddrinfo
 * arguments, and its result when gai_error says it is done */
struct gaicb {
    const char *ar_name;
    const char *ar_service;
    const struct addrinfo *ar_request;
    struct addrinfo *ar_result;
    int __return;
    int __state;
    int __glibc_reserved[4];
};

#define GAI_WAIT   0
#define GAI_NOWAIT 1

struct sigevent;
struct timespec;
int getaddrinfo_a(int mode, struct gaicb *list[], int ent, struct sigevent *sig);
int gai_suspend(const struct gaicb *const list[], int ent, const struct timespec *timeout);
int gai_error(struct gaicb *req);
int gai_cancel(struct gaicb *req);
#endif

#ifdef __cplusplus
}
#endif

#endif
