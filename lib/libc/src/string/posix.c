/* <string.h>'s POSIX functions beyond C's, and the GNU and BSD ones every
 * Unix C library has come to carry: strtok_r, strsep, strlcpy, strlcat,
 * memmem (POSIX.1-2024 took these three in), strcasestr, memrchr,
 * strchrnul, explicit_bzero.
 *
 * Weak, as in strings.c and for the same reason: none of these names is
 * reserved to the implementation, and a program that brings its own
 * strlcpy -- many do, from before C libraries had one -- keeps it. */
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* strtok with the position kept by the caller. *save is NOT required to
 * be meaningful when s is non-null, which is what lets a caller start a
 * second, nested tokenisation with its own pointer. */
__attribute__((weak)) char *strtok_r(char *restrict s, const char *restrict sep,
                                     char **restrict save)
{
    if (!s)
        s = *save;
    if (!s)
        return NULL;
    s += strspn(s, sep);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    char *end = s + strcspn(s, sep);
    if (*end)
        *end++ = 0;
    else
        end = NULL;
    *save = end;
    return s;
}

/* strtok's opposite in the one way that matters: two separators in a row
 * are an EMPTY field, not one separator. That is what parsing
 * "a::b" as three fields needs. */
__attribute__((weak)) char *strsep(char **sp, const char *sep)
{
    char *s = *sp;
    if (!s)
        return NULL;
    char *end = s + strcspn(s, sep);
    if (*end)
        *end++ = 0;
    else
        end = NULL;
    *sp = end;
    return s;
}

/* The length of the string it TRIED to make, which is how a caller asks
 * "did it fit": the result is >= n exactly when it was truncated. */
__attribute__((weak)) size_t strlcpy(char *restrict d, const char *restrict s,
                                     size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}

/* When d holds no NUL in its n bytes, it is not a string this can append
 * to: nothing is written and the result is n + strlen(s), still at least
 * n, so the caller sees a truncation and not a success. */
__attribute__((weak)) size_t strlcat(char *restrict d, const char *restrict s,
                                     size_t n)
{
    size_t have = strnlen(d, n);
    if (have == n)
        return n + strlen(s);
    return have + strlcpy(d + have, s, n - have);
}

/* The first place the n bytes of `needle` occur in the hn of `hay`. An
 * empty needle is found at the start, as strstr finds "". */
__attribute__((weak)) void *memmem(const void *hay, size_t hn,
                                   const void *needle, size_t nn)
{
    const unsigned char *h = hay, *n = needle;
    if (nn == 0)
        return (void *)h;
    if (nn > hn)
        return NULL;
    const unsigned char *last = h + (hn - nn);
    for (const unsigned char *p = h; p <= last; p++) {
        p = memchr(p, n[0], (size_t)(last - p) + 1);
        if (!p)
            return NULL;
        if (memcmp(p, n, nn) == 0)
            return (void *)p;
    }
    return NULL;
}

__attribute__((weak)) char *strcasestr(const char *h, const char *n)
{
    size_t k = strlen(n);
    for (; *h; h++)
        if (strncasecmp(h, n, k) == 0)
            return (char *)h;
    return k == 0 ? (char *)h : NULL;
}

__attribute__((weak)) void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--)
        if (*--p == (unsigned char)c)
            return (void *)p;
    return NULL;
}

/* strchr, except that not finding c answers the terminating NUL rather
 * than NULL -- so the result is always somewhere a loop can stop. */
__attribute__((weak)) char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}

/* bzero that the compiler may not delete as a dead store: the bytes are
 * a secret that is about to go out of scope, which is exactly the store
 * an optimizer proves useless. C23's memset_explicit is that guarantee,
 * and this is its BSD spelling. */
__attribute__((weak)) void explicit_bzero(void *s, size_t n)
{
    memset_explicit(s, 0, n);
}

/* Strings compared as versions, so "file9" sorts before "file10" --
 * glibc's state machine, whose order `ls -v` and versionsort users know:
 * a digit run is an integer unless it starts with 0, when it is a
 * fraction, and a fraction sorts before an integer. */
#define VS_N 0        /* not in a number */
#define VS_I 3        /* in an integer */
#define VS_F 6        /* in a fraction */
#define VS_Z 9        /* in leading zeros */
#define VS_CMP 2
#define VS_LEN 3

__attribute__((weak)) int strverscmp(const char *a, const char *b)
{
    static const unsigned char next[] = {
        /*        x     d     0 */
        /* N */ VS_N, VS_I, VS_Z,
        /* I */ VS_N, VS_I, VS_I,
        /* F */ VS_N, VS_F, VS_F,
        /* Z */ VS_N, VS_F, VS_Z,
    };
    static const signed char result[] = {
        /*       x/x     x/d     x/0     d/x     d/d     d/0     0/x     0/d     0/0 */
        /* N */ VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_LEN, VS_CMP, VS_CMP, VS_CMP, VS_CMP,
        /* I */ VS_CMP, -1,     -1,     +1,     VS_LEN, VS_LEN, +1,     VS_LEN, VS_LEN,
        /* F */ VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP,
        /* Z */ VS_CMP, +1,     +1,     -1,     VS_CMP, VS_CMP, -1,     VS_CMP, VS_CMP,
    };
    const unsigned char *p = (const unsigned char *)a,
                        *q = (const unsigned char *)b;
    if (p == q)
        return 0;
    unsigned c1 = *p++, c2 = *q++;
    int state = VS_N + (c1 == '0') + (isdigit(c1) != 0);
    int diff;
    while ((diff = (int)c1 - (int)c2) == 0) {
        if (!c1)
            return 0;
        state = next[state];
        c1 = *p++;
        c2 = *q++;
        state += (c1 == '0') + (isdigit(c1) != 0);
    }
    state = result[state * 3 + (c2 == '0') + (isdigit(c2) != 0)];
    switch (state) {
    case VS_CMP:
        return diff;
    case VS_LEN:
        while (isdigit(*p++))
            if (!isdigit(*q++))
                return 1;
        return isdigit(*q) ? -1 : diff;
    default:
        return state;
    }
}
