/* <stdlib.h>'s XSI and GNU leftovers: radix-64 numbers, the old
 * floating-point-to-digits calls, arc4random, and a handful of small
 * ones -- secure_getenv, getloadavg, rpmatch, strtoq. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WEAK __attribute__((weak))

/* ---- radix 64: ./0-9A-Za-z, least significant digit first ------------------ */

static int digit64(char c)
{
    if (c == '.') return 0;
    if (c == '/') return 1;
    if (c >= '0' && c <= '9') return c - '0' + 2;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 12;
    if (c >= 'a' && c <= 'z') return c - 'a' + 38;
    return -1;
}

/* up to six digits, 32 bits of them, sign-extended as glibc's is */
WEAK long a64l(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 6 && s[i]; i++) {
        int d = digit64(s[i]);
        if (d < 0)
            break;
        v |= (uint32_t)d << (6 * i);
    }
    return (long)(int32_t)v;
}

WEAK char *l64a(long n)
{
    static const char abc[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static char buf[7];
    uint32_t v = (uint32_t)n;
    int i = 0;
    for (; v && i < 6; v >>= 6)
        buf[i++] = abc[v & 63];
    buf[i] = 0;
    return buf;
}

/* ---- ecvt, fcvt, gcvt: digits, and where the point goes ------------------------ */

/* The digits of |v| with `prec` of them after the point (fixed) or
 * `prec` significant ones (e), no point, no leading zeros, and the
 * position of the decimal point; into buf of n bytes. */
static int cvt(double v, int prec, int fixed, int *decpt, int *sign,
               char *buf, size_t n)
{
    char tmp[512];
    *sign = v < 0 || (v == 0 && 1 / v < 0);
    if (v < 0)
        v = -v;
    if (v != v || v == __builtin_inf()) {
        snprintf(buf, n, "%s", v != v ? "nan" : "inf");
        *decpt = 0;
        return 0;
    }
    if (prec < 0)
        prec = 0;
    if (fixed) {
        if (prec > 340)
            prec = 340;
        snprintf(tmp, sizeof tmp, "%.*f", prec, v);
        char *dot = strchr(tmp, '.');
        int ip = dot ? (int)(dot - tmp) : (int)strlen(tmp);
        if (dot)
            memmove(dot, dot + 1, strlen(dot));
        char *p = tmp;
        while (*p == '0' && p[1]) {        /* leading zeros move the point */
            p++;
            ip--;
        }
        if (*p == '0') {                   /* the value is 0: glibc's form, */
            *decpt = 1;                    /* "0" and prec more zeros */
            memset(tmp, '0', (size_t)prec + 1);
            tmp[prec + 1] = 0;
            p = tmp;
        } else {
            *decpt = ip;
        }
        if (strlen(p) + 1 > n) {
            errno = ERANGE;
            return -1;
        }
        memmove(buf, p, strlen(p) + 1);
        return 0;
    }
    if (prec < 1)
        prec = 1;
    if (prec > 17 + 300)
        prec = 17 + 300;
    snprintf(tmp, sizeof tmp, "%.*e", prec - 1, v);
    char *e = strchr(tmp, 'e');
    int exp = atoi(e + 1);
    *e = 0;
    char *dot = strchr(tmp, '.');
    if (dot)
        memmove(dot, dot + 1, strlen(dot));
    *decpt = v == 0 ? 1 : exp + 1;
    if (strlen(tmp) + 1 > n) {
        errno = ERANGE;
        return -1;
    }
    memcpy(buf, tmp, strlen(tmp) + 1);
    return 0;
}

WEAK int ecvt_r(double v, int nd, int *restrict decpt, int *restrict sign,
                char *restrict buf, size_t n)
{
    return cvt(v, nd, 0, decpt, sign, buf, n);
}

WEAK int fcvt_r(double v, int nd, int *restrict decpt, int *restrict sign,
                char *restrict buf, size_t n)
{
    return cvt(v, nd, 1, decpt, sign, buf, n);
}

WEAK char *ecvt(double v, int nd, int *restrict decpt, int *restrict sign)
{
    static char buf[400];
    return cvt(v, nd, 0, decpt, sign, buf, sizeof buf) ? 0 : buf;
}

WEAK char *fcvt(double v, int nd, int *restrict decpt, int *restrict sign)
{
    static char buf[400];
    return cvt(v, nd, 1, decpt, sign, buf, sizeof buf) ? 0 : buf;
}

WEAK char *gcvt(double v, int nd, char *buf)
{
    sprintf(buf, "%.*g", nd < 1 ? 1 : nd, v);
    return buf;
}

/* ---- arc4random -------------------------------------------------------------------- *
 *
 * Every byte from the system's entropy (getentropy), asked for each time:
 * no state kept in the process means none for a fork to duplicate. Where
 * the system has no entropy at all the bytes come from a SplitMix64
 * stream seeded from the clock and the stack's address -- unpredictable
 * to nobody who can read those, which is why the system's is tried
 * first every time; arc4random has no way to report a failure. */
static void weak_bytes(unsigned char *p, size_t n)
{
    static uint64_t s;
    if (!s) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        s = (uint64_t)t.tv_nsec ^ (uint64_t)t.tv_sec << 32 ^
            (uint64_t)(uintptr_t)&t;
    }
    while (n) {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (int i = 0; i < 8 && n; i++, n--, z >>= 8)
            *p++ = (unsigned char)z;
    }
}

WEAK void arc4random_buf(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        size_t k = n < 256 ? n : 256;
        int saved = errno;
        if (getentropy(p, k))
            weak_bytes(p, k);
        errno = saved;
        p += k;
        n -= k;
    }
}

WEAK uint32_t arc4random(void)
{
    uint32_t v;
    arc4random_buf(&v, sizeof v);
    return v;
}

/* uniform below bound: values in the short last stretch are drawn again */
WEAK uint32_t arc4random_uniform(uint32_t bound)
{
    if (bound < 2)
        return 0;
    uint32_t min = -bound % bound;
    for (;;) {
        uint32_t r = arc4random();
        if (r >= min)
            return r % bound;
    }
}

/* ---- the small ones ------------------------------------------------------------------- */

/* getenv, unless the program runs with privileges it was not started
 * with: there an environment is the invoker's to set, and not trusted */
WEAK char *secure_getenv(const char *name)
{
    if (getuid() != geteuid() || getgid() != getegid())
        return 0;
    return getenv(name);
}

/* the load averages from /proc/loadavg, where there is one */
WEAK int getloadavg(double loads[], int n)
{
    if (n < 0)
        return -1;
    if (n > 3)
        n = 3;
    int fd = open("/proc/loadavg", O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[64];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0)
        return -1;
    buf[k] = 0;
    char *p = buf;
    int i = 0;
    for (; i < n; i++) {
        char *end;
        loads[i] = strtod(p, &end);
        if (end == p)
            break;
        p = end;
    }
    return i;
}

/* 1 yes, 0 no, -1 neither, as the "C" locale's YESEXPR and NOEXPR say:
 * ^[yY] and ^[nN] */
WEAK int rpmatch(const char *response)
{
    if (*response == 'y' || *response == 'Y')
        return 1;
    if (*response == 'n' || *response == 'N')
        return 0;
    return -1;
}

WEAK long long strtoq(const char *restrict s, char **restrict end, int base)
{
    return strtoll(s, end, base);
}

WEAK unsigned long long strtouq(const char *restrict s, char **restrict end,
                                int base)
{
    return strtoull(s, end, base);
}

WEAK void cfree(void *p) { free(p); }
