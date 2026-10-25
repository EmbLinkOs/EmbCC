/* The integer conversions, C11 §7.22.1.
 *
 * One engine: every one of atoi, strtol, strtoul and the long long forms is
 * this loop with a different range clamped at the end. Writing them
 * separately is how they drift -- a base-prefix rule fixed in one and not
 * the others.
 *
 * Saturation is the part that is easy to get wrong: on overflow C requires
 * the extreme value AND errno == ERANGE, and requires `end` to still point
 * past the whole numeral, not at the digit that overflowed.
 */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

static unsigned long long conv(const char *s, char **end, int base,
                               int *neg_out, int *overflow)
{
    const char *start = s;
    *overflow = 0;
    while (isspace((unsigned char)*s)) s++;

    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    *neg_out = neg;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0')                            { base = 8; }
        else                                              base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    const char *digits = s;
    unsigned long long acc = 0;
    const unsigned long long cutoff = (unsigned long long)-1 / (unsigned)base;
    const unsigned cutlim = (unsigned)((unsigned long long)-1 % (unsigned)base);
    for (;; s++) {
        unsigned c = (unsigned char)*s;
        unsigned d;
        if (isdigit(c))      d = c - '0';
        else if (isalpha(c)) d = (unsigned)(tolower((int)c) - 'a') + 10;
        else break;
        if (d >= (unsigned)base) break;
        if (acc > cutoff || (acc == cutoff && d > cutlim))
            *overflow = 1;                /* keep scanning: `end` must be right */
        else
            acc = acc * (unsigned)base + d;
    }
    if (s == digits) {
        /* No digits at all: C says `end` gets the ORIGINAL string, not the
         * position after the sign we hopefully consumed. */
        if (end) *end = (char *)start;
        return 0;
    }
    if (end) *end = (char *)s;
    return acc;
}

long strtol(const char *restrict s, char **restrict end, int base)
{
    int neg, ov;
    unsigned long long v = conv(s, end, base, &neg, &ov);
    if (ov || (!neg && v > LONG_MAX) ||
        (neg && v > (unsigned long long)LONG_MAX + 1)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? -(long)v : (long)v;
}

long long strtoll(const char *restrict s, char **restrict end, int base)
{
    int neg, ov;
    unsigned long long v = conv(s, end, base, &neg, &ov);
    if (ov || (!neg && v > LLONG_MAX) ||
        (neg && v > (unsigned long long)LLONG_MAX + 1)) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? -(long long)v : (long long)v;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base)
{
    int neg, ov;
    unsigned long long v = conv(s, end, base, &neg, &ov);
    if (ov || v > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    /* C really does say a negated unsigned conversion wraps. */
    return neg ? (unsigned long)(-(unsigned long)v) : (unsigned long)v;
}

unsigned long long strtoull(const char *restrict s, char **restrict end,
                            int base)
{
    int neg, ov;
    unsigned long long v = conv(s, end, base, &neg, &ov);
    if (ov) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? -v : v;
}

int       atoi(const char *s)  { return (int)strtol(s, NULL, 10); }
long      atol(const char *s)  { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }
