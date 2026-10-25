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

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Length modifiers, as a width in bytes plus a "this is a double family"
 * flag -- the same normalisation __vformat does on the way out. */
#define L_CHAR   1
#define L_SHORT  2
#define L_INT    3
#define L_LONG   4
#define L_LLONG  5
#define L_INTMAX 6
#define L_SIZE   7
#define L_PTRDIFF 8
#define L_DOUBLE 9        /* 'l' before a float conversion */
#define L_LDOUBLE 10

struct scan {
    int (*get)(void *);
    void (*unget)(void *, int);
    void *ctx;
    long nread;           /* characters consumed, for %n */
    int pushed;           /* a character is waiting in `back` */
    int back;
    int hit_eof;          /* an INPUT failure happened (end of input) */
};

static int sgetc(struct scan *s)
{
    int c;
    if (s->pushed) { c = s->back; s->pushed = 0; }
    else c = s->get(s->ctx);
    if (c != EOF)
        s->nread++;
    else
        s->hit_eof = 1;
    return c;
}

static void sungetc(struct scan *s, int c)
{
    if (c == EOF)
        return;
    s->back = c;
    s->pushed = 1;
    s->nread--;
}

/* ---- the assignment targets -------------------------------------------- */

static void store_int(va_list *ap, int len, unsigned long long v, int neg)
{
    unsigned long long u = neg ? (unsigned long long)-(long long)v : v;
    switch (len) {
    case L_CHAR:    *va_arg(*ap, char *) = (char)u; break;
    case L_SHORT:   *va_arg(*ap, short *) = (short)u; break;
    case L_LONG:    *va_arg(*ap, long *) = (long)u; break;
    case L_LLONG:   *va_arg(*ap, long long *) = (long long)u; break;
    case L_INTMAX:  *va_arg(*ap, long *) = (long)u; break;
    case L_SIZE:    *va_arg(*ap, size_t *) = (size_t)u; break;
    case L_PTRDIFF: *va_arg(*ap, long *) = (long)u; break;
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
static int scan_int(struct scan *s, int base, int width, int is_signed,
                    int assign, va_list *ap, int len)
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
     * "x1f" left behind, which is a matching success, not a failure. */
    if (width && c == '0') {
        any = 1;                       /* the 0 itself is already a match */
        width--;
        c = sgetc(s);
        if (width && (c == 'x' || c == 'X') && (base == 16 || base == 0)) {
            base = 16;
            width--;
            c = sgetc(s);
            any = 0;                   /* "0x" with no digit: back to the 0 */
            if (digit_val(c) >= 16) {
                sungetc(s, c);
                if (assign) store_int(ap, len, 0, 0);
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
    /* is_signed changes nothing about the bits stored -- %u on "-1" is
     * defined to store UINT_MAX, which is the same two's-complement
     * pattern as -1. It only affects what the caller may then read. */
    (void)is_signed;
    if (assign)
        store_int(ap, len, v, neg);
    return 1;
}

/* Scan a float. Handles decimal, hex (0x1.8p3), inf and nan, because
 * printf's %a can produce all four and a scanf that cannot read back what
 * its printf wrote is a bug waiting for someone's save file. */
static int scan_flt(struct scan *s, int width, int assign, va_list *ap,
                    int len)
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
               tolower(c) == word[matched]) {
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

    int hex = 0;
    if (width && c == '0') {
        any = 1;
        width--;
        c = sgetc(s);
        if (width && (c == 'x' || c == 'X')) {
            hex = 1;
            any = 0;
            width--;
            c = sgetc(s);
        }
    }

    int base = hex ? 16 : 10;
    long exp = 0;
    while (width && digit_val(c) < base) {
        v = v * base + digit_val(c);
        any = 1;
        width--;
        c = sgetc(s);
    }
    if (width && c == '.') {
        width--;
        c = sgetc(s);
        while (width && digit_val(c) < base) {
            v = v * base + digit_val(c);
            exp -= hex ? 4 : 1;
            any = 1;
            width--;
            c = sgetc(s);
        }
    }
    if (!any) {
        sungetc(s, c);
        return 0;
    }
    /* A decimal exponent is 'e', a hex one is 'p' and counts in powers of
     * two -- which is why `exp` above is already in bits for the hex case. */
    if (width && ((hex && (c == 'p' || c == 'P')) ||
                  (!hex && (c == 'e' || c == 'E')))) {
        int save_w = width;
        width--;
        c = sgetc(s);
        int eneg = 0;
        if (width && (c == '-' || c == '+')) {
            eneg = c == '-';
            width--;
            c = sgetc(s);
        }
        if (digit_val(c) < 10) {
            long e = 0;
            while (width && digit_val(c) < 10) {
                if (e < 100000) e = e * 10 + digit_val(c);
                width--;
                c = sgetc(s);
            }
            exp += eneg ? -e : e;
        } else {
            /* "1e" with no exponent digits: the 'e' is not part of the
             * number. One pushback slot, so the caller sees the character
             * after it; the standard allows that. */
            sungetc(s, c);
            width = save_w;
        }
    }
    sungetc(s, c);

    /* Scale by repeated squaring rather than a loop of multiplies: 1e300
     * is 300 multiplications the naive way, and each one adds rounding. */
    long double base_f = hex ? 2.0L : 10.0L;
    long n = exp < 0 ? -exp : exp;
    long double scale = 1.0L, p = base_f;
    while (n) {
        if (n & 1) scale *= p;
        p *= p;
        n >>= 1;
    }
    v = exp < 0 ? v / scale : v * scale;

    if (assign)
        store_flt(ap, len, neg ? -v : v);
    return 1;
}

/* %[...]: build the 256-bit set, then read while members last. */
static const char *scan_set(struct scan *s, const char *f, int width,
                            int assign, va_list *ap)
{
    unsigned char set[32];
    memset(set, 0, sizeof set);
    int negate = 0;
    if (*f == '^') { negate = 1; f++; }
    /* A ']' FIRST is a literal ']', not the terminator -- the one piece of
     * %[ syntax everyone forgets. */
    const char *first = f;
    if (*f == ']') { set[']' / 8] |= (unsigned char)(1 << (']' % 8)); f++; }
    while (*f && *f != ']') {
        /* `a-z` is a range; a '-' that is FIRST or LAST in the set is a
         * literal '-'. `first` is what makes that testable -- using f[-1]
         * would read the '[' or the '^' and build a range from it. */
        if (f[0] == '-' && f > first && f[1] && f[1] != ']') {
            for (int c = (unsigned char)f[-1] + 1; c <= (unsigned char)f[1];
                 c++)
                set[c / 8] |= (unsigned char)(1 << (c % 8));
            f += 2;
            continue;
        }
        set[(unsigned char)*f / 8] |= (unsigned char)(1 << ((unsigned char)*f % 8));
        f++;
    }
    if (*f == ']')
        f++;

    char *out = assign ? va_arg(*ap, char *) : NULL;
    int n = 0, c;
    while (width-- && (c = sgetc(s)) != EOF) {
        int in = (set[c / 8] >> (c % 8)) & 1;
        if (in == negate) { sungetc(s, c); break; }
        if (out) out[n] = (char)c;
        n++;
    }
    if (n == 0)
        return NULL;                   /* matching failure */
    if (out)
        out[n] = 0;
    return f;
}

int __vscan(int (*get)(void *), void (*unget)(void *, int), void *ctx,
            const char *fmt, va_list ap)
{
    struct scan s;
    s.get = get; s.unget = unget; s.ctx = ctx;
    s.nread = 0; s.pushed = 0; s.back = 0; s.hit_eof = 0;

    int assigned = 0, c;

    for (const char *f = fmt; *f; f++) {
        /* Whitespace in the format matches any run of whitespace, INCLUDING
         * none at all -- so a trailing " " in a format never fails. */
        if (isspace((unsigned char)*f)) {
            while ((c = sgetc(&s)) != EOF && isspace(c))
                ;
            sungetc(&s, c);
            continue;
        }
        if (*f != '%') {
            c = sgetc(&s);
            if (c != (unsigned char)*f) {
                sungetc(&s, c);
                goto done;             /* matching failure */
            }
            continue;
        }

        f++;
        int assign = 1, width = INT_MAX, len = L_INT;
        if (*f == '*') { assign = 0; f++; }
        if (*f >= '0' && *f <= '9') {
            width = 0;
            while (*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');
            if (width == 0)
                goto done;             /* %0d: undefined; refuse to guess */
        }
        switch (*f) {
        case 'h': f++; if (*f == 'h') { len = L_CHAR; f++; } else len = L_SHORT;
                  break;
        case 'l': f++; if (*f == 'l') { len = L_LLONG; f++; } else len = L_LONG;
                  break;
        case 'j': len = L_INTMAX; f++; break;
        case 'z': len = L_SIZE; f++; break;
        case 't': len = L_PTRDIFF; f++; break;
        case 'L': len = L_LDOUBLE; f++; break;
        default: break;
        }

        /* Every conversion but %c, %[ and %n skips leading whitespace
         * first, and that skipping does NOT count as input for the
         * "EOF before any conversion" test below. */
        int conv = *f;
        if (conv != 'c' && conv != '[' && conv != 'n') {
            while ((c = sgetc(&s)) != EOF && isspace(c))
                ;
            if (c == EOF)
                goto done;
            sungetc(&s, c);
        }

        switch (conv) {
        case 'd': case 'u':
            if (!scan_int(&s, 10, width, conv == 'd', assign, &ap, len))
                goto done;
            assigned += assign;
            break;
        case 'i':
            if (!scan_int(&s, 0, width, 1, assign, &ap, len))
                goto done;
            assigned += assign;
            break;
        case 'o':
            if (!scan_int(&s, 8, width, 0, assign, &ap, len))
                goto done;
            assigned += assign;
            break;
        case 'x': case 'X':
            if (!scan_int(&s, 16, width, 0, assign, &ap, len))
                goto done;
            assigned += assign;
            break;
        case 'p': {
            /* A pointer reads as a hex integer, written into a void* --
             * which is why it cannot share the %x path: the target width
             * is the pointer's, not the length modifier's. */
            unsigned long v = 0;
            int any = 0;
            c = sgetc(&s);
            if (c == '0' && width > 1) {
                int c2 = sgetc(&s);
                if (c2 == 'x' || c2 == 'X') { width -= 2; c = sgetc(&s); }
                else { sungetc(&s, c2); any = 1; }
            }
            while (width-- && digit_val(c) < 16) {
                v = v * 16 + (unsigned)digit_val(c);
                any = 1;
                c = sgetc(&s);
            }
            sungetc(&s, c);
            if (!any)
                goto done;
            if (assign)
                *va_arg(ap, void **) = (void *)v;
            assigned += assign;
            break;
        }
        case 'a': case 'A': case 'e': case 'E':
        case 'f': case 'F': case 'g': case 'G':
            /* The float conversions all read the same syntax; they differ
             * only in printf. L_LONG here means `double`, not `long`. */
            if (!scan_flt(&s, width, assign, &ap,
                          len == L_LONG ? L_DOUBLE :
                          len == L_LDOUBLE ? L_LDOUBLE : 0))
                goto done;
            assigned += assign;
            break;
        case 'c': {
            /* No whitespace skip, and no terminating NUL: %c reads exactly
             * `width` characters (default 1) into a char array. */
            int n = width == INT_MAX ? 1 : width;
            char *out = assign ? va_arg(ap, char *) : NULL;
            int got = 0;
            while (got < n && (c = sgetc(&s)) != EOF) {
                if (out) out[got] = (char)c;
                got++;
            }
            if (got < n)
                goto done;
            assigned += assign;
            break;
        }
        case 's': {
            char *out = assign ? va_arg(ap, char *) : NULL;
            int n = 0;
            while (width-- && (c = sgetc(&s)) != EOF && !isspace(c)) {
                if (out) out[n] = (char)c;
                n++;
            }
            sungetc(&s, c);
            if (n == 0)
                goto done;
            if (out) out[n] = 0;
            assigned += assign;
            break;
        }
        case '[': {
            const char *nf = scan_set(&s, f + 1, width, assign, &ap);
            if (!nf)
                goto done;
            f = nf - 1;                /* the loop's f++ steps past ']' */
            assigned += assign;
            break;
        }
        case 'n':
            /* §7.21.6.2p12: %n is not a conversion, so it does not count
             * toward the return value and cannot cause a failure. */
            if (assign)
                store_int(&ap, len, (unsigned long long)s.nread, 0);
            break;
        case '%':
            c = sgetc(&s);
            if (c != '%') { sungetc(&s, c); goto done; }
            break;
        default:
            goto done;                 /* an unknown conversion stops here */
        }
    }

done:
    /* §7.21.6.2p16 distinguishes two failures, and callers rely on it:
     * an INPUT failure (the input ended) before the first conversion
     * completed returns EOF, while a MATCHING failure (there was input, it
     * was the wrong shape) returns the number assigned, which may be 0.
     *
     * So the test is "did we reach end of input", not "did we consume
     * nothing" -- sscanf("x", "%d", &n) consumes nothing and must still
     * return 0, because there is an 'x' there to be re-read. */
    if (assigned == 0 && s.hit_eof)
        return EOF;
    return assigned;
}
