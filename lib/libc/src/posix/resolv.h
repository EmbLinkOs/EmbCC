/* The DNS client netdb.c uses (resolv.c). */
#ifndef EMBLIBC_RESOLV_H
#define EMBLIBC_RESOLV_H

#define DNS_A     1
#define DNS_CNAME 5
#define DNS_PTR   12
#define DNS_AAAA  28

int __dns_lookup(const char *name, int type, unsigned char *ans, int cap);
int __dns_records(const unsigned char *m, int len, int type, char *canon,
                  void (*cb)(void *, const unsigned char *, int,
                             const unsigned char *, int),
                  void *ctx);
int __dns_name(const unsigned char *m, int len, int off, char *out, int cap);

#endif
