/* The formatted-input engine, shared by every scanf.
 *
 * printf's mirror image, and built the same way: one engine over a pair of
 * callbacks so a FILE and a string are the same thing to it. The callbacks
 * are get/unget rather than a sink because scanning is inherently
 * one-character-lookahead -- "%d" on "12x" has to read the 'x' to learn
 * the number ended, and then put it back.
 *
 * Exactly one character of pushback is ever needed, which is the whole
 * reason a scanf can be written against ungetc() at all. The one place
 * that is not obviously true is a failed "%d" on "-x": the '-' and the 'x'
 * are both consumed before the conversion is known to have failed, and the
 * standard resolves it by saying the input is left in an unspecified
 * position rather than requiring two-character pushback (§7.21.6.2p9).
 */
#include "file.h"
#include "scan.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../internal/utf8.h"

/* ---- the assignment targets -------------------------------------------- */

void __scan_store_int(va_list *ap, int len, unsigned long long v, int neg)
{
    unsigned long long u = neg ? (unsigned long long)-(long long)v : v;
    switch (len) {
    case L_CHAR:    *va_arg(*ap, char *) = (char)u; break;
    case L_SHORT:   *va_arg(*ap, short *) = (short)u; break;
    case L_LONG:    *va_arg(*ap, long *) = (long)u; break;
    case L_LLONG:   *va_arg(*ap, long long *) = (long long)u; break;
    case L_INTMAX:  *va_arg(*ap, intmax_t *) = (intmax_t)u; break;
    case L_SIZE:    *va_arg(*ap, size_t *) = (size_t)u; break;
    case L_PTRDIFF: *va_arg(*ap, ptrdiff_t *) = (ptrdiff_t)u; break;
    default:        *va_arg(*ap, int *) = (int)u; break;
    }
}

static void store_flt(va_list *ap, int len, long double v)
{
    switch (len) {
    case L_LDOUBLE: *va_arg(*ap, long double *) = v; break;
    case L_DOUBLE:  *va_arg(*ap, double *) = (double)v; break;
    default:        *va_arg(*ap, float *) = (float)v; break;
    }
}

/* ---- the conversions ---------------------------------------------------- */

static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* Scan an integer. Returns 1 on success, 0 on matching failure. `base` of
 * 0 means "look at the prefix", which is what %i does. */
int __scan_int(struct scan *s, int base, int width, int assign, va_list *ap,
               int len)
{
    int c, neg = 0, any = 0;
    unsigned long long v = 0;

    c = sgetc(s);
    if ((c == '-' || c == '+') && width) {
        neg = c == '-';
        width--;
        c = sgetc(s);
    }
    /* The 0x prefix is part of the number for base 16 and for base 0, and
     * is NOT for base 10 -- "0x1f" read as %d is the single digit 0 with
     * "x1f" left behind, which is a matching success, not a failure. C23's
     * 0b is the same for base 2 (%b) and base 0. */
    if (width && c == '0') {
        any = 1;                       /* the 0 itself is already a match */
        width--;
        c = sgetc(s);
        int px = (c == 'x' || c == 'X') && (base == 16 || base == 0);
        int pb = (c == 'b' || c == 'B') && (base == 2 || base == 0);
        if (width && (px || pb)) {
            base = px ? 16 : 2;
            width--;
            c = sgetc(s);
            any = 0;                   /* "0x" with no digit: back to the 0 */
            if (digit_val(c) >= base) {
                sungetc(s, c);
                if (assign) __scan_store_int(ap, len, 0, 0);
                return 1;              /* matched just the "0" */
            }
        } else if (base == 0) {
            base = 8;
        }
    } else if (base == 0) {
        base = 10;
    }

    while (width-- && digit_val(c) < base) {
        v = v * (unsigned)base + (unsigned)digit_val(c);
        any = 1;
        c = sgetc(s);
    }
    sungetc(s, c);
    if (!any)
        return 0;
    /* %u on "-1" is defined to store UINT_MAX, which is the same
     * two's-complement pattern as -1: signed or not, the bits stored are
     * the same, and only what the caller may then read differs. */
    if (assign)
        __scan_store_int(ap, len, v, neg);
    return 1;
}

/* Scan a float. Handles decimal, hex (0x1.8p3), inf and nan, because
 * printf's %a can produce all four and a scanf that cannot read back what
 * its printf wrote is a bug waiting for someone's save file. */
int __scan_flt(struct scan *s, int width, int assign, va_list *ap, int len)
{
    int c = sgetc(s), neg = 0, any = 0;
    long double v = 0;

    if ((c == '-' || c == '+') && width) {
        neg = c == '-';
        width--;
        c = sgetc(s);
    }

    /* inf / infinity / nan, case-insensitively. Matching the longer form
     * greedily is why this checks "infinity" before settling for "inf". */
    if (width && (c == 'i' || c == 'I' || c == 'n' || c == 'N')) {
        static const char *words[2] = { "infinity", "nan" };
        int w = (c == 'i' || c == 'I') ? 0 : 1;
        const char *word = words[w];
        int matched = 0;
        while (matched < 8 && word[matched] && width &&
               c >= 0 && c < 128 && tolower(c) == word[matched]) {
            matched++;
            width--;
            c = sgetc(s);
        }
        sungetc(s, c);
        /* "inf" is a match; "infi" is not -- unwind to the longest legal
         * prefix. Only "inf" and "infinity" are legal, so at most five
         * characters ever have to go back, and there is only one slot. In
         * that case the input position is unspecified (§7.21.6.2p9) and
         * reporting a match for "inf" is the useful reading. */
        if (w == 0 && matched >= 3)
            v = __builtin_infl();
        else if (w == 1 && matched == 3)
            v = __builtin_nanl("");
        else
            return 0;
        if (assign) store_flt(ap, len, neg ? -v : v);
        return 1;
    }

    /* The rest is read into a buffer and converted by strtof, strtod or
     * strtold -- correctly rounded, at the type's own precision, and the
     * same answer the program's own strtod gives. The digits past the
     * buffer's 900 can only say whether the value is above what was kept
     * (strtod.c), so they are counted, and stand in as one nonzero digit
     * and an exponent. */
    char buf[960];
    int n = 0, kept = 0, sticky = 0, hex = 0, point = 0;
    long extra = 0;              /* integer-part digits not kept */
    if (neg)
        buf[n++] = '-';
    if (width && c == '0') {
        buf[n++] = '0';
        any = 1;
        width--;
        c = sgetc(s);
        if (width && (c == 'x' || c == 'X')) {
            hex = 1;
            any = 0;
            buf[n++] = 'x';
            width--;
            c = sgetc(s);
        }
    }
    int base = hex ? 16 : 10;
    while (width && (digit_val(c) < base || (c == '.' && !point))) {
        if (c == '.') {
            point = 1;
            buf[n++] = '.';
        } else if (kept < 900) {
            buf[n++] = (char)c;
            kept++;
            any = 1;
        } else {
            any = 1;
            if (c != '0')
                sticky = 1;
            if (!point)
                extra++;
        }
        width--;
        c = sgetc(s);
    }
    if (!any) {
        if (hex) {
            /* "0x" and no digit: the 0, the 'x' gone with no way back
             * (one character of pushback) -- the position is unspecified
             * by C, and the 0 is the useful reading. */
            sungetc(s, c);
            if (assign) store_flt(ap, len, neg ? -0.0L : 0.0L);
            return 1;
        }
        sungetc(s, c);
        return 0;
    }
    long ex = 0;
    if (width && ((hex && (c == 'p' || c == 'P')) ||
                  (!hex && (c == 'e' || c == 'E')))) {
        int save_w = width, eneg = 0;
        width--;
        c = sgetc(s);
        if (width && (c == '-' || c == '+')) {
            eneg = c == '-';
            width--;
            c = sgetc(s);
        }
        if (width && digit_val(c) < 10) {
            while (width && digit_val(c) < 10) {
                if (ex < 100000000)
                    ex = ex * 10 + digit_val(c);
                width--;
                c = sgetc(s);
            }
            if (eneg)
                ex = -ex;
        } else {
            /* "1e" with no exponent digits: the 'e' is not part of the
             * number. One pushback slot, so the caller sees the character
             * after it; the standard allows that. */
            width = save_w;
        }
    }
    sungetc(s, c);
    if (sticky) {
        if (!point)
            buf[n++] = '.';
        buf[n++] = '1';
    }
    ex += hex ? 4 * extra : extra;
    /* the exponent, written here: snprintf would bring printf in */
    buf[n++] = hex ? 'p' : 'e';
    if (ex < 0) {
        buf[n++] = '-';
        ex = -ex;
    }
    char t[12];
    int k = 0;
    do t[k++] = (char)('0' + ex % 10); while ((ex /= 10) && k < 12);
    while (k)
        buf[n++] = t[--k];
    buf[n] = 0;
    if (assign) {
        switch (len) {
        case L_LDOUBLE: *va_arg(*ap, long double *) = strtold(buf, NULL); break;
        case L_DOUBLE:  *va_arg(*ap, double *) = strtod(buf, NULL); break;
        default:        *va_arg(*ap, float *) = strtof(buf, NULL); break;
        }
    }
    return 1;
}

/* A pointer reads as a hex integer, written into a void* -- which is why
 * it cannot share the %x path: the target width is the pointer's, not the
 * length modifier's. */
int __scan_ptr(struct scan *s, int width, int assign, va_list *ap)
{
    uintptr_t v = 0;
    int any = 0, c = sgetc(s);
    if (c == '0' && width > 1) {
        int c2 = sgetc(s);
        if (c2 == 'x' || c2 == 'X') { width -= 2; c = sgetc(s); }
        else { sungetc(s, c2); any = 1; }
    }
    while (width-- && digit_val(c) < 16) {
        v = v * 16 + (unsigned)digit_val(c);
        any = 1;
        c = sgetc(s);
    }
    sungetc(s, c);
    if (!any)
        return 0;
    if (assign)
        *va_arg(*ap, void **) = (void *)v;
    return 1;
}

/* ---- the byte scanfs' %c, %s and %[ ----
 *
 * They store bytes, or with l the wide characters the bytes' UTF-8
 * decodes to: a byte that is no part of UTF-8, or a character the field
 * ends half way through, is a matching failure with errno EILSEQ. An
 * unassigned field decodes too, so a %*ls refuses what a %ls would. The
 * wide scanfs' are wide.c's. */
struct bdest {
    char *c;              /* or */
    wchar_t *w;
    size_t n;             /* elements stored */
    struct __utf8 st;
};

static void bd_open(struct bdest *d, int assign, int is_l, va_list *ap)
{
    memset(d, 0, sizeof *d);
    if (!assign)
        return;
    if (is_l) d->w = va_arg(*ap, wchar_t *);
    else      d->c = va_arg(*ap, char *);
}

static int bd_put(struct bdest *d, int is_l, int c)
{
    if (!is_l) {
        if (d->c) d->c[d->n] = (char)c;
        d->n++;
        return 1;
    }
    uint_least32_t wc;
    int r = __utf8_step(&d->st, (unsigned char)c, &wc);
    if (r < 0)
        return 0;
    if (r > 0) {
        if (d->w) d->w[d->n] = (wchar_t)wc;
        d->n++;
    }
    return 1;
}

static int bd_close(struct bdest *d, int is_l, int terminate)
{
    (void)is_l;
    if (d->st.want)
        return 0;
    if (terminate) {
        if (d->w) d->w[d->n] = 0;
        if (d->c) d->c[d->n] = 0;
    }
    return 1;
}

/* %[ as a 256-bit set. A '-' first or last is a literal '-'; between two
 * others it is a range. */
struct bset { unsigned char b[32]; };

static void bset_setup(struct bset *set, const char *at, const char *end)
{
    memset(set, 0, sizeof *set);
    for (const char *f = at; f < end; f++) {
        unsigned lo = (unsigned char)*f, hi = lo;
        if (f + 2 < end && f[1] == '-') {
            hi = (unsigned char)f[2];
            f += 2;
        }
        for (unsigned c = lo; c <= hi; c++)
            set->b[c / 8] |= (unsigned char)(1u << (c % 8));
    }
}

#define SCAN_CH char
#define SCAN_RUN scan_bytes
#define SCAN_FVAL(f) ((unsigned char)(f))
#define SCAN_FSPACE(f) isspace((unsigned char)(f))
#define SCAN_SPACE(c) isspace(c)
#define SCAN_DEST struct bdest
#define SCAN_DOPEN bd_open
#define SCAN_DPUT bd_put
#define SCAN_DCLOSE bd_close
#define SCAN_SET struct bset
#define SCAN_SETUP bset_setup
#define SCAN_IN(set, c) (((set)->b[(c) / 8] >> ((c) % 8)) & 1)
#include "scan-run.h"

int __vscan(int (*get)(void *), void (*unget)(void *, int), void *ctx,
            const char *fmt, va_list ap)
{
    struct scan s = { get, unget, ctx, 0, 0, 0, 0 };
    va_list cp;
    va_copy(cp, ap);
    int r = scan_bytes(&s, fmt, &cp);
    va_end(cp);
    return r;
}
