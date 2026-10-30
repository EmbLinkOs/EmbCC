/* A DNS client: the part of name lookup that asks the network, for a
 * system whose kernel has no resolver of its own (Linux; EmbLinkOS has
 * one, and netdb.c asks it first).
 *
 * /etc/resolv.conf as glibc reads it -- nameserver (three at most),
 * search and domain, options ndots: and timeout: and attempts: -- and
 * the queries as RFC 1035 has them: UDP to port 53, recursion desired,
 * an id from the OS's entropy, and the answer accepted only from the
 * server asked, with that id and that question. A truncated answer is
 * asked again over TCP. With no resolv.conf the server is 127.0.0.1,
 * as glibc's default is.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "posix.h"
#include "resolv.h"

struct conf {
    struct sockaddr_storage ns[3];
    socklen_t nslen[3];
    int nns;
    char search[6][256];
    int nsearch;
    int ndots, timeout, attempts;
};

static void add_ns(struct conf *c, const char *s)
{
    if (c->nns == 3)
        return;
    struct sockaddr_in *a4 = (struct sockaddr_in *)&c->ns[c->nns];
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&c->ns[c->nns];
    memset(&c->ns[c->nns], 0, sizeof c->ns[0]);
    if (inet_pton(AF_INET, s, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons(53);
        c->nslen[c->nns++] = sizeof *a4;
    } else if (inet_pton(AF_INET6, s, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(53);
        c->nslen[c->nns++] = sizeof *a6;
    }
}

static void read_conf(struct conf *c)
{
    memset(c, 0, sizeof *c);
    c->ndots = 1;
    c->timeout = 5;
    c->attempts = 2;
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *save, *key = strtok_r(line, " \t\r\n", &save);
        if (!key || *key == '#' || *key == ';')
            continue;
        if (!strcmp(key, "nameserver")) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            if (v)
                add_ns(c, v);
        } else if (!strcmp(key, "search") || !strcmp(key, "domain")) {
            c->nsearch = 0;
            for (char *v; (v = strtok_r(NULL, " \t\r\n", &save)) &&
                          c->nsearch < 6; )
                snprintf(c->search[c->nsearch++], 256, "%s", v);
        } else if (!strcmp(key, "options")) {
            for (char *v; (v = strtok_r(NULL, " \t\r\n", &save)); ) {
                if (!strncmp(v, "ndots:", 6))
                    c->ndots = atoi(v + 6);
                else if (!strncmp(v, "timeout:", 8))
                    c->timeout = atoi(v + 8) > 0 ? atoi(v + 8) : 1;
                else if (!strncmp(v, "attempts:", 9))
                    c->attempts = atoi(v + 9) > 0 ? atoi(v + 9) : 1;
            }
        }
    }
    if (f)
        fclose(f);
    if (!c->nns)
        add_ns(c, "127.0.0.1");
}

/* ---- the wire format ----------------------------------------------------- */

/* name as labels, at q; its length, or -1 if a label or the name is too
 * long */
static int put_name(unsigned char *q, size_t cap, const char *name)
{
    size_t at = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (n == 0 || n > 63 || at + n + 2 > cap)
            return -1;
        q[at++] = (unsigned char)n;
        memcpy(q + at, name, n);
        at += n;
        name += n;
        if (*name == '.')
            name++;
    }
    if (at + 1 > cap || at > 254)
        return -1;
    q[at++] = 0;
    return (int)at;
}

static int build_query(unsigned char *q, size_t cap, const char *name,
                       int type, unsigned id)
{
    if (cap < 12)
        return -1;
    memset(q, 0, 12);
    q[0] = (unsigned char)(id >> 8);
    q[1] = (unsigned char)id;
    q[2] = 0x01;                     /* RD */
    q[5] = 1;                        /* one question */
    int n = put_name(q + 12, cap - 12 - 4, name);
    if (n < 0)
        return -1;
    unsigned char *t = q + 12 + n;
    t[0] = (unsigned char)(type >> 8);
    t[1] = (unsigned char)type;
    t[2] = 0;
    t[3] = 1;                        /* class IN */
    return 12 + n + 4;
}

/* The name at off, decompressed into out; the offset after it in the
 * message, or -1 for a malformed one -- pointers that loop or point
 * past the end included. */
int __dns_name(const unsigned char *m, int len, int off, char *out, int cap)
{
    int at = 0, jumps = 0, next = -1;
    while (off < len) {
        unsigned c = m[off];
        if (c == 0) {
            if (next < 0)
                next = off + 1;
            if (at == 0 && cap > 1)
                out[at++] = '.';
            if (at < cap)
                out[at] = 0;
            return at < cap ? next : -1;
        }
        if ((c & 0xc0) == 0xc0) {
            if (off + 1 >= len || ++jumps > 32)
                return -1;
            if (next < 0)
                next = off + 2;
            off = (int)((c & 0x3f) << 8 | m[off + 1]);
            continue;
        }
        if (c & 0xc0 || off + 1 + (int)c > len)
            return -1;
        if (at && at < cap)
            out[at++] = '.';
        if (at + (int)c >= cap)
            return -1;
        memcpy(out + at, m + off + 1, c);
        at += (int)c;
        off += 1 + (int)c;
    }
    return -1;
}

static unsigned id16(void)
{
    unsigned short r;
    if (__os_getentropy(&r, sizeof r) == 0)
        return r;
    return (unsigned)(__os_clock_ns() ^ (long)__os_time()) & 0xffff;
}

/* ---- asking ----------------------------------------------------------------- */

static int ask_tcp(const struct sockaddr *ns, socklen_t nslen,
                   const unsigned char *q, int qlen, unsigned char *ans,
                   int cap, int timeout)
{
    int fd = socket(ns->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int r = -1;
    unsigned char pre[2] = { (unsigned char)(qlen >> 8), (unsigned char)qlen };
    if (connect(fd, ns, nslen) == 0 && write(fd, pre, 2) == 2 &&
        write(fd, q, (size_t)qlen) == qlen) {
        int got = 0, want = -1;
        unsigned char lenb[2];
        int lg = 0;
        struct pollfd p = { fd, POLLIN, 0 };
        while (poll(&p, 1, timeout * 1000) > 0) {
            if (lg < 2) {
                long k = read(fd, lenb + lg, (size_t)(2 - lg));
                if (k <= 0)
                    break;
                lg += (int)k;
                if (lg == 2)
                    want = lenb[0] << 8 | lenb[1];
                continue;
            }
            if (want > cap)
                break;
            long k = read(fd, ans + got, (size_t)(want - got));
            if (k <= 0)
                break;
            got += (int)k;
            if (got == want) {
                r = got;
                break;
            }
        }
    }
    close(fd);
    return r;
}

/* One question to the servers in turn, attempts times round. The
 * answer's length, or -1: EAGAIN when nobody answered, ENOENT for a
 * name that does not exist (NXDOMAIN), EIO for a server failure. */
static int ask(const struct conf *c, const char *name, int type,
               unsigned char *ans, int cap)
{
    unsigned char q[300];
    unsigned id = id16();
    int qlen = build_query(q, sizeof q, name, type, id);
    if (qlen < 0) {
        errno = EINVAL;
        return -1;
    }
    int failed = EAGAIN;
    for (int round = 0; round < c->attempts; round++)
        for (int i = 0; i < c->nns; i++) {
            const struct sockaddr *ns = (const struct sockaddr *)&c->ns[i];
            int fd = socket(ns->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return -1;
            if (connect(fd, ns, c->nslen[i]) < 0 ||
                send(fd, q, (size_t)qlen, 0) != qlen) {
                close(fd);
                continue;
            }
            struct pollfd p = { fd, POLLIN, 0 };
            long got = -1;
            while (poll(&p, 1, c->timeout * 1000) > 0) {
                got = recv(fd, ans, (size_t)cap, 0);
                /* the reply to THIS question: id, QR set, the same name */
                if (got >= 12 && ans[0] == q[0] && ans[1] == q[1] &&
                    (ans[2] & 0x80) && got >= qlen &&
                    memcmp(ans + 12, q + 12, (size_t)(qlen - 12)) == 0)
                    break;
                got = -1;
            }
            close(fd);
            if (got < 0)
                continue;
            if (ans[2] & 0x02) {     /* TC: the whole answer, over TCP */
                int t = ask_tcp(ns, c->nslen[i], q, qlen, ans, cap,
                                c->timeout);
                if (t > 0)
                    got = t;
            }
            int rcode = ans[3] & 0x0f;
            if (rcode == 0)
                return (int)got;
            if (rcode == 3) {
                errno = ENOENT;
                return -1;
            }
            failed = EIO;
        }
    errno = failed;
    return -1;
}

/* ---- reading an answer ---------------------------------------------------- */

static int skip_question(const unsigned char *m, int len)
{
    char tmp[256];
    int qd = m[4] << 8 | m[5], off = 12;
    for (int i = 0; i < qd; i++) {
        off = __dns_name(m, len, off, tmp, sizeof tmp);
        if (off < 0 || off + 4 > len)
            return -1;
        off += 4;
    }
    return off;
}

/* The records of `type` in the answer section, through cb; the canonical
 * name (the last CNAME's target, else the name asked) in canon. */
int __dns_records(const unsigned char *m, int len, int type, char *canon,
                  void (*cb)(void *, const unsigned char *, int, const
                             unsigned char *, int),
                  void *ctx)
{
    int an = m[6] << 8 | m[7], n = 0;
    int off = skip_question(m, len);
    if (off < 0)
        return -1;
    for (int i = 0; i < an; i++) {
        char owner[256];
        off = __dns_name(m, len, off, owner, sizeof owner);
        if (off < 0 || off + 10 > len)
            return -1;
        int t = m[off] << 8 | m[off + 1];
        int rdlen = m[off + 8] << 8 | m[off + 9];
        off += 10;
        if (off + rdlen > len)
            return -1;
        if (t == 5 && canon)          /* CNAME */
            __dns_name(m, len, off, canon, 256);
        else if (t == type) {
            cb(ctx, m + off, rdlen, m, len);
            n++;
        }
        off += rdlen;
    }
    return n;
}

/* A question about name, with resolv.conf's search list applied: a name
 * with fewer dots than ndots is tried with each search domain first,
 * then as it is. The answer as for ask(). */
int __dns_lookup(const char *name, int type, unsigned char *ans, int cap)
{
    struct conf c;
    read_conf(&c);
    int dots = 0;
    for (const char *p = name; *p; p++)
        dots += *p == '.';
    size_t n = strlen(name);
    if (n && name[n - 1] == '.') {    /* absolute: exactly this */
        char tmp[256];
        if (n > sizeof tmp) {
            errno = EINVAL;
            return -1;
        }
        memcpy(tmp, name, n - 1);
        tmp[n - 1] = 0;
        return ask(&c, tmp, type, ans, cap);
    }
    int last = ENOENT;
    if (dots < c.ndots)
        for (int i = 0; i < c.nsearch; i++) {
            char full[512];
            snprintf(full, sizeof full, "%s.%s", name, c.search[i]);
            int r = ask(&c, full, type, ans, cap);
            if (r >= 0)
                return r;
            if (errno != ENOENT)
                last = errno;
        }
    int r = ask(&c, name, type, ans, cap);
    if (r < 0 && errno == ENOENT && last != ENOENT)
        errno = last;
    return r;
}
