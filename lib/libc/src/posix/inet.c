/* Internet addresses as text, both ways, and the byte-order functions.
 *
 * inet_pton is strict (POSIX's: four decimal parts for IPv4, no leading
 * zeros, as glibc has it); inet_aton is the old lenient one, which takes
 * "127.1" and parts in octal and hex because a whole generation of
 * configuration files was written for it. inet_ntop writes IPv6 as RFC
 * 5952 says -- lower case, the longest run of zero groups as "::" -- and
 * an IPv4-mapped address as ::ffff:a.b.c.d, as glibc does. */
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "posix.h"

#undef htonl
#undef htons
#undef ntohl
#undef ntohs

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
WEAK uint32_t htonl(uint32_t v) { return v; }
WEAK uint16_t htons(uint16_t v) { return v; }
#else
WEAK uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
WEAK uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
#endif
WEAK uint32_t ntohl(uint32_t v) { return htonl(v); }
WEAK uint16_t ntohs(uint16_t v) { return htons(v); }

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

/* "a", "a.b", "a.b.c", "a.b.c.d", each part decimal, 0-octal or 0x-hex;
 * the last part fills the bytes that are left */
WEAK int inet_aton(const char *s, struct in_addr *out)
{
    unsigned long part[4];
    int n = 0;
    const char *p = s;
    for (;;) {
        if (!isdigit((unsigned char)*p))
            return 0;
        char *end;
        part[n] = strtoul(p, &end, 0);
        if (end == p)
            return 0;
        p = end;
        n++;
        if (*p != '.')
            break;
        if (n == 4)
            return 0;
        p++;
    }
    if (*p && !isspace((unsigned char)*p))
        return 0;
    unsigned long v;
    switch (n) {
    case 1:
        if (part[0] > 0xffffffffUL) return 0;
        v = part[0];
        break;
    case 2:
        if (part[0] > 255 || part[1] > 0xffffff) return 0;
        v = part[0] << 24 | part[1];
        break;
    case 3:
        if (part[0] > 255 || part[1] > 255 || part[2] > 0xffff) return 0;
        v = part[0] << 24 | part[1] << 16 | part[2];
        break;
    default:
        if (part[0] > 255 || part[1] > 255 || part[2] > 255 || part[3] > 255)
            return 0;
        v = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
        break;
    }
    if (out)
        out->s_addr = htonl((uint32_t)v);
    return 1;
}

WEAK in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

WEAK in_addr_t inet_network(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? ntohl(a.s_addr) : INADDR_NONE;
}

WEAK char *inet_ntoa(struct in_addr a)
{
    static char buf[INET_ADDRSTRLEN];
    const unsigned char *b = (const unsigned char *)&a.s_addr;
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static int pton4(const char *s, unsigned char out[4])
{
    int n = 0;
    while (n < 4) {
        if (!isdigit((unsigned char)*s))
            return 0;
        unsigned v = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) {
            if (digits && v == 0)
                return 0;            /* a leading zero */
            v = v * 10 + (unsigned)(*s++ - '0');
            if (v > 255)
                return 0;
            digits++;
        }
        out[n++] = (unsigned char)v;
        if (n < 4 && *s++ != '.')
            return 0;
    }
    return *s == 0;
}

static int hexval(int c)
{
    return isdigit(c) ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f'
                                      ? (c | 32) - 'a' + 10 : -1;
}

static int pton6(const char *s, unsigned char out[16])
{
    unsigned char tmp[16] = { 0 };
    int at = 0, gap = -1;
    if (s[0] == ':') {
        if (s[1] != ':')
            return 0;
        s++;
    }
    while (*s) {
        if (*s == ':') {
            if (gap >= 0)
                return 0;            /* a second "::" */
            gap = at;
            s++;
            continue;
        }
        /* an embedded IPv4 address, in the last four bytes */
        const char *q = s;
        while (isxdigit((unsigned char)*q))
            q++;
        if (*q == '.') {
            if (at > 12 || !pton4(s, tmp + at))
                return 0;
            at += 4;
            break;
        }
        unsigned v = 0;
        int digits = 0;
        while (hexval((unsigned char)*s) >= 0) {
            v = v << 4 | (unsigned)hexval((unsigned char)*s++);
            if (++digits > 4)
                return 0;
        }
        if (!digits || at > 14)
            return 0;
        tmp[at++] = (unsigned char)(v >> 8);
        tmp[at++] = (unsigned char)v;
        if (*s == ':') {
            s++;
            if (!*s)
                return 0;            /* a trailing single ':' */
        } else if (*s)
            return 0;
    }
    if (gap >= 0) {
        int move = at - gap;
        if (at == 16)
            return 0;                /* "::" standing for nothing */
        memmove(tmp + 16 - move, tmp + gap, (size_t)move);
        memset(tmp + gap, 0, (size_t)(16 - move - gap));
    } else if (at != 16)
        return 0;
    memcpy(out, tmp, 16);
    return 1;
}

WEAK int inet_pton(int family, const char *restrict src, void *restrict dst)
{
    if (family == AF_INET)
        return pton4(src, dst);
    if (family == AF_INET6)
        return pton6(src, dst);
    errno = EAFNOSUPPORT;
    return -1;
}

static const char *ntop6(const unsigned char *a, char *dst, socklen_t n)
{
    unsigned w[8];
    for (int i = 0; i < 8; i++)
        w[i] = (unsigned)a[2 * i] << 8 | a[2 * i + 1];
    /* the longest run of zero groups, two or more, the first if tied */
    int best = -1, blen = 0;
    for (int i = 0; i < 8; ) {
        if (w[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !w[j])
            j++;
        if (j - i > blen) { best = i; blen = j - i; }
        i = j;
    }
    if (blen < 2)
        best = -1;
    char buf[INET6_ADDRSTRLEN], *p = buf;
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            *p++ = ':';
            if (i == 0)
                *p++ = ':';
            i += blen - 1;
            continue;
        }
        /* glibc's IPv4 tails: ::ffff:a.b.c.d and ::a.b.c.d */
        if (i == 6 && best == 0 &&
            (blen == 6 || (blen == 5 && w[5] == 0xffff))) {
            p += sprintf(p, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
            break;
        }
        p += sprintf(p, "%x", w[i]);
        if (i < 7)
            *p++ = ':';
    }
    *p = 0;
    if ((size_t)(p - buf) + 1 > n) {
        errno = ENOSPC;
        return NULL;
    }
    return memcpy(dst, buf, (size_t)(p - buf) + 1);
}

WEAK const char *inet_ntop(int family, const void *restrict src,
                           char *restrict dst, socklen_t n)
{
    if (family == AF_INET) {
        const unsigned char *b = src;
        char buf[INET_ADDRSTRLEN];
        int k = snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if ((socklen_t)k + 1 > n) {
            errno = ENOSPC;
            return NULL;
        }
        return memcpy(dst, buf, (size_t)k + 1);
    }
    if (family == AF_INET6)
        return ntop6(src, dst, n);
    errno = EAFNOSUPPORT;
    return NULL;
}
