/* <netdb.h>'s databases read one entry at a time (sethostent, gethostent,
 * endhostent and the services', protocols' and networks' sets), and the
 * reentrant lookups (gethostbyname_r and the rest).
 *
 * The files are /etc/hosts, /etc/services, /etc/protocols and
 * /etc/networks; a system without one has no entries to enumerate. A
 * reentrant lookup asks netdb.c's under a lock and copies the answer
 * into the caller's buffer, ERANGE when it does not fit. */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "posix.h"
#include "../internal/lock.h"

#define MAXFIELDS 36

/* the next line's fields, comments and blank lines skipped: n of them */
static int next_fields(FILE *f, char *line, size_t len, char **field)
{
    while (fgets(line, (int)len, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        int n = 0;
        for (char *p = strtok(line, " \t\r\n"); p && n < MAXFIELDS;
             p = strtok(0, " \t\r\n"))
            field[n++] = p;
        if (n)
            return n;
    }
    return 0;
}

/* ---- one database, one entry at a time ----------------------------------------- */

struct db {
    const char *path;
    FILE *f;
    int stay;                  /* set*ent's stayopen: by-name calls keep it */
    char line[1024];
    char *field[MAXFIELDS + 1];
};

static int db_open(struct db *d)
{
    if (!d->f)
        d->f = fopen(d->path, "re");
    return d->f != 0;
}

static void db_set(struct db *d, int stay)
{
    d->stay = stay;
    if (d->f)
        rewind(d->f);
}

static void db_end(struct db *d)
{
    if (d->f)
        fclose(d->f);
    d->f = 0;
    d->stay = 0;
}

/* aliases from field[from] on, NULL-terminated in place */
static char **aliases(struct db *d, int n, int from)
{
    char **a = d->field + from;
    if (from > n)
        a = d->field + n;
    a[n - from > 0 ? n - from : 0] = 0;
    return a;
}

/* ---- hosts --------------------------------------------------------------------- */

static struct db g_hosts = { "/etc/hosts" };
static struct hostent g_hent;
static unsigned char g_haddr[16];
static char *g_hlist[2];

WEAK void sethostent(int stay) { db_set(&g_hosts, stay); }
WEAK void endhostent(void) { db_end(&g_hosts); }

WEAK struct hostent *gethostent(void)
{
    if (!db_open(&g_hosts))
        return 0;
    for (;;) {
        int n = next_fields(g_hosts.f, g_hosts.line, sizeof g_hosts.line,
                            g_hosts.field);
        if (n < 2)
            if (!n) return 0; else continue;
        /* IPv4 entries, as glibc's gethostent gives them without
         * RES_USE_INET6: an IPv6 line is skipped, except the loopback,
         * which is IPv4's loopback there */
        int family = AF_INET;
        if (strchr(g_hosts.field[0], ':')) {
            unsigned char a6[16];
            static const unsigned char lo6[16] = { [15] = 1 };
            if (inet_pton(AF_INET6, g_hosts.field[0], a6) != 1 ||
                memcmp(a6, lo6, 16))
                continue;
            inet_pton(AF_INET, "127.0.0.1", g_haddr);
        } else if (inet_pton(AF_INET, g_hosts.field[0], g_haddr) != 1) {
            continue;
        }
        g_hent.h_name = g_hosts.field[1];
        g_hent.h_aliases = aliases(&g_hosts, n, 2);
        g_hent.h_addrtype = family;
        g_hent.h_length = family == AF_INET ? 4 : 16;
        g_hlist[0] = (char *)g_haddr;
        g_hlist[1] = 0;
        g_hent.h_addr_list = g_hlist;
        return &g_hent;
    }
}

/* ---- services ------------------------------------------------------------------ */

static struct db g_serv = { "/etc/services" };
static struct servent g_sent;

WEAK void setservent(int stay) { db_set(&g_serv, stay); }
WEAK void endservent(void) { db_end(&g_serv); }

WEAK struct servent *getservent(void)
{
    if (!db_open(&g_serv))
        return 0;
    for (;;) {
        int n = next_fields(g_serv.f, g_serv.line, sizeof g_serv.line,
                            g_serv.field);
        if (!n)
            return 0;
        char *slash = n >= 2 ? strchr(g_serv.field[1], '/') : 0;
        if (!slash)
            continue;
        *slash = 0;
        g_sent.s_name = g_serv.field[0];
        g_sent.s_port = (int)htons((uint16_t)atoi(g_serv.field[1]));
        g_sent.s_proto = slash + 1;
        g_sent.s_aliases = aliases(&g_serv, n, 2);
        return &g_sent;
    }
}

/* ---- protocols ----------------------------------------------------------------- */

static struct db g_proto = { "/etc/protocols" };
static struct protoent g_pent;

WEAK void setprotoent(int stay) { db_set(&g_proto, stay); }
WEAK void endprotoent(void) { db_end(&g_proto); }

WEAK struct protoent *getprotoent(void)
{
    if (!db_open(&g_proto))
        return 0;
    for (;;) {
        int n = next_fields(g_proto.f, g_proto.line, sizeof g_proto.line,
                            g_proto.field);
        if (!n)
            return 0;
        if (n < 2)
            continue;
        g_pent.p_name = g_proto.field[0];
        g_pent.p_proto = atoi(g_proto.field[1]);
        g_pent.p_aliases = aliases(&g_proto, n, 2);
        return &g_pent;
    }
}

/* ---- networks ------------------------------------------------------------------ */

static struct db g_net = { "/etc/networks" };
static struct netent g_nent;

WEAK void setnetent(int stay) { db_set(&g_net, stay); }
WEAK void endnetent(void) { db_end(&g_net); }

WEAK struct netent *getnetent(void)
{
    if (!db_open(&g_net))
        return 0;
    for (;;) {
        int n = next_fields(g_net.f, g_net.line, sizeof g_net.line, g_net.field);
        if (!n)
            return 0;
        if (n < 2)
            continue;
        uint32_t net = inet_network(g_net.field[1]);
        if (net == INADDR_NONE)
            continue;
        g_nent.n_name = g_net.field[0];
        g_nent.n_aliases = aliases(&g_net, n, 2);
        g_nent.n_addrtype = AF_INET;
        g_nent.n_net = net;
        return &g_nent;
    }
}

WEAK struct netent *getnetbyname(const char *name)
{
    int stay = g_net.stay;
    setnetent(1);
    struct netent *e;
    while ((e = getnetent())) {
        if (!strcmp(e->n_name, name))
            break;
        char **a = e->n_aliases;
        while (*a && strcmp(*a, name))
            a++;
        if (*a)
            break;
    }
    if (!stay)
        endnetent();
    return e;
}

WEAK struct netent *getnetbyaddr(uint32_t net, int type)
{
    int stay = g_net.stay;
    setnetent(1);
    struct netent *e;
    while ((e = getnetent()))
        if (e->n_addrtype == type && e->n_net == net)
            break;
    if (!stay)
        endnetent();
    return e;
}

/* ---- the reentrant lookups ----------------------------------------------------- */

static __lock_t g_lock = LOCK_INIT;

/* bytes of buf, aligned for a pointer, or NULL when they do not fit */
struct carve { char *p; size_t left; };

static void *take(struct carve *c, size_t n, size_t align)
{
    uintptr_t at = ((uintptr_t)c->p + align - 1) & ~(uintptr_t)(align - 1);
    size_t pad = (size_t)(at - (uintptr_t)c->p);
    if (pad + n > c->left)
        return 0;
    c->p += pad + n;
    c->left -= pad + n;
    return (void *)at;
}

static char *take_str(struct carve *c, const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = take(c, n, 1);
    return d ? memcpy(d, s, n) : 0;
}

/* a NULL-terminated vector of strings, or of n-byte addresses (n > 0) */
static char **take_vec(struct carve *c, char *const *v, size_t n)
{
    size_t k = 0;
    while (v && v[k])
        k++;
    char **d = take(c, (k + 1) * sizeof *d, sizeof(void *));
    if (!d)
        return 0;
    for (size_t i = 0; i < k; i++) {
        d[i] = n ? take(c, n, 8) : take_str(c, v[i]);
        if (!d[i])
            return 0;
        if (n)
            memcpy(d[i], v[i], n);
    }
    d[k] = 0;
    return d;
}

static int copy_host(const struct hostent *h, struct hostent *out, char *buf,
                     size_t len)
{
    struct carve c = { buf, len };
    out->h_addrtype = h->h_addrtype;
    out->h_length = h->h_length;
    if (!(out->h_name = take_str(&c, h->h_name)) ||
        !(out->h_aliases = take_vec(&c, h->h_aliases, 0)) ||
        !(out->h_addr_list = take_vec(&c, h->h_addr_list, (size_t)h->h_length)))
        return ERANGE;
    return 0;
}

/* glibc's convention: 0 with *res NULL for "not found", an errno value
 * (ERANGE) for a buffer too small, *herr the resolver's code */
static int host_r(struct hostent *h, struct hostent *out, char *buf,
                  size_t len, struct hostent **res, int *herr)
{
    *res = 0;
    if (!h) {
        if (herr)
            *herr = h_errno;
        __unlock(&g_lock);
        return h_errno == NETDB_INTERNAL ? errno : 0;
    }
    int e = copy_host(h, out, buf, len);
    __unlock(&g_lock);
    if (e) {
        if (herr)
            *herr = NETDB_INTERNAL;
        return e;
    }
    *res = out;
    return 0;
}

WEAK int gethostbyname2_r(const char *name, int family, struct hostent *out,
                          char *buf, size_t len, struct hostent **res, int *herr)
{
    __lock(&g_lock);
    return host_r(gethostbyname2(name, family), out, buf, len, res, herr);
}

WEAK int gethostbyname_r(const char *name, struct hostent *out, char *buf,
                         size_t len, struct hostent **res, int *herr)
{
    __lock(&g_lock);
    return host_r(gethostbyname(name), out, buf, len, res, herr);
}

WEAK int gethostbyaddr_r(const void *addr, socklen_t alen, int type,
                         struct hostent *out, char *buf, size_t len,
                         struct hostent **res, int *herr)
{
    __lock(&g_lock);
    return host_r(gethostbyaddr(addr, alen, type), out, buf, len, res, herr);
}

WEAK int gethostent_r(struct hostent *out, char *buf, size_t len,
                      struct hostent **res, int *herr)
{
    __lock(&g_lock);
    struct hostent *h = gethostent();
    if (!h)
        h_errno = HOST_NOT_FOUND;
    int r = host_r(h, out, buf, len, res, herr);
    return h ? r : ENOENT;
}

static int serv_r(struct servent *s, struct servent *out, char *buf,
                  size_t len, struct servent **res)
{
    *res = 0;
    int e = 0;
    if (s) {
        struct carve c = { buf, len };
        out->s_port = s->s_port;
        if (!(out->s_name = take_str(&c, s->s_name)) ||
            !(out->s_proto = take_str(&c, s->s_proto)) ||
            !(out->s_aliases = take_vec(&c, s->s_aliases, 0)))
            e = ERANGE;
        else
            *res = out;
    }
    __unlock(&g_lock);
    return e;
}

WEAK int getservbyname_r(const char *name, const char *proto,
                         struct servent *out, char *buf, size_t len,
                         struct servent **res)
{
    __lock(&g_lock);
    return serv_r(getservbyname(name, proto), out, buf, len, res);
}

WEAK int getservbyport_r(int port, const char *proto, struct servent *out,
                         char *buf, size_t len, struct servent **res)
{
    __lock(&g_lock);
    return serv_r(getservbyport(port, proto), out, buf, len, res);
}

WEAK int getservent_r(struct servent *out, char *buf, size_t len,
                      struct servent **res)
{
    __lock(&g_lock);
    struct servent *s = getservent();
    int r = serv_r(s, out, buf, len, res);
    return s ? r : ENOENT;
}

static int proto_r(struct protoent *p, struct protoent *out, char *buf,
                   size_t len, struct protoent **res)
{
    *res = 0;
    int e = 0;
    if (p) {
        struct carve c = { buf, len };
        out->p_proto = p->p_proto;
        if (!(out->p_name = take_str(&c, p->p_name)) ||
            !(out->p_aliases = take_vec(&c, p->p_aliases, 0)))
            e = ERANGE;
        else
            *res = out;
    }
    __unlock(&g_lock);
    return e;
}

WEAK int getprotobyname_r(const char *name, struct protoent *out, char *buf,
                          size_t len, struct protoent **res)
{
    __lock(&g_lock);
    return proto_r(getprotobyname(name), out, buf, len, res);
}

WEAK int getprotobynumber_r(int num, struct protoent *out, char *buf,
                            size_t len, struct protoent **res)
{
    __lock(&g_lock);
    return proto_r(getprotobynumber(num), out, buf, len, res);
}

WEAK int getprotoent_r(struct protoent *out, char *buf, size_t len,
                       struct protoent **res)
{
    __lock(&g_lock);
    struct protoent *p = getprotoent();
    int r = proto_r(p, out, buf, len, res);
    return p ? r : ENOENT;
}
