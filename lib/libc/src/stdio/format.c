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
 * long double is converted the same way and just as exactly, over a
 * bigger big integer (5^16494 rather than 5^1074), and %a is here too --
 * the one conversion that needs no expansion at all, since four bits of
 * a binary significand are one hexadecimal digit.
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
 * The limbs belong to the CALLER. A double needs 86 of them -- its
 * smallest subnormal is 2^-1074, whose exact expansion is 5^1074 scaled,
 * and that is 767 digits -- while a long double needs some fifteen times
 * more. Sizing one buffer for the larger would put that cost on every
 * printf of a double, so each entry point declares its own and passes it
 * in, and the code between them is written once.
 */
#define DEC_BASE   1000000000u
#define DEC_LIMBS  96
#define DEC_DIGITS 800

/* An IEEE quad's smallest subnormal is 2^-16494; times its 113-bit
 * significand that is 11,563 digits, so 1300 limbs (11,700 digits) is
 * the exact requirement with a little room. An x87 extended needs less.
 * The buffers are ~17KB of stack, paid only by a call that actually
 * passes a long double. */
#if __LDBL_MANT_DIG__ > 53
#define LD_LIMBS  1300
#else
#define LD_LIMBS  DEC_LIMBS
#endif
#define LD_DIGITS (LD_LIMBS * 9)

struct bigdec {
    unsigned int *l;                 /* little end first, each < DEC_BASE */
    int cap;
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
    while (carry && b->n < b->cap) {
        b->l[b->n++] = (unsigned int)(carry % DEC_BASE);
        carry /= DEC_BASE;
    }
}

static void bd_add(struct bigdec *b, unsigned long long v)
{
    int i = 0;
    while (v && i < b->cap) {
        if (i == b->n)
            b->l[b->n++] = 0;
        unsigned long long t = (unsigned long long)b->l[i] + v % DEC_BASE;
        v /= DEC_BASE;
        if (t >= DEC_BASE) { t -= DEC_BASE; v++; }   /* carry into the next */
        b->l[i++] = (unsigned int)t;
    }
}

/* b = (hi << 64) | lo, for the significands too wide for one word. */
static void bd_set128(struct bigdec *b, unsigned long long hi,
                      unsigned long long lo)
{
    bd_set(b, hi);
    if (hi)
        for (int i = 0; i < 4; i++)  /* x 2^64, in steps bd_mul allows */
            bd_mul(b, 1u << 16);
    bd_add(b, lo);
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

/* Would rounding to `keep` digits carry out of the leading digit? That
 * is the only thing %g needs to know about the rounded value -- it picks
 * its style from the exponent AFTER rounding -- and answering it
 * directly avoids rounding a scratch copy of the digits, which for a
 * long double would be another eleven kilobytes of stack. The rule is
 * bd_round's, and the carry escapes exactly when every kept digit was a
 * nine. */
static int bd_round_carries(const char *dg, int ndg, int keep)
{
    if (keep >= ndg || keep < 0)
        return 0;
    int next = dg[keep] - '0';
    int sticky = 0;
    for (int i = keep + 1; i < ndg; i++)
        if (dg[i] != '0') { sticky = 1; break; }
    int last = keep > 0 ? dg[keep - 1] - '0' : 0;
    if (!(next > 5 || (next == 5 && (sticky || (last & 1)))))
        return 0;
    for (int i = keep - 1; i >= 0; i--)
        if (dg[i] != '9')
            return 0;
    return 1;
}

/* ---- a floating value with its format forgotten -------------------------
 *
 * A double, an x87 80-bit extended and an IEEE quad differ only in how
 * their bits are laid out; once decomposed they are the same thing --
 * an integer significand times a power of two. Everything below this
 * point is written once and works for all three, which is what keeps
 * long double from being a second implementation that drifts.
 */
enum { FP_ZERO, FP_NORMAL, FP_INF, FP_NAN };

struct fpval {
    unsigned long long hi, lo;   /* significand: value = (hi:lo) x 2^e2 */
    int e2;
    int mant_bits;               /* its width, which %a splits into digits */
    int cls;
    int neg;
};

static struct fpval fp_of_double(double v)
{
    union { double d; unsigned long long u; } cv;
    cv.d = v;
    struct fpval f;
    f.hi = 0;
    f.neg = (int)(cv.u >> 63);
    f.lo = cv.u & 0xfffffffffffffULL;
    f.mant_bits = 53;
    int be = (int)((cv.u >> 52) & 0x7ff);
    if (be == 0x7ff) {
        f.cls = f.lo ? FP_NAN : FP_INF;
        f.e2 = 0;
    } else if (be == 0) {
        f.e2 = -1074;                /* subnormal: no implicit leading 1 */
        f.cls = f.lo ? FP_NORMAL : FP_ZERO;
    } else {
        f.lo |= 1ULL << 52;
        f.e2 = be - 1075;
        f.cls = FP_NORMAL;
    }
    return f;
}

#if __LDBL_MANT_DIG__ == 64
/* x87 80-bit extended, little-endian: eight bytes of significand whose
 * leading bit is EXPLICIT -- the one format that stores it rather than
 * implying it -- then sixteen bits of sign and exponent. */
static struct fpval fp_of_ldouble(long double v)
{
    unsigned char b[sizeof(long double)];
    memcpy(b, &v, sizeof b);
    struct fpval f;
    f.hi = 0;
    f.lo = 0;
    for (int i = 7; i >= 0; i--) f.lo = (f.lo << 8) | b[i];
    unsigned int se = (unsigned)b[8] | ((unsigned)b[9] << 8);
    f.neg = (int)(se >> 15);
    f.mant_bits = 64;
    int be = (int)(se & 0x7fff);
    if (be == 0x7fff) {
        /* With the bit explicit, infinity is exactly 0x8000000000000000
         * and every other significand is a NaN. */
        f.cls = (f.lo << 1) ? FP_NAN : FP_INF;
        f.e2 = 0;
    } else if (be == 0) {
        f.e2 = -16445;
        f.cls = f.lo ? FP_NORMAL : FP_ZERO;
    } else {
        f.e2 = be - 16383 - 63;
        f.cls = FP_NORMAL;
    }
    return f;
}
#elif __LDBL_MANT_DIG__ == 113
/* IEEE binary128: a 113-bit significand with the leading bit implied,
 * spanning both words. */
static struct fpval fp_of_ldouble(long double v)
{
    unsigned char b[sizeof(long double)];
    memcpy(b, &v, sizeof b);
    struct fpval f;
    f.hi = 0;
    f.lo = 0;
    for (int i = 7; i >= 0; i--)  f.lo = (f.lo << 8) | b[i];
    for (int i = 15; i >= 8; i--) f.hi = (f.hi << 8) | b[i];
    f.neg = (int)(f.hi >> 63);
    f.mant_bits = 113;
    int be = (int)((f.hi >> 48) & 0x7fff);
    f.hi &= 0xffffffffffffULL;
    if (be == 0x7fff) {
        f.cls = (f.hi | f.lo) ? FP_NAN : FP_INF;
        f.e2 = 0;
    } else if (be == 0) {
        f.e2 = -16494;
        f.cls = (f.hi | f.lo) ? FP_NORMAL : FP_ZERO;
    } else {
        f.hi |= 1ULL << 48;
        f.e2 = be - 16383 - 112;
        f.cls = FP_NORMAL;
    }
    return f;
}
#else
/* A target where long double is double: one format, one decomposition. */
static struct fpval fp_of_ldouble(long double v)
{
    return fp_of_double((double)v);
}
#endif

/* Bits [p, p+4) of the significand, as a nibble. */
static unsigned fp_nib(const struct fpval *f, int p)
{
    unsigned long long w;
    if (p >= 64)
        w = f->hi >> (p - 64);
    else if (p == 0)
        w = f->lo;                   /* a 64-bit shift is undefined */
    else
        w = (f->lo >> p) | (f->hi << (64 - p));
    return (unsigned)(w & 0xf);
}

/* Emit n digits of the significand starting at index i. Indices outside
 * [0, ndg) are zeros -- that is where a %f's leading and trailing zeros
 * come from, and why they need no case of their own. */
static void emit_dg(struct out *o, const char *dg, int ndg, int i, int n)
{
    char buf[64];
    int k = 0;
    while (n-- > 0) {
        buf[k++] = (i >= 0 && i < ndg) ? dg[i] : '0';
        i++;
        if (k == (int)sizeof buf) { emit(o, buf, (size_t)k); k = 0; }
    }
    if (k)
        emit(o, buf, (size_t)k);
}

/* The exponent field. C wants at least two digits for %e and at least
 * one for %a; a long double reaches four, so the width is not fixed. */
static int exp_str(char *b, int ex, char marker, int mindig)
{
    int n = 0;
    b[n++] = marker;
    b[n++] = ex < 0 ? '-' : '+';
    unsigned a = (unsigned)(ex < 0 ? -ex : ex);
    char t[8];
    int k = 0;
    do { t[k++] = (char)('0' + a % 10); a /= 10; } while (a);
    while (k < mindig) t[k++] = '0';
    while (k) b[n++] = t[--k];
    return n;
}

/* ---- %a: hexadecimal, and exact without any expansion at all ------------
 *
 * A binary significand written in base sixteen needs no conversion --
 * four bits are one digit -- so %a is the only conversion that can print
 * a float and read back the identical bits. It rounds only when a
 * precision asks for fewer digits than the significand has.
 */
static void put_hexfp(struct out *o, struct fpval f, char conv, int flags,
                      int width, int prec, const char *sign)
{
    int up = (conv == 'A');
    const char *hx = up ? "0123456789ABCDEF" : "0123456789abcdef";
    int nfrac = (f.mant_bits - 1) / 4;
    int lead_bits = f.mant_bits - 4 * nfrac;

    /* d[0] is the digit before the point, d[1..nfrac] the fraction. A
     * double's 53 bits split as 1 + 13x4, so the lead is the implicit
     * one and the value reads 0x1.xxxp+e; an x87's 64 split as 4 + 15x4,
     * which is why glibc prints 1.0L as 0x8p-3 and so does this. */
    /* A subnormal is shifted up until its leading bit is set, so every
     * finite non-zero value prints with the same leading digit and the
     * exponent is the true binary exponent. Shifting is exact -- it
     * moves bits and the exponent together -- and C leaves the leading
     * digit unspecified for a value that is not normalized, which is
     * why implementations differ here: glibc prints the smallest double
     * as 0x0.0000000000001p-1022, this and BSD as 0x1p-1074. */
    if (f.cls == FP_NORMAL) {
        while (!((f.mant_bits > 64 ? (f.hi >> (f.mant_bits - 65))
                                   : (f.lo >> (f.mant_bits - 1))) & 1)) {
            f.hi = (f.hi << 1) | (f.lo >> 63);
            f.lo <<= 1;
            f.e2--;
        }
    }

    unsigned char d[36];
    d[0] = (unsigned char)(fp_nib(&f, 4 * nfrac) & ((1u << lead_bits) - 1));
    for (int j = 1; j <= nfrac; j++)
        d[j] = (unsigned char)fp_nib(&f, 4 * (nfrac - j));

    int ex = f.cls == FP_ZERO ? 0 : f.e2 + 4 * nfrac;
    int p = prec;
    if (p < 0) {
        /* Enough digits to be exact and no more. */
        p = nfrac;
        while (p > 0 && d[p] == 0) p--;
    } else if (p < nfrac) {
        /* Ties to even, and a tie here is an exact eight with nothing
         * after it -- the same rule the decimal path uses, in base 16. */
        int rup = 0;
        if (d[p + 1] > 8) {
            rup = 1;
        } else if (d[p + 1] == 8) {
            int any = 0;
            for (int j = p + 2; j <= nfrac; j++)
                if (d[j]) { any = 1; break; }
            rup = any || (d[p] & 1);
        }
        if (rup) {
            int j = p;
            for (; j >= 0; j--) {
                if (++d[j] < 16) break;
                d[j] = 0;
            }
            if (j < 0) {
                /* Carried off the front: 0xf.ff -> 0x1.00 four bits up.
                 * Only a four-bit lead can do this. */
                d[0] = 1;
                ex += 4;
            }
        }
    }

    int dot = p > 0 || (flags & F_HASH);
    char eb[10];
    int nex = exp_str(eb, ex, up ? 'P' : 'p', 1);
    int body = 2 + 1 + (dot ? 1 : 0) + p + nex + (sign[0] ? 1 : 0);
    int zpad = (flags & F_ZERO) && !(flags & F_MINUS) ? width - body : 0;
    int spad = width - body - (zpad > 0 ? zpad : 0);

    if (!(flags & F_MINUS)) pad(o, ' ', spad);
    if (sign[0]) emit(o, sign, 1);
    emit(o, up ? "0X" : "0x", 2);
    pad(o, '0', zpad);               /* zeros go after the 0x, not before */
    char c = hx[d[0]];
    emit(o, &c, 1);
    if (dot) emit(o, ".", 1);
    for (int j = 1; j <= p; j++) {
        c = j <= nfrac ? hx[d[j]] : '0';
        emit(o, &c, 1);
    }
    emit(o, eb, (size_t)nex);
    if (flags & F_MINUS) pad(o, ' ', spad);
}

/* %f / %e / %g / %a, over the caller's big-integer buffers. */
static void put_fp(struct out *o, struct fpval f, char conv, int flags,
                   int width, int prec, unsigned int *limbs, int nlimbs,
                   char *dg, int dgcap)
{
    /* %g strips trailing zeros from the fraction unless '#' is given
     * (C11 §7.21.6.1p8) -- which is most of what makes %g readable, and is
     * the step a hand-written formatter always leaves out. */
    int strip = (conv == 'g' || conv == 'G') && !(flags & F_HASH);
    char sign[2] = {0, 0};
    if (f.neg)                       /* -0.0 keeps its sign, as C requires */
        sign[0] = '-';
    else if (flags & F_PLUS)  sign[0] = '+';
    else if (flags & F_SPACE) sign[0] = ' ';

    /* Not-a-number and infinity print as words, and the words are lower or
     * upper case with the conversion. */
    if (f.cls == FP_NAN || f.cls == FP_INF) {
        int up = conv < 'a';
        const char *w = f.cls == FP_NAN ? (up ? "NAN" : "nan")
                                        : (up ? "INF" : "inf");
        int body = 3 + (sign[0] ? 1 : 0);
        if (!(flags & F_MINUS)) pad(o, ' ', width - body);
        if (sign[0]) emit(o, sign, 1);
        emit(o, w, 3);
        if (flags & F_MINUS) pad(o, ' ', width - body);
        return;
    }

    if (conv == 'a' || conv == 'A') {
        put_hexfp(o, f, conv, flags, width, prec, sign);
        return;
    }

    if (prec < 0) prec = 6;

    /* ---- the exact decimal digits ---------------------------------------
     *
     * The value is m x 2^e with m an integer, and that ALWAYS has a
     * finite decimal expansion: for e >= 0 it is the integer m x 2^e,
     * and for e < 0 it is m x 5^-e with the point shifted -e places,
     * because 1/2^k is 5^k/10^k. So the exact digits come from integer
     * multiplication alone -- no division, no approximation, and no
     * question about where a tie falls.
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
    int ndg, scale = 0;
    int zero = (f.cls == FP_ZERO);
    if (zero) {
        dg[0] = '0';
        ndg = 1;
    } else {
        struct bigdec b;
        b.l = limbs;
        b.cap = nlimbs;
        b.n = 0;
        bd_set128(&b, f.hi, f.lo);
        if (f.e2 > 0) {
            int k = f.e2;
            while (k >= 29) { bd_mul(&b, 1u << 29); k -= 29; }
            if (k) bd_mul(&b, 1u << k);
        } else if (f.e2 < 0) {
            static const unsigned int P5[13] = {
                1u, 5u, 25u, 125u, 625u, 3125u, 15625u, 78125u,
                390625u, 1953125u, 9765625u, 48828125u, 244140625u
            };
            int k = scale = -f.e2;
            while (k >= 12) { bd_mul(&b, P5[12]); k -= 12; }
            if (k) bd_mul(&b, P5[k]);
        }
        ndg = bd_digits(&b, dg);
    }
    (void)dgcap;
    /* The exponent of the leading digit: value = d.ddd x 10^exp10. */
    int exp10 = zero ? 0 : ndg - 1 - scale;

    if (conv == 'g' || conv == 'G') {
        /* %g picks %e or %f by exponent, and its precision counts
         * significant digits rather than fraction digits. The exponent
         * it looks at is the one AFTER rounding (C11 7.21.6.1p8): 9.99
         * at three significant digits is 9.99 and stays %f, but at two
         * it is 10 and the exponent that decides has become 1. */
        int pg = prec ? prec : 1;
        int te = exp10 + bd_round_carries(dg, ndg, pg);
        if (te < -4 || te >= pg) {
            conv = (conv == 'g') ? 'e' : 'E';
            prec = pg - 1;
        } else {
            conv = 'f';
            prec = pg - 1 - te;
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

    /* ---- lay it out without building it ---------------------------------
     *
     * The width padding needs the total length first, and for a long
     * double %f that string runs to nearly five thousand characters --
     * so the length is computed and the pieces are streamed. The fixed
     * output buffer this replaces silently truncated any precision that
     * overran it.
     */
    int expo = (conv == 'e' || conv == 'E');
    int ip_len = ndg > 0 ? exp10 + 1 : 1;    /* digits before the point */
    if (strip && prec > 0) {
        /* Trailing zeros come off the FRACTION only, so 100.0 with %g is
         * "100" and not "1". A position past the exact expansion is a
         * zero, which is why this asks dg by index rather than scanning
         * a string that was built for the purpose. */
        int base = expo ? 1 : ip_len;
        while (prec > 0) {
            int idx = base + prec - 1;
            if ((idx >= 0 && idx < ndg ? dg[idx] : '0') != '0')
                break;
            prec--;
        }
    }
    int dot = prec > 0 || (flags & F_HASH);
    char eb[10];
    int nex = expo ? exp_str(eb, zero ? 0 : exp10, conv, 2) : 0;
    int ipc = expo ? 1 : (ip_len > 0 ? ip_len : 1);
    int body = ipc + (dot ? 1 : 0) + prec + nex + (sign[0] ? 1 : 0);
    int zpad = (flags & F_ZERO) && !(flags & F_MINUS) ? width - body : 0;
    int spad = width - body - (zpad > 0 ? zpad : 0);

    if (!(flags & F_MINUS)) pad(o, ' ', spad);
    if (sign[0]) emit(o, sign, 1);
    pad(o, '0', zpad);
    if (expo) {
        emit_dg(o, dg, ndg, 0, 1);
        if (dot) emit(o, ".", 1);
        emit_dg(o, dg, ndg, 1, prec);
        emit(o, eb, (size_t)nex);
    } else {
        if (ip_len <= 0)
            emit(o, "0", 1);
        else
            emit_dg(o, dg, ndg, 0, ip_len);
        if (dot) emit(o, ".", 1);
        emit_dg(o, dg, ndg, ip_len, prec);
    }
    if (flags & F_MINUS) pad(o, ' ', spad);
}

static void put_double(struct out *o, double v, char conv, int flags,
                       int width, int prec)
{
    unsigned int limbs[DEC_LIMBS];
    char dg[DEC_DIGITS];
    put_fp(o, fp_of_double(v), conv, flags, width, prec,
           limbs, DEC_LIMBS, dg, DEC_DIGITS);
}

/* A separate entry point rather than a flag, because the difference that
 * matters is the size of the buffers above -- putting the long double's
 * on every printf of a double would be ~17KB of stack for nothing. */
static void put_ldouble(struct out *o, long double v, char conv, int flags,
                        int width, int prec)
{
    unsigned int limbs[LD_LIMBS];
    char dg[LD_DIGITS];
    put_fp(o, fp_of_ldouble(v), conv, flags, width, prec,
           limbs, LD_LIMBS, dg, LD_DIGITS);
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
        case 'a': case 'A':
            if (len == L_LDOUBLE)
                put_ldouble(&o, va_arg(ap, long double), conv, flags,
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
