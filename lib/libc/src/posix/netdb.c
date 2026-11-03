/* <netdb.h>: names to addresses and back, services, protocols.
 *
 * A host name is looked for in this order: a numeric address, the
 * hosts file, the OS's own resolver where it has one (__os_resolve --
 * EmbLinkOS's), and DNS (resolv.c). "localhost" answers the loopback
 * address even with no hosts file, as every resolver since RFC 6761.
 *
 * The services and protocols databases are /etc/services and
 * /etc/protocols; a system without them still has the well-known ones
 * IANA assigned, which are a table here, not a guess.
 */
#define _GNU_SOURCE 1
#include <netdb.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>

#include "posix.h"
#include "resolv.h"

__attribute__((weak)) int h_errno;

/* ---- addresses found ------------------------------------------------------- */

#define MAXADDR 32

struct found {
    int n;
    int family[MAXADDR];
    unsigned char addr[MAXADDR][16];
    char canon[256];
};

static void found_add(struct found *f, int family, const void *a)
{
    int len = family == AF_INET ? 4 : 16;
    for (int i = 0; i < f->n; i++)
        if (f->family[i] == family && !memcmp(f->addr[i], a, (size_t)len))
            return;
    if (f->n < MAXADDR) {
        f->family[f->n] = family;
        memcpy(f->addr[f->n++], a, (size_t)len);
    }
}

/* ---- the hosts file ---------------------------------------------------------- */

static int want(int asked, int family)
{
    return asked == AF_UNSPEC || asked == family;
}

static void hosts_by_name(const char *name, int family, struct found *f)
{
    FILE *h = fopen("/etc/hosts", "r");
    char line[512];
    while (h && fgets(line, sizeof line, h)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *save, *a = strtok_r(line, " \t\r\n", &save);
        if (!a)
            continue;
        unsigned char bin[16];
        int fam = inet_pton(AF_INET, a, bin) == 1    ? AF_INET
                  : inet_pton(AF_INET6, a, bin) == 1 ? AF_INET6 : 0;
        if (!fam || !want(family, fam))
            continue;
        /* the line's first name is the official one, whichever matched */
        char *official = NULL;
        for (char *n; (n = strtok_r(NULL, " \t\r\n", &save)); ) {
            if (!official)
                official = n;
            if (!strcasecmp(n, name)) {
                if (!f->canon[0])
                    snprintf(f->canon, sizeof f->canon, "%s", official);
                found_add(f, fam, bin);
                break;
            }
        }
    }
    if (h)
        fclose(h);
}

static int hosts_by_addr(int family, const void *a, char *out, size_t n)
{
    FILE *h = fopen("/etc/hosts", "r");
    char line[512];
    int ok = 0;
    while (!ok && h && fgets(line, sizeof line, h)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *save, *s = strtok_r(line, " \t\r\n", &save);
        unsigned char bin[16];
        if (!s || inet_pton(family, s, bin) != 1 ||
            memcmp(bin, a, family == AF_INET ? 4 : 16))
            continue;
        char *name = strtok_r(NULL, " \t\r\n", &save);
        if (name && strlen(name) < n) {
            strcpy(out, name);
            ok = 1;
        }
    }
    if (h)
        fclose(h);
    return ok;
}

/* ---- by DNS ---------------------------------------------------------------- */

struct dnsctx { struct found *f; int family; };

static void take_addr(void *c, const unsigned char *rd, int n,
                      const unsigned char *m, int len)
{
    (void)m; (void)len;
    struct dnsctx *d = c;
    if ((d->family == AF_INET && n == 4) || (d->family == AF_INET6 && n == 16))
        found_add(d->f, d->family, rd);
}

/* 0, or an EAI_ code */
static int dns_by_name(const char *name, int family, struct found *f)
{
    static unsigned char ans[4096];
    int err = 0, any = 0;
    int fams[2] = { AF_INET, AF_INET6 };
    for (int i = 0; i < 2; i++) {
        if (!want(family, fams[i]))
            continue;
        int len = __dns_lookup(name, fams[i] == AF_INET ? DNS_A : DNS_AAAA,
                               ans, sizeof ans);
        if (len < 0) {
            if (errno != ENOENT)
                err = errno == EAGAIN ? EAI_AGAIN : EAI_FAIL;
            continue;
        }
        struct dnsctx d = { f, fams[i] };
        char canon[256] = "";
        if (__dns_records(ans, len, fams[i] == AF_INET ? DNS_A : DNS_AAAA,
                          canon, take_addr, &d) > 0)
            any = 1;
        if (canon[0] && !f->canon[0])
            snprintf(f->canon, sizeof f->canon, "%s", canon);
    }
    return any ? 0 : err ? err : EAI_NONAME;
}

struct ptrctx { char *out; size_t n; int ok; };

static void take_ptr(void *c, const unsigned char *rd, int n,
                     const unsigned char *m, int len)
{
    (void)n;
    struct ptrctx *p = c;
    char name[256];
    if (!p->ok && __dns_name(m, len, (int)(rd - m), name, sizeof name) > 0 &&
        strlen(name) < p->n) {
        strcpy(p->out, name);
        p->ok = 1;
    }
}

static int dns_by_addr(int family, const unsigned char *a, char *out,
                       size_t n)
{
    char q[80];
    char *p = q;
    if (family == AF_INET)
        p += sprintf(p, "%u.%u.%u.%u.in-addr.arpa", a[3], a[2], a[1], a[0]);
    else {
        static const char hex[] = "0123456789abcdef";
        for (int i = 15; i >= 0; i--) {
            *p++ = hex[a[i] & 15];
            *p++ = '.';
            *p++ = hex[a[i] >> 4];
            *p++ = '.';
        }
        strcpy(p, "ip6.arpa.");
    }
    static unsigned char ans[4096];
    int len = __dns_lookup(q, DNS_PTR, ans, sizeof ans);
    if (len < 0)
        return 0;
    struct ptrctx c = { out, n, 0 };
    __dns_records(ans, len, DNS_PTR, NULL, take_ptr, &c);
    return c.ok;
}

/* ---- all of it, in order ----------------------------------------------------- */

static int by_name(const char *name, int family, struct found *f)
{
    unsigned char bin[16];
    if (want(family, AF_INET) && inet_pton(AF_INET, name, bin) == 1) {
        found_add(f, AF_INET, bin);
        return 0;
    }
    if (want(family, AF_INET6) && inet_pton(AF_INET6, name, bin) == 1) {
        found_add(f, AF_INET6, bin);
        return 0;
    }
    hosts_by_name(name, family, f);
    if (f->n)
        return 0;
    if (!strcasecmp(name, "localhost") ||
        (strlen(name) > 10 &&
         !strcasecmp(name + strlen(name) - 10, ".localhost"))) {
        if (want(family, AF_INET))
            found_add(f, AF_INET, (unsigned char[4]){ 127, 0, 0, 1 });
        if (want(family, AF_INET6))
            found_add(f, AF_INET6, in6addr_loopback.s6_addr);
        return 0;
    }
    /* the OS's resolver, where it has one */
    int saved = errno, asked = 0, fams[2] = { AF_INET, AF_INET6 };
    for (int i = 0; i < 2; i++) {
        if (!want(family, fams[i]))
            continue;
        unsigned char got[8][16];
        int k = __os_resolve(name, fams[i], &got[0][0], 8);
        if (k < 0 && errno == ENOSYS)
            break;
        asked = 1;
        for (int j = 0; j < k; j++)
            found_add(f, fams[i], fams[i] == AF_INET
                                      ? &got[0][0] + 4 * j : got[j]);
    }
    errno = saved;
    if (asked)
        return f->n ? 0 : EAI_NONAME;
    return dns_by_name(name, family, f);
}

/* ---- services and protocols ------------------------------------------------ */

static const struct { const char *name; int port; const char *proto; }
well_known[] = {
    { "echo", 7, NULL }, { "discard", 9, NULL }, { "daytime", 13, NULL },
    { "ftp-data", 20, "tcp" }, { "ftp", 21, "tcp" }, { "ssh", 22, NULL },
    { "telnet", 23, "tcp" }, { "smtp", 25, "tcp" }, { "time", 37, NULL },
    { "domain", 53, NULL }, { "bootps", 67, "udp" }, { "bootpc", 68, "udp" },
    { "tftp", 69, "udp" }, { "http", 80, NULL }, { "www", 80, NULL },
    { "pop3", 110, "tcp" }, { "sunrpc", 111, NULL }, { "ntp", 123, "udp" },
    { "imap", 143, "tcp" }, { "snmp", 161, "udp" }, { "ldap", 389, NULL },
    { "https", 443, NULL }, { "submission", 587, "tcp" },
    { "ldaps", 636, "tcp" }, { "imaps", 993, "tcp" }, { "pop3s", 995, "tcp" },
    { "mysql", 3306, "tcp" }, { "postgresql", 5432, "tcp" },
    { "git", 9418, "tcp" },
};

/* the port, host order, or -1 */
static int service_port(const char *name, const char *proto)
{
    FILE *f = fopen("/etc/services", "r");
    char line[512];
    int port = -1;
    while (port < 0 && f && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *save, *n = strtok_r(line, " \t\r\n", &save);
        char *pp = strtok_r(NULL, " \t\r\n", &save);
        if (!n || !pp)
            continue;
        char *slash = strchr(pp, '/');
        if (!slash || (proto && strcmp(slash + 1, proto)))
            continue;
        int match = !strcmp(n, name);
        char *a = strtok_r(NULL, " \t\r\n", &save);
        while (!match && a) {
            match = !strcmp(a, name);
            a = strtok_r(NULL, " \t\r\n", &save);
        }
        if (match)
            port = atoi(pp);
    }
    if (f)
        fclose(f);
    for (size_t i = 0; port < 0 && i < sizeof well_known / sizeof *well_known;
         i++)
        if (!strcmp(well_known[i].name, name) &&
            (!proto || !well_known[i].proto ||
             !strcmp(well_known[i].proto, proto)))
            port = well_known[i].port;
    return port;
}

static const char *service_name(int port, const char *proto)
{
    static char buf[64];
    FILE *f = fopen("/etc/services", "r");
    char line[512];
    const char *r = NULL;
    while (!r && f && fgets(line, sizeof line, f)) {
        char *save, *n = strtok_r(line, " \t\r\n", &save);
        char *pp = strtok_r(NULL, " \t\r\n", &save);
        if (!n || *n == '#' || !pp || atoi(pp) != port)
            continue;
        char *slash = strchr(pp, '/');
        if (slash && (!proto || !strcmp(slash + 1, proto))) {
            snprintf(buf, sizeof buf, "%s", n);
            r = buf;
        }
    }
    if (f)
        fclose(f);
    for (size_t i = 0; !r && i < sizeof well_known / sizeof *well_known; i++)
        if (well_known[i].port == port &&
            (!proto || !well_known[i].proto ||
             !strcmp(well_known[i].proto, proto)))
            r = well_known[i].name;
    return r;
}

WEAK struct servent *getservbyname(const char *name, const char *proto)
{
    static struct servent s;
    static char nbuf[64], pbuf[8];
    static char *aliases[1];
    int port = service_port(name, proto);
    if (port < 0)
        return NULL;
    snprintf(nbuf, sizeof nbuf, "%s", name);
    snprintf(pbuf, sizeof pbuf, "%s", proto ? proto : "tcp");
    s.s_name = nbuf;
    s.s_aliases = aliases;
    s.s_port = htons((uint16_t)port);
    s.s_proto = pbuf;
    return &s;
}

WEAK struct servent *getservbyport(int port, const char *proto)
{
    static struct servent s;
    static char nbuf[64], pbuf[8];
    static char *aliases[1];
    const char *n = service_name(ntohs((uint16_t)port), proto);
    if (!n)
        return NULL;
    snprintf(nbuf, sizeof nbuf, "%s", n);
    snprintf(pbuf, sizeof pbuf, "%s", proto ? proto : "tcp");
    s.s_name = nbuf;
    s.s_aliases = aliases;
    s.s_port = port;
    s.s_proto = pbuf;
    return &s;
}

static const struct { const char *name; int num; } protocols[] = {
    { "ip", 0 }, { "icmp", 1 }, { "igmp", 2 }, { "tcp", 6 }, { "udp", 17 },
    { "ipv6", 41 }, { "gre", 47 }, { "esp", 50 }, { "ah", 51 },
    { "ipv6-icmp", 58 }, { "sctp", 132 }, { "raw", 255 },
};

static struct protoent *proto_ent(int i)
{
    static struct protoent p;
    static char *aliases[1];
    p.p_name = (char *)protocols[i].name;
    p.p_aliases = aliases;
    p.p_proto = protocols[i].num;
    return &p;
}

WEAK struct protoent *getprotobyname(const char *name)
{
    for (size_t i = 0; i < sizeof protocols / sizeof *protocols; i++)
        if (!strcmp(protocols[i].name, name))
            return proto_ent((int)i);
    return NULL;
}

WEAK struct protoent *getprotobynumber(int num)
{
    for (size_t i = 0; i < sizeof protocols / sizeof *protocols; i++)
        if (protocols[i].num == num)
            return proto_ent((int)i);
    return NULL;
}

/* ---- getaddrinfo ------------------------------------------------------------ */

static struct addrinfo *entry(int family, const unsigned char *a, int port,
                              int socktype, int protocol)
{
    struct addrinfo *ai = calloc(1, sizeof *ai + sizeof(struct sockaddr_in6));
    if (!ai)
        return NULL;
    ai->ai_family = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addr = (struct sockaddr *)(ai + 1);
    if (family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)ai->ai_addr;
        s->sin_family = AF_INET;
        s->sin_port = htons((uint16_t)port);
        memcpy(&s->sin_addr, a, 4);
        ai->ai_addrlen = sizeof *s;
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)ai->ai_addr;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons((uint16_t)port);
        memcpy(&s->sin6_addr, a, 16);
        ai->ai_addrlen = sizeof *s;
    }
    return ai;
}

WEAK void freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_canonname);
        free(ai);
        ai = next;
    }
}

#define FLAGS (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED | \
               AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV)

WEAK int getaddrinfo(const char *restrict node, const char *restrict service,
                     const struct addrinfo *restrict hints,
                     struct addrinfo **restrict res)
{
    int flags = hints ? hints->ai_flags : AI_V4MAPPED | AI_ADDRCONFIG;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    if (flags & ~FLAGS)
        return EAI_BADFLAGS;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
        return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM &&
        socktype != SOCK_RAW)
        return EAI_SOCKTYPE;
    if (!node && !service)
        return EAI_NONAME;
    if ((flags & AI_CANONNAME) && !node)
        return EAI_BADFLAGS;

    /* the port, for each socket type asked */
    int types[3], ntypes = 0;
    if (socktype)
        types[ntypes++] = socktype;
    else {
        /* every type, as glibc answers -- raw too, unless a service NAME
         * was asked, which only TCP and UDP have */
        char *end = NULL;
        int numeric = service && *service &&
                      (strtol(service, &end, 10), *end == 0);
        types[ntypes++] = SOCK_STREAM;
        types[ntypes++] = SOCK_DGRAM;
        if (!service || !*service || numeric)
            types[ntypes++] = SOCK_RAW;
    }
    int port[3] = { 0, 0, 0 };
    if (service && *service) {
        char *end;
        long p = strtol(service, &end, 10);
        if (*end == 0 && p >= 0 && p <= 65535) {
            for (int i = 0; i < ntypes; i++)
                port[i] = (int)p;
        } else if (flags & AI_NUMERICSERV) {
            return EAI_NONAME;
        } else {
            int any = 0;
            for (int i = 0; i < ntypes; i++) {
                if (types[i] == SOCK_RAW)
                    return EAI_SERVICE;
                port[i] = service_port(service, types[i] == SOCK_STREAM
                                                    ? "tcp" : "udp");
                any |= port[i] >= 0;
            }
            if (!any)
                return EAI_SERVICE;
        }
    }

    struct found f;
    memset(&f, 0, sizeof f);
    if (!node) {
        int passive = flags & AI_PASSIVE;
        if (want(family, AF_INET))
            found_add(&f, AF_INET, passive ? (unsigned char[4]){ 0, 0, 0, 0 }
                                           : (unsigned char[4]){ 127, 0, 0, 1 });
        if (want(family, AF_INET6))
            found_add(&f, AF_INET6, passive ? in6addr_any.s6_addr
                                            : in6addr_loopback.s6_addr);
    } else if (flags & AI_NUMERICHOST) {
        unsigned char bin[16];
        if (want(family, AF_INET) && inet_pton(AF_INET, node, bin) == 1)
            found_add(&f, AF_INET, bin);
        else if (want(family, AF_INET6) && inet_pton(AF_INET6, node, bin) == 1)
            found_add(&f, AF_INET6, bin);
        else
            return EAI_NONAME;
    } else {
        int search = family == AF_INET6 && (flags & AI_V4MAPPED) ? AF_UNSPEC
                                                                : family;
        int e = by_name(node, search, &f);
        if (e)
            return e;
    }

    /* AI_V4MAPPED: an IPv6 caller with only IPv4 answers gets them as
     * ::ffff:a.b.c.d (all of them as well, with AI_ALL) */
    if (family == AF_INET6 && (flags & AI_V4MAPPED)) {
        int has6 = 0;
        for (int i = 0; i < f.n; i++)
            has6 |= f.family[i] == AF_INET6;
        struct found g;
        memset(&g, 0, sizeof g);
        memcpy(g.canon, f.canon, sizeof g.canon);
        for (int i = 0; i < f.n; i++) {
            if (f.family[i] == AF_INET6)
                found_add(&g, AF_INET6, f.addr[i]);
            else if (!has6 || (flags & AI_ALL)) {
                unsigned char m[16] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
                memcpy(m + 12, f.addr[i], 4);
                found_add(&g, AF_INET6, m);
            }
        }
        f = g;
    }
    if (!f.n)
        return EAI_NONAME;

    struct addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < f.n; i++)
        for (int t = 0; t < ntypes; t++) {
            if (port[t] < 0)
                continue;
            int proto = protocol ? protocol
                        : types[t] == SOCK_STREAM ? IPPROTO_TCP
                        : types[t] == SOCK_DGRAM ? IPPROTO_UDP : 0;
            struct addrinfo *ai = entry(f.family[i], f.addr[i], port[t],
                                        types[t], proto);
            if (!ai) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            *tail = ai;
            tail = &ai->ai_next;
        }
    if (head && (flags & AI_CANONNAME)) {
        head->ai_canonname = strdup(f.canon[0] ? f.canon : node);
        if (!head->ai_canonname) {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }
    }
    *res = head;
    return 0;
}

/* glibc's words */
WEAK const char *gai_strerror(int code)
{
    switch (code) {
    case 0:            return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME:   return "Name or service not known";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE:  return "Servname not supported for ai_socktype";
    case EAI_MEMORY:   return "Memory allocation failure";
    case EAI_SYSTEM:   return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    case EAI_NODATA:      return "No address associated with hostname";
    case EAI_ADDRFAMILY:  return "Address family for hostname not supported";
    case EAI_INPROGRESS:  return "Processing request in progress";
    case EAI_CANCELED:    return "Request canceled";
    case EAI_NOTCANCELED: return "Request not canceled";
    case EAI_ALLDONE:     return "All requests done";
    case EAI_INTR:        return "Interrupted by a signal";
    case EAI_IDN_ENCODE:  return "Parameter string not correctly encoded";
    }
    return "Unknown error";
}

/* ---- getnameinfo ---------------------------------------------------------------- */

WEAK int getnameinfo(const struct sockaddr *restrict sa, socklen_t salen,
                     char *restrict host, socklen_t hostlen,
                     char *restrict serv, socklen_t servlen, int flags)
{
    const unsigned char *a;
    int family = sa->sa_family, port;
    if (family == AF_INET && salen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
        a = (const unsigned char *)&s->sin_addr;
        port = ntohs(s->sin_port);
    } else if (family == AF_INET6 && salen >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
        a = s->sin6_addr.s6_addr;
        port = ntohs(s->sin6_port);
    } else
        return EAI_FAMILY;

    if (host && hostlen) {
        int named = 0;
        if (!(flags & NI_NUMERICHOST))
            named = hosts_by_addr(family, a, host, hostlen) ||
                    dns_by_addr(family, a, host, hostlen);
        if (!named) {
            if (flags & NI_NAMEREQD)
                return EAI_NONAME;
            if (!inet_ntop(family, a, host, hostlen))
                return EAI_OVERFLOW;
        } else if (flags & NI_NOFQDN) {
            char *dot = strchr(host, '.');
            if (dot)
                *dot = 0;
        }
    }
    if (serv && servlen) {
        const char *n = flags & NI_NUMERICSERV
                            ? NULL
                            : service_name(port, flags & NI_DGRAM ? "udp"
                                                                   : "tcp");
        char num[8];
        if (!n) {
            snprintf(num, sizeof num, "%d", port);
            n = num;
        }
        if (strlen(n) + 1 > servlen)
            return EAI_OVERFLOW;
        strcpy(serv, n);
    }
    return 0;
}

/* ---- the old interface ---------------------------------------------------------- */

static struct hostent *fill_hostent(const struct found *f, int family,
                                    const char *name)
{
    static struct hostent h;
    static char hname[256];
    static char *aliases[1];
    static char *list[MAXADDR + 1];
    static unsigned char store[MAXADDR][16];
    int k = 0;
    for (int i = 0; i < f->n; i++)
        if (f->family[i] == family) {
            memcpy(store[k], f->addr[i], 16);
            list[k] = (char *)store[k];
            k++;
        }
    if (!k) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    list[k] = NULL;
    snprintf(hname, sizeof hname, "%s", f->canon[0] ? f->canon : name);
    h.h_name = hname;
    h.h_aliases = aliases;
    h.h_addrtype = family;
    h.h_length = family == AF_INET ? 4 : 16;
    h.h_addr_list = list;
    return &h;
}

WEAK struct hostent *gethostbyname2(const char *name, int family)
{
    struct found f;
    memset(&f, 0, sizeof f);
    int e = by_name(name, family, &f);
    if (e) {
        h_errno = e == EAI_AGAIN ? TRY_AGAIN : e == EAI_FAIL ? NO_RECOVERY
                                                             : HOST_NOT_FOUND;
        return NULL;
    }
    return fill_hostent(&f, family, name);
}

WEAK struct hostent *gethostbyname(const char *name)
{
    return gethostbyname2(name, AF_INET);
}

WEAK struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)
{
    char name[NI_MAXHOST];
    if ((type == AF_INET && len != 4) || (type == AF_INET6 && len != 16) ||
        (type != AF_INET && type != AF_INET6)) {
        h_errno = NO_RECOVERY;
        return NULL;
    }
    if (!hosts_by_addr(type, addr, name, sizeof name) &&
        !dns_by_addr(type, addr, name, sizeof name)) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    struct found f;
    memset(&f, 0, sizeof f);
    found_add(&f, type, addr);
    snprintf(f.canon, sizeof f.canon, "%s", name);
    return fill_hostent(&f, type, name);
}

WEAK const char *hstrerror(int code)
{
    switch (code) {
    case 0:              return "Resolver Error 0 (no error)";
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN:      return "Host name lookup failure";
    case NO_RECOVERY:    return "Unknown server error";
    case NO_DATA:        return "No address associated with name";
    }
    return "Unknown resolver error";
}

WEAK void herror(const char *msg)
{
    if (msg && *msg)
        fprintf(stderr, "%s: %s\n", msg, hstrerror(h_errno));
    else
        fprintf(stderr, "%s\n", hstrerror(h_errno));
}
