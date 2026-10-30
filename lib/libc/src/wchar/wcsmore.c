/* POSIX's and GNU's wide-string functions beyond C's: case-blind
 * comparison, stpcpy's wide twins, wcsdup, the bounded conversions, and
 * how many columns a character takes on a terminal. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define WEAK __attribute__((weak))

WEAK int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
    for (;; a++, b++) {
        wint_t x = towlower((wint_t)*a), y = towlower((wint_t)*b);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
}

WEAK int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        wint_t x = towlower((wint_t)*a), y = towlower((wint_t)*b);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
    return 0;
}

WEAK wchar_t *wcpcpy(wchar_t *restrict d, const wchar_t *restrict s)
{
    while ((*d = *s++))
        d++;
    return d;
}

/* n wide characters written, the tail filled with nulls; the result is
 * where the copy's terminator is, or d + n if it has none */
WEAK wchar_t *wcpncpy(wchar_t *restrict d, const wchar_t *restrict s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    wchar_t *end = d + i;
    for (; i < n; i++)
        d[i] = 0;
    return end;
}

WEAK wchar_t *wcsdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = malloc(n * sizeof *d);
    return d ? wmemcpy(d, s, n) : 0;
}

WEAK wchar_t *wmempcpy(wchar_t *restrict d, const wchar_t *restrict s, size_t n)
{
    return wmemcpy(d, s, n) + n;
}

WEAK wchar_t *wcschrnul(const wchar_t *s, wchar_t c)
{
    while (*s && *s != c)
        s++;
    return (wchar_t *)s;
}

WEAK wchar_t *wcswcs(const wchar_t *big, const wchar_t *little)
{
    return wcsstr(big, little);
}

/* mbsrtowcs reading at most nms bytes: a character cut off by the bound
 * is taken into *ps, to be finished by the next call */
WEAK size_t mbsnrtowcs(wchar_t *restrict d, const char **restrict s,
                       size_t nms, size_t len, mbstate_t *restrict ps)
{
    static mbstate_t own;
    if (!ps)
        ps = &own;
    const char *p = *s, *end = p + nms;
    size_t made = 0;
    while (p < end && (!d || made < len)) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, p, (size_t)(end - p), ps);
        if (r == (size_t)-1) {
            if (d)
                *s = p;
            return (size_t)-1;
        }
        if (r == (size_t)-2) {
            p = end;
            break;
        }
        if (d)
            d[made] = wc;
        if (wc == 0) {
            if (d)
                *s = 0;
            return made;
        }
        p += r;
        made++;
    }
    if (d)
        *s = p;
    return made;
}

/* wcsrtombs reading at most nwc wide characters */
WEAK size_t wcsnrtombs(char *restrict d, const wchar_t **restrict s,
                       size_t nwc, size_t len, mbstate_t *restrict ps)
{
    static mbstate_t own;
    if (!ps)
        ps = &own;
    const wchar_t *p = *s;
    size_t made = 0;
    char buf[MB_LEN_MAX > 8 ? MB_LEN_MAX : 8];
    for (; nwc; nwc--, p++) {
        size_t r = wcrtomb(buf, *p, ps);
        if (r == (size_t)-1) {
            if (d)
                *s = p;
            return (size_t)-1;
        }
        if (d) {
            if (made + r > len)
                break;
            memcpy(d + made, buf, r);
        }
        if (*p == 0) {
            if (d)
                *s = 0;
            return made;
        }
        made += r;
    }
    if (d)
        *s = p;
    return made;
}

/* ---- columns ------------------------------------------------------------------- */

#include "wcwidth-table.h"

/* 0 for a combining or zero-width character, 2 for a wide one, -1 for
 * one that is not printable, 1 for the rest: glibc's C.UTF-8 answers,
 * the whole table (wcwidth-table.h) read from it */
WEAK int wcwidth(wchar_t wc)
{
    uint32_t u = (uint32_t)wc;
    if (u < 0x7F)
        return u >= 0x20 ? 1 : u ? -1 : 0;
    size_t lo = 0, hi = sizeof g_ww / sizeof g_ww[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_ww[mid] >> 2 <= u)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return -1;
    int w = (int)(g_ww[lo - 1] & 3);
    return w == 3 ? -1 : w;
}

WEAK int wcswidth(const wchar_t *s, size_t n)
{
    int total = 0;
    for (; n && *s; n--, s++) {
        int w = wcwidth(*s);
        if (w < 0)
            return -1;
        total += w;
    }
    return total;
}
