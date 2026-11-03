/* <resolv.h> -- the resolver's own interface (src/posix/res.c): DNS
 * questions of any type (MX, SRV, TXT -- what getaddrinfo does not
 * ask), the messages built and sent by hand, and the names in them
 * compressed and expanded. BIND 8's API, as glibc has it: the res_n*
 * calls on a state of the caller's, the others on _res. */
#ifndef _RESOLV_H
#define _RESOLV_H

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAXNS            3     /* name servers */
#define MAXDFLSRCH       3
#define MAXDNSRCH        6     /* search domains */
#define LOCALDOMAINPARTS 2
#define RES_TIMEOUT      5     /* seconds, the first try */
#define MAXRESOLVSORT    10
#define RES_MAXNDOTS     15
#define RES_MAXRETRANS   30
#define RES_MAXRETRY     5
#define RES_DFLRETRY     2
#define RES_MAXTIME      65535

struct __res_state {
    int retrans;                       /* seconds to wait for an answer */
    int retry;                         /* times round the servers */
    unsigned long options;             /* RES_* */
    int nscount;
    struct sockaddr_in nsaddr_list[MAXNS];
    unsigned short id;
    char *dnsrch[MAXDNSRCH + 1];       /* the search list, null-ended */
    char defdname[256];                /* its names, back to back */
    unsigned long pfcode;
    unsigned ndots : 4;                /* dots that make a name absolute */
    unsigned nsort : 4;
    unsigned ipv6_unavail : 1;
#if __SIZEOF_INT__ > 2
    unsigned unused : 23;
#else
    unsigned unused : 7;
#endif
    struct {
        struct in_addr addr;
        uint32_t mask;
    } sort_list[MAXRESOLVSORT];
    void *__glibc_unused_qhook;
    void *__glibc_unused_rhook;
    int res_h_errno;                   /* the last h_errno on this state */
    int _vcsock;
    unsigned _flags;
    union {
        char pad[52];
        struct {
            uint16_t nscount;
            uint16_t nsmap[MAXNS];
            int nssocks[MAXNS];
            uint16_t nscount6;
            uint16_t nsinit;
            struct sockaddr_in6 *nsaddrs[MAXNS];  /* an IPv6 server, in place
                                                   * of nsaddr_list's */
            unsigned __glibc_reserved[2];
        } _ext;
    } _u;
};

typedef struct __res_state *res_state;

#define nsaddr nsaddr_list[0]

#define RES_INIT        0x00000001
#define RES_DEBUG       0x00000002
#define RES_AAONLY      0x00000004
#define RES_USEVC       0x00000008     /* TCP, not UDP */
#define RES_PRIMARY     0x00000010
#define RES_IGNTC       0x00000020
#define RES_RECURSE     0x00000040     /* recursion desired */
#define RES_DEFNAMES    0x00000080     /* a name of no dots gets the domain */
#define RES_STAYOPEN    0x00000100
#define RES_DNSRCH      0x00000200     /* the search list */
#define RES_NOALIASES   0x00001000
#define RES_ROTATE      0x00004000
#define RES_NOCHECKNAME 0x00008000
#define RES_KEEPTSIG    0x00010000
#define RES_BLAST       0x00020000
#define RES_USE_EDNS0   0x00100000
#define RES_SNGLKUP     0x00200000
#define RES_SNGLKUPREOP 0x00400000
#define RES_USE_DNSSEC  0x00800000
#define RES_NOTLDQUERY  0x01000000
#define RES_NORELOAD    0x02000000
#define RES_TRUSTAD     0x04000000
#define RES_NOAAAA      0x08000000
#define RES_DEFAULT     (RES_RECURSE | RES_DEFNAMES | RES_DNSRCH)

/* the state the calls without an n use */
struct __res_state *__res_state(void);
#define _res (*__res_state())

int  res_init(void);
int  res_query(const char *dname, int cls, int type, unsigned char *answer, int anslen);
int  res_search(const char *dname, int cls, int type, unsigned char *answer, int anslen);
int  res_querydomain(const char *name, const char *domain, int cls, int type,
                     unsigned char *answer, int anslen);
int  res_mkquery(int op, const char *dname, int cls, int type, const unsigned char *data,
                 int datalen, const unsigned char *newrr, unsigned char *buf, int buflen);
int  res_send(const unsigned char *msg, int msglen, unsigned char *answer, int anslen);
void res_close(void);

int  res_ninit(res_state);
int  res_nquery(res_state, const char *, int, int, unsigned char *, int);
int  res_nsearch(res_state, const char *, int, int, unsigned char *, int);
int  res_nquerydomain(res_state, const char *, const char *, int, int, unsigned char *, int);
int  res_nmkquery(res_state, int, const char *, int, int, const unsigned char *, int,
                  const unsigned char *, unsigned char *, int);
int  res_nsend(res_state, const unsigned char *, int, unsigned char *, int);
void res_nclose(res_state);

int  dn_comp(const char *src, unsigned char *dst, int dstsiz, unsigned char **dnptrs,
             unsigned char **lastdnptr);
int  dn_expand(const unsigned char *msg, const unsigned char *eom, const unsigned char *src,
               char *dst, int dstsiz);
int  dn_skipname(const unsigned char *ptr, const unsigned char *eom);

int  res_hnok(const char *);
int  res_ownok(const char *);
int  res_mailok(const char *);
int  res_dnok(const char *);

#ifdef __cplusplus
}
#endif

#endif
