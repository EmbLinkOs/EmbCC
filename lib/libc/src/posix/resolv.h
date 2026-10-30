/* The DNS client netdb.c uses (resolv.c). */
#ifndef EMBLIBC_RESOLV_H
#define EMBLIBC_RESOLV_H

#define DNS_A     1
#define DNS_CNAME 5
#define DNS_PTR   12
#define DNS_AAAA  28

#include <sys/socket.h>

struct __dns_conf {
    struct sockaddr_storage ns[3];
    socklen_t nslen[3];
    int nns;
    char search[6][256];
    int nsearch;
    int ndots, timeout, attempts;
};

/* /etc/resolv.conf, with glibc's defaults for what it leaves out */
void __dns_read_conf(struct __dns_conf *c);
int __dns_exchange(const struct __dns_conf *c, const unsigned char *q, int qlen,
                   unsigned char *ans, int cap, unsigned accept, int usevc);
/* a random message id */
unsigned __dns_id16(void);
int __dns_lookup(const char *name, int type, unsigned char *ans, int cap);
int __dns_records(const unsigned char *m, int len, int type, char *canon,
                  void (*cb)(void *, const unsigned char *, int,
                             const unsigned char *, int),
                  void *ctx);
int __dns_name(const unsigned char *m, int len, int off, char *out, int cap);

#endif
