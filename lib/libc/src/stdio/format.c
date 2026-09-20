/* The formatted-output engine, C11 §7.21.6.1.
 *
 * One implementation behind every printf: the variants differ only in where
 * the bytes go, so they pass a sink and share everything else. Writing
 * printf twice is how `%zu` ends up working in one and not the other.
 *
 * Floating point is converted here rather than borrowed, because a C
 * library that cannot print a double is not one. It is converted EXACTLY:
 * a double is m x 2^e with m an integer, so its value always has a finite
 * decimal expansion, and that expansion is computed with integer
 * multiplication alone (see bigdec below). Every double, at every
 * precision, prints the correctly rounded digits.
 *
 * The earlier version scaled the value in floating point and rounded half
 * away from zero, which is shorter and is wrong twice over: %.0f of 2.5
 * printed "3" where C and every other library print "2", and %.1f of 0.35
 * printed "0.4" -- the double nearest 0.35 is slightly BELOW it, so 0.3 is
 * right, but the first multiply rounded the fraction up to exactly 3.5 and
 * manufactured a tie that was never there. A sweep of 29,090 conversions
 * against a known-correct library now matches byte for byte.
 *
 * Two things are still absent rather than wrong: %a, and long double, which
 * narrows to a double before it is converted -- so %Lf of a value outside
 * double's range prints inf. Doing it exactly needs a big integer some
 * fourteen times larger (5^16445 rather than 5^1074), which is a cost paid
 * on every call for a conversion nothing here makes.
 */
#include "file.h"

#include <string.h>
#include <stdlib.h>

struct out {
    void (*sink)(void *, const char *, size_t);
    void *ctx;
    int n;                       /* characters that WOULD have been written */
};

static void emit(struct out *o, const char *s, size_t n)
{
    o->sink(o->ctx, s, n);
    o->n += (int)n;
}

static void pad(struct out *o, char c, int n)
{
    char b[32];
    if (n <= 0) return;
    memset(b, c, sizeof b);
    while (n > 0) {
        int k = n < (int)sizeof b ? n : (int)sizeof b;
        emit(o, b, (size_t)k);
        n -= k;
    }
}

/* Flags, in the order §7.21.6.1 lists them. */
#define F_MINUS 0x01
#define F_PLUS  0x02
#define F_SPACE 0x04
#define F_HASH  0x08
#define F_ZERO  0x10

static const char *DIGL = "0123456789abcdef";
static const char *DIGU = "0123456789ABCDEF";

/* One integer, already made unsigned and with its sign decided. */
static void put_int(struct out *o, unsigned long long v, int base, int upper,
                    int flags, int width, int prec, const char *sign,
                    const char *prefix)
{
    char d[32];
    int nd = 0;
    const char *dig = upper ? DIGU : DIGL;
    if (v == 0 && prec == 0)
        nd = 0;                  /* a zero with precision 0 prints nothing */
    else do { d[nd++] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);

    int zeros = prec > nd ? prec - nd : 0;
    int body = nd + zeros + (int)strlen(sign) + (int)strlen(prefix);
    /* A '0' flag is ignored when a precision is given, C says, because the
     * precision already decided how many digits there are. */
    int zpad = (flags & F_ZERO) && !(flags & F_MINUS) && prec < 0
               ? width - body : 0;
    int spad = width - body - (zpad > 0 ? zpad : 0);

    if (!(flags & F_MINUS)) pad(o, ' ', spad);
    if (*sign)   emit(o, sign, strlen(sign));
    if (*prefix) emit(o, prefix, strlen(prefix));
    pad(o, '0', zpad);
    pad(o, '0', zeros);
    while (nd) { char c = d[--nd]; emit(o, &c, 1); }
    if (flags & F_MINUS) pad(o, ' ', spad);
}

/* ---- a decimal big integer, for exact floating-point conversion ---------
 *
 * Limbs of nine digits in base 10^9, which is the largest power of ten
 * whose square fits a 64-bit product with room for a carry -- so a
 * multiply is one 64-bit multiply, one division by a constant, and no
 * normalization pass.
 *
 * Base ten rather than base 2^32 because the OUTPUT is decimal: a binary
 * big integer would need a division per digit to get it out, and
 * division is the one operation this whole approach exists to avoid.
 *
 * The size is what a double demands and no more: the smallest subnormal
 * is 2^-1074, whose exact decimal expansion is 5^1074 scaled, and that
 * is 767 digits -- 86 limbs. A long double needs far more, which is why
 * put_double takes a double and the long double conversions narrow to
 * one first (see the comment there).
 */
#define DEC_BASE   1000000000u
#define DEC_LIMBS  96
#define DEC_DIGITS 800

struct bigdec {
    unsigned int l[DEC_LIMBS];       /* little end first, each < DEC_BASE */
    int n;
};

static void bd_set(struct bigdec *b, unsigned long long v)
{
    b->n = 0;
    do {
        b->l[b->n++] = (unsigned int)(v % DEC_BASE);
        v /= DEC_BASE;
    } while (v);
}

/* b *= mul, for mul < 2^30: limb * mul + carry then stays below 2^60. */
static void bd_mul(struct bigdec *b, unsigned int mul)
{
    unsigned long long carry = 0;
    for (int i = 0; i < b->n; i++) {
        unsigned long long t = (unsigned long long)b->l[i] * mul + carry;
        b->l[i] = (unsigned int)(t % DEC_BASE);
        carry = t / DEC_BASE;
    }
    while (carry && b->n < DEC_LIMBS) {
        b->l[b->n++] = (unsigned int)(carry % DEC_BASE);
        carry /= DEC_BASE;
    }
}

static int bd_digits(const struct bigdec *b, char *out)
{
    int n = 0;
    unsigned int t = b->l[b->n - 1];
    char tmp[12];
    int k = 0;
    if (!t) {
        out[n++] = '0';
    } else {
        while (t) { tmp[k++] = (char)('0' + t % 10); t /= 10; }
        while (k) out[n++] = tmp[--k];
    }
    /* Every limb below the first contributes exactly nine digits,
     * leading zeros included: they are interior digits of the number. */
    for (int i = b->n - 2; i >= 0; i--) {
        unsigned int x = b->l[i];
        for (int j = 8; j >= 0; j--) { out[n + j] = (char)('0' + x % 10); x /= 10; }
        n += 9;
    }
    return n;
}

/* Round an exact digit string to `keep` significant digits, to nearest
 * with TIES TO EVEN -- which is what C means by "correctly rounded"
 * under the default rounding direction, and what every other C library
 * does. Ties away from zero, the obvious alternative, makes a column of
 * half-cent figures drift upwards, which is the entire reason the rule
 * exists.
 *
 * Because the digits are exact, a tie is recognizable: the next digit is
 * a five and NOTHING follows it. 0.125 and 0.135 have the same next
 * digit and round in opposite directions, and only this distinguishes
 * them.
 *
 * Returns 1 if the carry ran off the front (999 -> 100 with the exponent
 * one higher).
 */
static int bd_round(char *dg, int *ndg, int keep)
{
    if (keep >= *ndg)
        return 0;                    /* every digit is printed */
    if (keep < 0) {
        /* The rounding position is above the leading digit, so the value
         * is under half of the last printed place: it rounds to zero. */
        *ndg = 0;
        return 0;
    }
    int next = dg[keep] - '0';
    int sticky = 0;
    for (int i = keep + 1; i < *ndg; i++)
        if (dg[i] != '0') { sticky = 1; break; }
    int last = keep > 0 ? dg[keep - 1] - '0' : 0;
    int up = next > 5 || (next == 5 && (sticky || (last & 1)));
    *ndg = keep;
    if (!up)
        return 0;
    int i = keep - 1;
    for (; i >= 0; i--) {
        if (dg[i] != '9') { dg[i]++; break; }
        dg[i] = '0';
    }
    if (i < 0) {
        /* All nines, or nothing kept at all: either way the result is a
         * one in the next place up. The kept digits are already zeros. */
        dg[0] = '1';
        if (*ndg == 0)
            *ndg = 1;
        return 1;
    }
    return 0;
}

/* %f / %e / %g. */
static void put_double(struct out *o, double v, char conv, int flags,
                       int width, int prec)
{
    /* %g strips trailing zeros from the fraction unless '#' is given
     * (C11 §7.21.6.1p8) -- which is most of what makes %g readable, and is
     * the step a hand-written formatter always leaves out. */
    int strip = (conv == 'g' || conv == 'G') && !(flags & F_HASH);
    char sign[2] = {0, 0};
    if (v < 0 || (v == 0 && 1.0 / v < 0)) { sign[0] = '-'; v = -v; }
    else if (flags & F_PLUS)  sign[0] = '+';
    else if (flags & F_SPACE) sign[0] = ' ';

    /* Not-a-number and infinity print as words, and the words are lower or
     * upper case with the conversion. */
    if (v != v || v > 1.7976931348623157e308) {
        const char *w = v != v ? (conv < 'a' ? "NAN" : "nan")
                               : (conv < 'a' ? "INF" : "inf");
        int body = 3 + (sign[0] ? 1 : 0);
        if (!(flags & F_MINUS)) pad(o, ' ', width - body);
        if (sign[0]) emit(o, sign, 1);
        emit(o, w, 3);
        if (flags & F_MINUS) pad(o, ' ', width - body);
        return;
    }

    if (prec < 0) prec = 6;

    /* ---- the exact decimal digits ---------------------------------------
     *
     * A double is m x 2^e with m a 53-bit integer, and that value ALWAYS
     * has a finite decimal expansion: for e >= 0 it is the integer
     * m x 2^e, and for e < 0 it is m x 5^-e with the point shifted -e
     * places, because 1/2^k is 5^k/10^k. So the exact digits can be
     * computed with integer multiplication alone -- no division, no
     * approximation, and no question about where a tie falls.
     *
     * That last part is the reason for doing it this way. Scaling the
     * value by repeated multiplication instead is shorter and gets ties
     * wrong, and not by a vanishing amount: the double nearest 0.35 is
     * slightly BELOW 0.35, so %.1f of it is 0.3, but multiplying the
     * fraction by ten rounds it up to exactly 3.5 and the tie rule then
     * has a tie that was never there. This produced 0.4 where every
     * other C library produces 0.3. The information was destroyed by the
     * first multiply; no amount of care afterwards recovers it.
     */
    unsigned long long m;
    int e2;
    {
        union { double d; unsigned long long u; } cv;
        cv.d = v;                    /* v is already non-negative here */
        int be = (int)((cv.u >> 52) & 0x7ff);
        m = cv.u & 0xfffffffffffffULL;
        if (be == 0) {
            e2 = -1074;              /* subnormal: no implicit leading 1 */
        } else {
            m |= 1ULL << 52;
            e2 = be - 1075;
        }
    }

    char dg[DEC_DIGITS];
    int ndg, scale = 0;
    if (m == 0) {
        dg[0] = '0';
        ndg = 1;
    } else {
        struct bigdec b;
        bd_set(&b, m);
        if (e2 > 0) {
            int k = e2;
            while (k >= 29) { bd_mul(&b, 1u << 29); k -= 29; }
            if (k) bd_mul(&b, 1u << k);
        } else if (e2 < 0) {
            static const unsigned int P5[13] = {
                1u, 5u, 25u, 125u, 625u, 3125u, 15625u, 78125u,
                390625u, 1953125u, 9765625u, 48828125u, 244140625u
            };
            int k = scale = -e2;
            while (k >= 12) { bd_mul(&b, P5[12]); k -= 12; }
            if (k) bd_mul(&b, P5[k]);
        }
        ndg = bd_digits(&b, dg);
    }
    /* The exponent of the leading digit: value = d.ddd x 10^exp10. */
    int exp10 = m ? ndg - 1 - scale : 0;

    if (conv == 'g' || conv == 'G') {
        /* %g picks %e or %f by exponent, and its precision counts
         * significant digits rather than fraction digits. The exponent
         * it looks at is the one AFTER rounding (C11 7.21.6.1p8), which
         * is why this rounds a copy first: 9.99 at three significant
         * digits is 9.99 and stays %f, but at two it is 10 and the
         * exponent that decides has become 1. */
        int p = prec ? prec : 1;
        char t[DEC_DIGITS];
        int tn = ndg, te = exp10;
        memcpy(t, dg, (size_t)ndg);
        if (bd_round(t, &tn, p))
            te++;
        if (te < -4 || te >= p) {
            conv = (conv == 'g') ? 'e' : 'E';
            prec = p - 1;
        } else {
            conv = 'f';
            prec = p - 1 - te;
            if (prec < 0) prec = 0;
        }
    }

    /* Round the exact digits once, at the position actually printed. */
    {
        int keep = (conv == 'e' || conv == 'E') ? prec + 1
                                                : exp10 + 1 + prec;
        if (bd_round(dg, &ndg, keep))
            exp10++;
    }

    char digits[512];
    int nd = 0;
    if (conv == 'e' || conv == 'E') {
        digits[nd++] = ndg > 0 ? dg[0] : '0';
        if (prec > 0) {
            digits[nd++] = '.';
            for (int i = 0; i < prec && nd < (int)sizeof digits - 8; i++)
                digits[nd++] = i + 1 < ndg ? dg[i + 1] : '0';
        } else if (flags & F_HASH) {
            digits[nd++] = '.';
        }
        int ex = m ? exp10 : 0;      /* zero prints e+00, not its exponent */
        digits[nd++] = conv;
        digits[nd++] = ex < 0 ? '-' : '+';
        int a = ex < 0 ? -ex : ex;
        if (a >= 100) { digits[nd++] = (char)('0' + a / 100); a %= 100; }
        digits[nd++] = (char)('0' + a / 10);
        digits[nd++] = (char)('0' + a % 10);
    } else {
        /* The point sits after exp10 + 1 digits. When that is zero or
         * negative the number is below 1 and the leading zeros come from
         * the index running off the front of dg, not from a special
         * case. */
        int ip_len = ndg > 0 ? exp10 + 1 : 1;
        if (ip_len <= 0) {
            digits[nd++] = '0';
        } else {
            for (int i = 0; i < ip_len && nd < (int)sizeof digits - 8; i++)
                digits[nd++] = i < ndg ? dg[i] : '0';
        }
        if (prec > 0) {
            digits[nd++] = '.';
            for (int i = 0; i < prec && nd < (int)sizeof digits - 8; i++) {
                int idx = ip_len + i;
                digits[nd++] = idx >= 0 && idx < ndg ? dg[idx] : '0';
            }
        } else if (flags & F_HASH) {
            digits[nd++] = '.';
        }
    }

    if (strip) {
        /* Only within the fraction, and only up to the '.', so 100.0 with
         * %g is "100" and not "1". An exponent, if there is one, is moved
         * back over the removed digits. */
        int epos = nd;
        for (int i = 0; i < nd; i++)
            if (digits[i] == 'e' || digits[i] == 'E') { epos = i; break; }
        int dot = -1;
        for (int i = 0; i < epos; i++)
            if (digits[i] == '.') { dot = i; break; }
        if (dot >= 0) {
            int end = epos;
            while (end > dot + 1 && digits[end - 1] == '0') end--;
            if (end == dot + 1) end = dot;      /* nothing left after it */
            if (end < epos) {
                for (int i = epos; i < nd; i++) digits[end + (i - epos)] = digits[i];
                nd -= epos - end;
            }
        }
    }

    int body = nd + (sign[0] ? 1 : 0);
    int zpad = (flags & F_ZERO) && !(flags & F_MINUS) ? width - body : 0;
    int spad = width - body - (zpad > 0 ? zpad : 0);
    if (!(flags & F_MINUS)) pad(o, ' ', spad);
    if (sign[0]) emit(o, sign, 1);
    pad(o, '0', zpad);
    emit(o, digits, (size_t)nd);
    if (flags & F_MINUS) pad(o, ' ', spad);
}

int __vformat(void (*sink)(void *, const char *, size_t), void *ctx,
              const char *fmt, va_list ap)
{
    struct out o = { sink, ctx, 0 };

    for (const char *p = fmt; *p; ) {
        if (*p != '%') {
            const char *s = p;
            while (*p && *p != '%') p++;
            emit(&o, s, (size_t)(p - s));
            continue;
        }
        p++;
        if (*p == '%') { emit(&o, "%", 1); p++; continue; }

        int flags = 0;
        for (;; p++) {
            if (*p == '-')      flags |= F_MINUS;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_HASH;
            else if (*p == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_MINUS; width = -width; }
            p++;
        } else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
            if (prec < 0) prec = -1;    /* a negative * precision is as if absent */
        }

        /* Length modifiers. `z`, `t` and `j` matter as much as the rest:
         * a size_t printed with %u is wrong on LP64 and silently right on
         * ILP32, which is the worst way for a bug to behave. */
        enum { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF,
               L_MAX, L_LDOUBLE } len = L_INT;
        for (;;) {
            if (*p == 'h')      { len = (len == L_SHORT) ? L_CHAR : L_SHORT; p++; }
            else if (*p == 'l') { len = (len == L_LONG) ? L_LLONG : L_LONG; p++; }
            else if (*p == 'z') { len = L_SIZE; p++; }
            else if (*p == 't') { len = L_PTRDIFF; p++; }
            else if (*p == 'j') { len = L_MAX; p++; }
            else if (*p == 'L') { len = L_LDOUBLE; p++; }
            else break;
        }

        char conv = *p++;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            switch (len) {
            case L_CHAR:    v = (signed char)va_arg(ap, int); break;
            case L_SHORT:   v = (short)va_arg(ap, int); break;
            case L_LONG:    v = va_arg(ap, long); break;
            case L_LLONG:   v = va_arg(ap, long long); break;
            case L_SIZE:    v = (long long)va_arg(ap, size_t); break;
            case L_PTRDIFF: v = (long long)va_arg(ap, ptrdiff_t); break;
            case L_MAX:     v = va_arg(ap, long long); break;
            default:        v = va_arg(ap, int); break;
            }
            const char *sign = v < 0 ? "-" : (flags & F_PLUS) ? "+"
                             : (flags & F_SPACE) ? " " : "";
            unsigned long long u = v < 0 ? (unsigned long long)-(v + 1) + 1
                                         : (unsigned long long)v;
            put_int(&o, u, 10, 0, flags, width, prec, sign, "");
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'b': {
            unsigned long long v;
            switch (len) {
            case L_CHAR:    v = (unsigned char)va_arg(ap, unsigned); break;
            case L_SHORT:   v = (unsigned short)va_arg(ap, unsigned); break;
            case L_LONG:    v = va_arg(ap, unsigned long); break;
            case L_LLONG:   v = va_arg(ap, unsigned long long); break;
            case L_SIZE:    v = va_arg(ap, size_t); break;
            case L_PTRDIFF: v = (unsigned long long)va_arg(ap, ptrdiff_t); break;
            case L_MAX:     v = va_arg(ap, unsigned long long); break;
            default:        v = va_arg(ap, unsigned); break;
            }
            int base = conv == 'o' ? 8 : conv == 'u' ? 10
                     : conv == 'b' ? 2 : 16;
            const char *pfx = "";
            if ((flags & F_HASH) && v) {
                if (conv == 'x') pfx = "0x";
                else if (conv == 'X') pfx = "0X";
                else if (conv == 'o') pfx = "0";
            }
            put_int(&o, v, base, conv == 'X', flags, width, prec, "", pfx);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            if (len == L_LDOUBLE)
                put_double(&o, (double)va_arg(ap, long double), conv, flags,
                           width, prec);
            else
                put_double(&o, va_arg(ap, double), conv, flags, width, prec);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!(flags & F_MINUS)) pad(&o, ' ', width - 1);
            emit(&o, &c, 1);
            if (flags & F_MINUS) pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            /* A precision on %s is a MAXIMUM, and the string need not be
             * terminated within it -- so the length is strnlen, not strlen. */
            size_t n = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
            if (!(flags & F_MINUS)) pad(&o, ' ', width - (int)n);
            emit(&o, s, n);
            if (flags & F_MINUS) pad(&o, ' ', width - (int)n);
            break;
        }
        case 'p': {
            void *v = va_arg(ap, void *);
            if (!v) {
                const char *nil = "(nil)";
                if (!(flags & F_MINUS)) pad(&o, ' ', width - 5);
                emit(&o, nil, 5);
                if (flags & F_MINUS) pad(&o, ' ', width - 5);
            } else {
                put_int(&o, (unsigned long long)(size_t)v, 16, 0, flags,
                        width, prec, "", "0x");
            }
            break;
        }
        case 'n': {
            int *q = va_arg(ap, int *);
            if (q) *q = o.n;
            break;
        }
        case 0:
            return o.n;                /* a trailing '%': stop, do not run off */
        default:
            /* An unknown conversion prints itself, which is more use than
             * silence when a format string has a typo. */
            emit(&o, "%", 1);
            emit(&o, &conv, 1);
            break;
        }
    }
    return o.n;
}
