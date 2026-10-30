/* The resolver's public interface: BIND's res_* calls, the name codecs
 * and the message parser, over the DNS client in resolv.c.
 *
 * A name is text ("www.example.com", with \. and \DDD escapes), or wire
 * format (length-prefixed labels and a 0), or compressed wire format (a
 * tail replaced by a pointer back into the message). ns_name_pton and
 * ns_name_ntop go between the first two, ns_name_pack and
 * ns_name_unpack between the last two. Their edge cases -- what counts
 * as fully qualified, what is escaped, which earlier name a pointer
 * goes to -- are BIND's, which glibc keeps, since programs compare the
 * bytes.
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <resolv.h>
#include <stdlib.h>
#include <unistd.h>

#include "posix.h"
#include "resolv.h"

#define EMSG(r) do { errno = EMSGSIZE; return (r); } while (0)

/* ---- numbers on the wire ------------------------------------------------------------- */

WEAK unsigned ns_get16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

WEAK unsigned long ns_get32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 |
           p[3];
}

WEAK void ns_put16(unsigned v, unsigned char *p)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

WEAK void ns_put32(unsigned long v, unsigned char *p)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

WEAK const struct _ns_flagdata _ns_flagdata[16] = {
    { 0x8000, 15 }, { 0x7800, 11 }, { 0x0400, 10 }, { 0x0200, 9 }, { 0x0100, 8 },
    { 0x0080, 7 },  { 0x0040, 6 },  { 0x0020, 5 },  { 0x0010, 4 }, { 0x000f, 0 },
};

WEAK int (ns_msg_getflag)(ns_msg h, int flag)
{
    return (h._flags & _ns_flagdata[flag].mask) >> _ns_flagdata[flag].shift;
}

/* ---- names: text and wire ------------------------------------------------------------- */

static int special(int c)
{
    return c == '"' || c == '.' || c == ';' || c == '\\' || c == '(' || c == ')' ||
           c == '@' || c == '$';
}

/* the wire name src as text, escaped: its length with the NUL */
WEAK int ns_name_ntop(const unsigned char *src, char *dst, size_t dstsiz)
{
    char *d = dst, *end = dst + dstsiz;
    unsigned n;
    while ((n = *src++) != 0) {
        if (n & NS_CMPRSFLGS)
            EMSG(-1);
        if (d != dst) {
            if (d >= end)
                EMSG(-1);
            *d++ = '.';
        }
        for (; n > 0; n--) {
            int c = *src++;
            if (special(c)) {
                if (d + 1 >= end)
                    EMSG(-1);
                *d++ = '\\';
                *d++ = (char)c;
            } else if (c > 0x20 && c < 0x7f) {
                if (d >= end)
                    EMSG(-1);
                *d++ = (char)c;
            } else {
                if (d + 3 >= end)
                    EMSG(-1);
                *d++ = '\\';
                *d++ = (char)('0' + c / 100);
                *d++ = (char)('0' + c / 10 % 10);
                *d++ = (char)('0' + c % 10);
            }
        }
    }
    if (d == dst) {
        if (d >= end)
            EMSG(-1);
        *d++ = '.';
    }
    if (d >= end)
        EMSG(-1);
    *d++ = 0;
    return (int)(d - dst);
}

/* the text name src on the wire: 1 when it ended in a dot (or was "."),
 * 0 when not */
WEAK int ns_name_pton(const char *src, unsigned char *dst, size_t dstsiz)
{
    unsigned char *label = dst, *w = dst + 1, *end = dst + dstsiz;
    if (!dstsiz)
        EMSG(-1);
    for (;;) {
        int c = (unsigned char)*src++;
        if (c == 0)
            break;
        if (c == '\\') {
            c = (unsigned char)*src++;
            if (c == 0)
                EMSG(-1);          /* a backslash at the end */
            if (isdigit(c)) {
                if (!isdigit((unsigned char)src[0]) || !isdigit((unsigned char)src[1]))
                    EMSG(-1);
                c = (c - '0') * 100 + (src[0] - '0') * 10 + (src[1] - '0');
                src += 2;
                if (c > 255)
                    EMSG(-1);
            }
        } else if (c == '.') {
            size_t len = (size_t)(w - label - 1);
            if (len > NS_MAXLABEL)
                EMSG(-1);
            *label = (unsigned char)len;
            if (*src == 0) {           /* the dot that ends the name */
                if (len) {
                    if (w >= end)
                        EMSG(-1);
                    *w++ = 0;
                }
                if (w - dst > NS_MAXCDNAME)
                    EMSG(-1);
                return 1;
            }
            if (len == 0 || *src == '.')
                EMSG(-1);          /* an empty label */
            if (w >= end)
                EMSG(-1);
            label = w++;
            continue;
        }
        if (w >= end)
            EMSG(-1);
        *w++ = (unsigned char)c;
    }
    size_t len = (size_t)(w - label - 1);
    if (len > NS_MAXLABEL)
        EMSG(-1);
    *label = (unsigned char)len;
    if (len) {
        if (w >= end)
            EMSG(-1);
        *w++ = 0;
    }
    if (w - dst > NS_MAXCDNAME)
        EMSG(-1);
    return 0;
}

/* the name at src in msg, its pointers followed, into dst: the bytes it
 * takes in the message */
WEAK int ns_name_unpack(const unsigned char *msg, const unsigned char *eom,
                        const unsigned char *src, unsigned char *dst, size_t dstsiz)
{
    const unsigned char *p = src;
    unsigned char *w = dst, *end = dst + dstsiz;
    int taken = -1;
    long seen = 0;                 /* bytes walked: past the message's size, a loop */
    if (p < msg || p >= eom)
        EMSG(-1);
    for (;;) {
        unsigned n = *p++;
        switch (n & NS_CMPRSFLGS) {
        case 0:
            if (w + n + 1 > end || p + n > eom)
                EMSG(-1);
            *w++ = (unsigned char)n;
            if (n == 0) {
                if (taken < 0)
                    taken = (int)(p - src);
                return taken;
            }
            memcpy(w, p, n);
            w += n;
            p += n;
            seen += n + 1;
            break;
        case NS_CMPRSFLGS:
            if (p >= eom)
                EMSG(-1);
            if (taken < 0)
                taken = (int)(p + 1 - src);
            p = msg + ((n & 0x3f) << 8 | *p);
            if (p < msg || p >= eom)
                EMSG(-1);
            seen += 2;
            break;
        default:
            EMSG(-1);              /* 0x40 and 0x80: extended labels */
        }
        if (seen >= eom - msg)
            EMSG(-1);
    }
}

WEAK int ns_name_uncompress(const unsigned char *msg, const unsigned char *eom,
                            const unsigned char *src, char *dst, size_t dstsiz)
{
    unsigned char tmp[NS_MAXCDNAME];
    int n = ns_name_unpack(msg, eom, src, tmp, sizeof tmp);
    if (n < 0 || ns_name_ntop(tmp, dst, dstsiz) < 0)
        return -1;
    return n;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* the remaining labels at name equal, ignoring case, a name already in
 * the message starting at at (pointers in it followed): whether so */
static int same_tail(const unsigned char *name, const unsigned char *msg,
                     const unsigned char *at)
{
    for (int hops = 0; hops < 128; hops++) {
        unsigned n = *at;
        if ((n & NS_CMPRSFLGS) == NS_CMPRSFLGS) {
            at = msg + ((n & 0x3f) << 8 | at[1]);
            continue;
        }
        if (n & NS_CMPRSFLGS)
            return 0;
        if (n != *name)
            return 0;
        if (n == 0)
            return 1;
        for (unsigned i = 1; i <= n; i++)
            if (lower(name[i]) != lower(at[i]))
                return 0;
        name += n + 1;
        at += n + 1;
    }
    return 0;
}

/* where in the message the name (wire format, uncompressed) already is,
 * as itself or as the tail of a name dnptrs lists -- the first such
 * name, the first of its tails: the offset, or -1 */
static int find_name(const unsigned char *name, const unsigned char *msg,
                     const unsigned char **dnptrs, const unsigned char **end)
{
    for (const unsigned char **pp = dnptrs; pp < end && *pp; pp++)
        for (const unsigned char *s = *pp;
             *s && !(*s & NS_CMPRSFLGS) && s - msg < 0x4000; s += *s + 1)
            if (same_tail(name, msg, s))
                return (int)(s - msg);
    return -1;
}

/* the wire name src into dst, its longest tail already in the message
 * replaced by a pointer; the name's start added to dnptrs (dnptrs[0]
 * is the message) when there is room. The bytes written. */
WEAK int ns_name_pack(const unsigned char *src, unsigned char *dst, int dstsiz,
                      const unsigned char **dnptrs, const unsigned char **lastdnptr)
{
    const unsigned char *msg = 0, **list = 0, **listend = 0, **added = 0;
    if (dnptrs && (msg = *dnptrs) != 0) {
        list = dnptrs + 1;
        listend = list;
        while (*listend)
            listend++;
    }
    /* the name's length, checked */
    size_t total = 0;
    for (const unsigned char *s = src;; s += *s + 1) {
        if (*s & NS_CMPRSFLGS)
            EMSG(-1);
        total += *s + 1u;
        if (total > NS_MAXCDNAME)
            EMSG(-1);
        if (!*s)
            break;
    }
    unsigned char *w = dst, *end = dst + dstsiz;
    for (const unsigned char *s = src;; s += *s + 1) {
        if (msg && *s) {
            /* only the names listed before this one */
            int at = find_name(s, msg, list, listend);
            if (at >= 0) {
                if (w + 2 > end)
                    goto full;
                *w++ = (unsigned char)(NS_CMPRSFLGS | at >> 8);
                *w++ = (unsigned char)at;
                return (int)(w - dst);
            }
            if (!added && lastdnptr && listend < lastdnptr - 1 &&
                w - msg < 0x4000) {
                added = listend;
                added[0] = w;
                added[1] = 0;
            }
        }
        if (w + *s + 1 > end)
            goto full;
        memcpy(w, s, *s + 1u);
        w += *s + 1;
        if (!*s)
            return (int)(w - dst);
    }
full:
    if (added)
        *added = 0;
    EMSG(-1);
}

WEAK int ns_name_compress(const char *src, unsigned char *dst, size_t dstsiz,
                          const unsigned char **dnptrs, const unsigned char **lastdnptr)
{
    unsigned char tmp[NS_MAXCDNAME];
    if (ns_name_pton(src, tmp, sizeof tmp) < 0)
        return -1;
    return ns_name_pack(tmp, dst, (int)dstsiz, dnptrs, lastdnptr);
}

WEAK int ns_name_skip(const unsigned char **pp, const unsigned char *eom)
{
    const unsigned char *p = *pp;
    while (p < eom) {
        unsigned n = *p++;
        if ((n & NS_CMPRSFLGS) == NS_CMPRSFLGS) {
            p++;
            break;
        }
        if (n & NS_CMPRSFLGS)
            EMSG(-1);
        if (n == 0)
            break;
        p += n;
    }
    if (p > eom)
        EMSG(-1);
    *pp = p;
    return 0;
}

WEAK int dn_expand(const unsigned char *msg, const unsigned char *eom,
                   const unsigned char *src, char *dst, int dstsiz)
{
    int n = ns_name_uncompress(msg, eom, src, dst, (size_t)dstsiz);
    if (n > 0 && dst[0] == '.')
        dst[0] = 0;                /* the root, as "" */
    return n;
}

WEAK int dn_comp(const char *src, unsigned char *dst, int dstsiz, unsigned char **dnptrs,
                 unsigned char **lastdnptr)
{
    return ns_name_compress(src, dst, (size_t)dstsiz, (const unsigned char **)dnptrs,
                            (const unsigned char **)lastdnptr);
}

WEAK int dn_skipname(const unsigned char *ptr, const unsigned char *eom)
{
    const unsigned char *p = ptr;
    if (ns_name_skip(&p, eom) < 0)
        return -1;
    return (int)(p - ptr);
}

/* ---- reading a message ----------------------------------------------------------------- */

WEAK int ns_skiprr(const unsigned char *ptr, const unsigned char *eom, ns_sect s, int count)
{
    const unsigned char *p = ptr;
    while (count-- > 0) {
        int b = dn_skipname(p, eom);
        if (b < 0)
            EMSG(-1);
        p += b + 4;
        if (s != ns_s_qd) {
            if (p + 6 > eom)
                EMSG(-1);
            p += 4;
            unsigned rdlen = ns_get16(p);
            p += 2 + rdlen;
        }
    }
    if (p > eom)
        EMSG(-1);
    return (int)(p - ptr);
}

static void set_section(ns_msg *h, int s)
{
    h->_sect = (ns_sect)s;
    if (s == ns_s_max) {
        h->_rrnum = -1;
        h->_msg_ptr = 0;
    } else {
        h->_rrnum = 0;
        h->_msg_ptr = h->_sections[s];
    }
}

WEAK int ns_initparse(const unsigned char *msg, int len, ns_msg *h)
{
    const unsigned char *eom = msg + len, *p = msg;
    h->_msg = msg;
    h->_eom = eom;
    if (len < NS_HFIXEDSZ)
        EMSG(-1);
    h->_id = (uint16_t)ns_get16(p);
    h->_flags = (uint16_t)ns_get16(p + 2);
    for (int i = 0; i < ns_s_max; i++)
        h->_counts[i] = (uint16_t)ns_get16(p + 4 + 2 * i);
    p += NS_HFIXEDSZ;
    for (int i = 0; i < ns_s_max; i++) {
        if (!h->_counts[i]) {
            h->_sections[i] = 0;
            continue;
        }
        int b = ns_skiprr(p, eom, (ns_sect)i, h->_counts[i]);
        if (b < 0)
            return -1;
        h->_sections[i] = p;
        p += b;
    }
    if (p != eom)
        EMSG(-1);                  /* bytes after the last record */
    set_section(h, ns_s_max);
    return 0;
}

WEAK int ns_parserr(ns_msg *h, ns_sect s, int rrnum, ns_rr *rr)
{
    if ((int)s < 0 || s >= ns_s_max) {
        errno = ENODEV;
        return -1;
    }
    if (s != h->_sect)
        set_section(h, s);
    if (rrnum == -1)
        rrnum = h->_rrnum;
    if (rrnum < 0 || (long)rrnum >= (long)h->_counts[s]) {
        errno = ENODEV;
        return -1;
    }
    if (rrnum < h->_rrnum)
        set_section(h, s);
    if (rrnum > h->_rrnum) {
        int b = ns_skiprr(h->_msg_ptr, h->_eom, s, rrnum - h->_rrnum);
        if (b < 0)
            return -1;
        h->_msg_ptr += b;
        h->_rrnum = rrnum;
    }
    int b = dn_expand(h->_msg, h->_eom, h->_msg_ptr, rr->name, NS_MAXDNAME);
    if (b < 0)
        return -1;
    h->_msg_ptr += b;
    if (h->_msg_ptr + 4 > h->_eom)
        EMSG(-1);
    rr->type = (uint16_t)ns_get16(h->_msg_ptr);
    rr->rr_class = (uint16_t)ns_get16(h->_msg_ptr + 2);
    h->_msg_ptr += 4;
    if (s == ns_s_qd) {
        rr->ttl = 0;
        rr->rdlength = 0;
        rr->rdata = 0;
    } else {
        if (h->_msg_ptr + 6 > h->_eom)
            EMSG(-1);
        rr->ttl = (uint32_t)ns_get32(h->_msg_ptr);
        rr->rdlength = (uint16_t)ns_get16(h->_msg_ptr + 4);
        h->_msg_ptr += 6;
        if (h->_msg_ptr + rr->rdlength > h->_eom)
            EMSG(-1);
        rr->rdata = h->_msg_ptr;
        h->_msg_ptr += rr->rdlength;
    }
    if ((long)++h->_rrnum > (long)h->_counts[s])
        set_section(h, s + 1);
    return 0;
}

/* ---- the state ------------------------------------------------------------------------- */

static struct __res_state res_global;

WEAK struct __res_state *__res_state(void) { return &res_global; }

static void set_herr(res_state st, int e)
{
    st->res_h_errno = e;
    h_errno = e;
}

WEAK void res_nclose(res_state st)
{
    for (int i = 0; i < MAXNS; i++) {
        free(st->_u._ext.nsaddrs[i]);
        st->_u._ext.nsaddrs[i] = 0;
    }
}

WEAK int res_ninit(res_state st)
{
    struct __dns_conf c;
    __dns_read_conf(&c);
    res_nclose(st);
    memset(st, 0, sizeof *st);
    st->retrans = c.timeout;
    st->retry = c.attempts;
    st->options = RES_DEFAULT;
    st->ndots = (unsigned)(c.ndots > RES_MAXNDOTS ? RES_MAXNDOTS : c.ndots);
    st->id = (unsigned short)__dns_id16();
    for (int i = 0; i < c.nns && i < MAXNS; i++) {
        if (c.ns[i].ss_family == AF_INET) {
            memcpy(&st->nsaddr_list[i], &c.ns[i], sizeof st->nsaddr_list[i]);
        } else {
            struct sockaddr_in6 *a6 = malloc(sizeof *a6);
            if (a6)
                memcpy(a6, &c.ns[i], sizeof *a6);
            st->_u._ext.nsaddrs[i] = a6;
            st->nsaddr_list[i].sin_family = 0;
        }
        st->nscount++;
    }
    /* the search list: resolv.conf's, else the domain of the host's name */
    char *w = st->defdname, *wend = st->defdname + sizeof st->defdname;
    int k = 0;
    if (c.nsearch) {
        for (int i = 0; i < c.nsearch && k < MAXDNSRCH; i++) {
            size_t n = strlen(c.search[i]) + 1;
            if (w + n > wend)
                break;
            memcpy(w, c.search[i], n);
            st->dnsrch[k++] = w;
            w += n;
        }
    } else {
        char host[256];
        char *dot;
        if (gethostname(host, sizeof host) == 0 && (dot = strchr(host, '.')) && dot[1]) {
            snprintf(st->defdname, sizeof st->defdname, "%s", dot + 1);
            st->dnsrch[k++] = st->defdname;
        }
    }
    st->dnsrch[k] = 0;
    st->options |= RES_INIT;
    return 0;
}

WEAK int res_init(void) { return res_ninit(&_res); }

WEAK void res_close(void) { res_nclose(&_res); }

static res_state global_ready(void)
{
    if (!(_res.options & RES_INIT))
        res_init();
    return &_res;
}

/* ---- building and sending ------------------------------------------------------------- */

WEAK int res_nmkquery(res_state st, int op, const char *dname, int cls, int type,
                      const unsigned char *data, int datalen, const unsigned char *newrr,
                      unsigned char *buf, int buflen)
{
    (void)data;
    (void)datalen;
    (void)newrr;
    if (!buf || buflen < NS_HFIXEDSZ || (op != ns_o_query && op != ns_o_notify))
        return -1;
    memset(buf, 0, NS_HFIXEDSZ);
    unsigned id = __dns_id16();
    buf[0] = (unsigned char)(id >> 8);
    buf[1] = (unsigned char)id;
    buf[2] = (unsigned char)(op << 3 | ((st->options & RES_RECURSE) ? 1 : 0));
    if (st->options & RES_TRUSTAD)
        buf[3] |= 0x20;
    int room = buflen - NS_HFIXEDSZ - NS_QFIXEDSZ;
    if (room < 0)
        return -1;
    const unsigned char *dnptrs[20] = { buf, 0 };
    int n = ns_name_compress(dname, buf + NS_HFIXEDSZ, (size_t)room, dnptrs, dnptrs + 20);
    if (n < 0)
        return -1;
    unsigned char *p = buf + NS_HFIXEDSZ + n;
    ns_put16((unsigned)type, p);
    ns_put16((unsigned)cls, p + 2);
    buf[5] = 1;                    /* one question */
    return NS_HFIXEDSZ + n + NS_QFIXEDSZ;
}

WEAK int res_mkquery(int op, const char *dname, int cls, int type, const unsigned char *data,
                     int datalen, const unsigned char *newrr, unsigned char *buf, int buflen)
{
    return res_nmkquery(global_ready(), op, dname, cls, type, data, datalen, newrr, buf, buflen);
}

/* the answer -- any rcode but SERVFAIL, NOTIMP and REFUSED, which move on
 * to the next server -- or -1: ECONNREFUSED when no server was there,
 * ETIMEDOUT otherwise (glibc's numbers: res_search stops at the first) */
WEAK int res_nsend(res_state st, const unsigned char *msg, int msglen, unsigned char *ans,
                   int anslen)
{
    struct __dns_conf c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < st->nscount && i < MAXNS; i++) {
        if (st->_u._ext.nsaddrs[i]) {
            memcpy(&c.ns[c.nns], st->_u._ext.nsaddrs[i], sizeof(struct sockaddr_in6));
            c.nslen[c.nns++] = sizeof(struct sockaddr_in6);
        } else {
            memcpy(&c.ns[c.nns], &st->nsaddr_list[i], sizeof(struct sockaddr_in));
            c.nslen[c.nns++] = sizeof(struct sockaddr_in);
        }
    }
    if (!c.nns) {
        errno = ESRCH;
        return -1;
    }
    c.timeout = st->retrans > 0 ? st->retrans : RES_TIMEOUT;
    c.attempts = st->retry > 0 ? st->retry : 1;
    unsigned accept = 0xffffu & ~(1u << 2 | 1u << 4 | 1u << 5);
    int n = __dns_exchange(&c, msg, msglen, ans, anslen, accept,
                           (st->options & RES_USEVC) != 0);
    if (n < 0)
        errno = errno == EIO || errno == EAGAIN ? ETIMEDOUT : errno;
    return n;
}

WEAK int res_send(const unsigned char *msg, int msglen, unsigned char *ans, int anslen)
{
    return res_nsend(global_ready(), msg, msglen, ans, anslen);
}

/* ---- asking ----------------------------------------------------------------------------- */

WEAK int res_nquery(res_state st, const char *name, int cls, int type, unsigned char *ans,
                    int anslen)
{
    unsigned char q[NS_PACKETSZ];
    int n = res_nmkquery(st, ns_o_query, name, cls, type, 0, 0, 0, q, sizeof q);
    if (n <= 0) {
        set_herr(st, NO_RECOVERY);
        return n;
    }
    n = res_nsend(st, q, n, ans, anslen);
    if (n < 0) {
        set_herr(st, TRY_AGAIN);
        return n;
    }
    int rcode = ans[3] & 0x0f, ancount = ans[6] << 8 | ans[7];
    if (rcode != ns_r_noerror || ancount == 0) {
        set_herr(st, rcode == ns_r_nxdomain ? HOST_NOT_FOUND
                     : rcode == ns_r_servfail ? TRY_AGAIN
                     : rcode == ns_r_noerror ? NO_DATA : NO_RECOVERY);
        return -1;
    }
    return n;
}

WEAK int res_query(const char *name, int cls, int type, unsigned char *ans, int anslen)
{
    return res_nquery(global_ready(), name, cls, type, ans, anslen);
}

/* name.domain -- or name alone without its final dot, for no domain */
WEAK int res_nquerydomain(res_state st, const char *name, const char *domain, int cls,
                          int type, unsigned char *ans, int anslen)
{
    char full[NS_MAXDNAME];
    size_t n = strlen(name);
    if (!domain) {
        if (n >= NS_MAXDNAME) {
            set_herr(st, NO_RECOVERY);
            return -1;
        }
        memcpy(full, name, n + 1);
        if (n && full[n - 1] == '.')
            full[n - 1] = 0;
    } else {
        size_t d = strlen(domain);
        if (n + d + 1 >= NS_MAXDNAME) {
            set_herr(st, NO_RECOVERY);
            return -1;
        }
        snprintf(full, sizeof full, "%s.%s", name, domain);
    }
    return res_nquery(st, full, cls, type, ans, anslen);
}

WEAK int res_querydomain(const char *name, const char *domain, int cls, int type,
                         unsigned char *ans, int anslen)
{
    return res_nquerydomain(global_ready(), name, domain, cls, type, ans, anslen);
}

/* the name as it is when it has ndots dots (or ends in one), and with
 * each search domain -- in that order, or the other way round -- until
 * an answer; the h_errno of the try that says most */
WEAK int res_nsearch(res_state st, const char *name, int cls, int type, unsigned char *ans,
                     int anslen)
{
    int dots = 0, trailing = 0;
    const char *p;
    errno = 0;
    set_herr(st, HOST_NOT_FOUND);
    for (p = name; *p; p++)
        dots += *p == '.';
    if (p > name && p[-1] == '.')
        trailing = 1;
    int saved = -1, as_is = 0, searched = 0, root_listed = 0, nodata = 0, servfail = 0, r;
    if (dots >= (int)st->ndots || trailing) {
        r = res_nquerydomain(st, name, 0, cls, type, ans, anslen);
        if (r > 0 || trailing)
            return r;
        saved = st->res_h_errno;
        as_is = 1;
    }
    if ((!dots && (st->options & RES_DEFNAMES)) ||
        (dots && !trailing && (st->options & RES_DNSRCH))) {
        for (char **d = st->dnsrch; *d; d++) {
            const char *dom = *d;
            searched = 1;
            if (*dom == '.')
                dom++;
            if (!*dom)
                root_listed = 1;
            r = res_nquerydomain(st, name, dom, cls, type, ans, anslen);
            if (r > 0)
                return r;
            if (errno == ECONNREFUSED) {
                set_herr(st, TRY_AGAIN);
                return -1;
            }
            int stop = 0;
            if (st->res_h_errno == NO_DATA)
                nodata = 1;
            else if (st->res_h_errno == TRY_AGAIN && r < 0 && anslen >= 4 &&
                     (ans[3] & 0x0f) == ns_r_servfail)
                servfail = 1;
            else if (st->res_h_errno != HOST_NOT_FOUND)
                stop = 1;
            if (stop || !(st->options & RES_DNSRCH))
                break;
        }
    }
    if ((dots || !searched || !(st->options & RES_NOTLDQUERY)) && !as_is && !root_listed) {
        r = res_nquerydomain(st, name, 0, cls, type, ans, anslen);
        if (r > 0)
            return r;
    }
    if (saved != -1)
        set_herr(st, saved);
    else if (nodata)
        set_herr(st, NO_DATA);
    else if (servfail)
        set_herr(st, TRY_AGAIN);
    return -1;
}

WEAK int res_search(const char *name, int cls, int type, unsigned char *ans, int anslen)
{
    return res_nsearch(global_ready(), name, cls, type, ans, anslen);
}

/* ---- what a name may be ------------------------------------------------------------------ */

static int printable(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s <= ' ' || (unsigned char)*s > '~')
            return 0;
    return 1;
}

/* every label of the wire name letters, digits, '-' and '_' */
static int host_labels(const unsigned char *w)
{
    for (unsigned n; (n = *w++) != 0;)
        for (; n; n--, w++)
            if (!isalnum(*w) && *w != '-' && *w != '_')
                return 0;
    return 1;
}

WEAK int res_hnok(const char *dn)
{
    unsigned char w[NS_MAXCDNAME];
    if (!printable(dn) || ns_name_pton(dn, w, sizeof w) < 0 || (w[0] && w[1] == '-'))
        return 0;
    return host_labels(w);
}

/* a host name, or "*." before one */
WEAK int res_ownok(const char *dn)
{
    unsigned char w[NS_MAXCDNAME];
    if (!printable(dn) || ns_name_pton(dn, w, sizeof w) < 0 || (w[0] && w[1] == '-'))
        return 0;
    return host_labels(w[0] == 1 && w[1] == '*' ? w + 2 : w);
}

/* any first label, then a host name: a mailbox */
WEAK int res_mailok(const char *dn)
{
    unsigned char w[NS_MAXCDNAME];
    if (!printable(dn) || ns_name_pton(dn, w, sizeof w) < 0)
        return 0;
    if (!w[0])
        return 1;
    const unsigned char *tail = w + 1 + w[0];
    return *tail && host_labels(tail);
}

WEAK int res_dnok(const char *dn)
{
    unsigned char w[NS_MAXCDNAME];
    return printable(dn) && ns_name_pton(dn, w, sizeof w) >= 0;
}
