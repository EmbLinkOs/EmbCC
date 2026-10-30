/* XSI's strfmon in the one locale there is: no currency symbol, no
 * grouping, '.' between the units and the fraction, two digits of
 * fraction (C's frac_digits is CHAR_MAX, "not given", and two is what
 * glibc uses then).
 *
 *   %[flags][width][#left][.right]{i,n}
 *   flags: =f fill character, ^ no grouping, + or ( how a negative
 *          shows, ! no currency symbol, - left-justified */
#define _GNU_SOURCE 1
#include <errno.h>
#include <locale.h>
#include <monetary.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEAK __attribute__((weak))

static ssize_t vfmon(char *restrict s, size_t max, const char *restrict fmt,
                     va_list ap)
{
    size_t n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || p[1] == '%') {
            if (n + 1 >= max)
                goto big;
            s[n++] = *p;
            if (*p == '%')
                p++;
            continue;
        }
        p++;
        char fill = ' ';
        int paren = 0, left = 0;
        for (;; p++) {
            if (*p == '=' && p[1]) {
                fill = *++p;
            } else if (*p == '(') {
                paren = 1;
            } else if (*p == '-') {
                left = 1;
            } else if (*p == '^' || *p == '+' || *p == '!') {
                ;                  /* nothing to group, no symbol to drop */
            } else {
                break;
            }
        }
        int width = 0, lprec = -1, rprec = 2;
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        if (*p == '#') {
            lprec = 0;
            for (p++; *p >= '0' && *p <= '9'; p++)
                lprec = lprec * 10 + (*p - '0');
        }
        if (*p == '.') {
            rprec = 0;
            for (p++; *p >= '0' && *p <= '9'; p++)
                rprec = rprec * 10 + (*p - '0');
        }
        if (*p != 'i' && *p != 'n') {
            errno = EINVAL;
            return -1;
        }
        double v = va_arg(ap, double);
        int neg = v < 0;
        char num[400], body[480];
        snprintf(num, sizeof num, "%.*f", rprec, neg ? -v : v);
        /* the units padded on the left with the fill to #left digits */
        int units = (int)strcspn(num, ".");
        int pad = lprec > units ? lprec - units : 0;
        size_t b = 0;
        if (neg && !paren)
            body[b++] = '-';
        else if (neg)
            body[b++] = '(';
        else if (lprec >= 0)
            body[b++] = ' ';   /* where the sign would be, so columns align */
        for (int i = 0; i < pad && b < sizeof body - 2; i++)
            body[b++] = fill;
        for (const char *q = num; *q && b < sizeof body - 2; q++)
            body[b++] = *q;
        if (paren && neg)
            body[b++] = ')';
        body[b] = 0;
        size_t len = strlen(body);
        size_t spaces = (size_t)width > len ? (size_t)width - len : 0;
        if (n + len + spaces >= max)
            goto big;
        if (!left)
            for (; spaces; spaces--)
                s[n++] = ' ';
        memcpy(s + n, body, len);
        n += len;
        for (; spaces; spaces--)
            s[n++] = ' ';
    }
    if (n >= max)
        goto big;
    s[n] = 0;
    return (ssize_t)n;
big:
    errno = E2BIG;
    return -1;
}

WEAK ssize_t strfmon(char *restrict s, size_t max, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ssize_t r = vfmon(s, max, fmt, ap);
    va_end(ap);
    return r;
}

WEAK ssize_t strfmon_l(char *restrict s, size_t max, locale_t l,
                       const char *restrict fmt, ...)
{
    (void)l;
    va_list ap;
    va_start(ap, fmt);
    ssize_t r = vfmon(s, max, fmt, ap);
    va_end(ap);
    return r;
}
