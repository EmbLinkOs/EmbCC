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

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "../internal/utf8.h"

#include "format.h"

/* A wide printf's ASCII is the same characters, one wide one per byte. */
void __fmt_emit(struct out *o, const char *s, size_t n)
{
    if (o->wsink) {
        wchar_t w[32];
        for (size_t done = 0; done < n; ) {
            size_t k = n - done < 32 ? n - done : 32;
            for (size_t i = 0; i < k; i++)
                w[i] = (unsigned char)s[done + i];
            o->wsink(o->ctx, w, k);
            done += k;
        }
    } else {
        o->sink(o->ctx, s, n);
    }
    o->n += (int)n;
}

void __fmt_pad(struct out *o, char c, int n)
{
    char b[32];
    if (n <= 0) return;
    memset(b, c, sizeof b);
    while (n > 0) {
        int k = n < (int)sizeof b ? n : (int)sizeof b;
        __fmt_emit(o, b, (size_t)k);
        n -= k;
    }
}

static const char *DIGL = "0123456789abcdef";
static const char *DIGU = "0123456789ABCDEF";

/* One integer, already made unsigned and with its sign decided. */
void __fmt_int(struct out *o, unsigned long long v, int base, int upper,
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

    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', spad);
    if (*sign)   __fmt_emit(o, sign, strlen(sign));
    if (*prefix) __fmt_emit(o, prefix, strlen(prefix));
    __fmt_pad(o, '0', zpad);
    __fmt_pad(o, '0', zeros);
    while (nd) { char c = d[--nd]; __fmt_emit(o, &c, 1); }
    if (flags & F_MINUS) __fmt_pad(o, ' ', spad);
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

#if __SIZEOF_DOUBLE__ == 4
/* A double that is binary32 (RX's and AVR's: GCC's default there): eight
 * exponent bits and a 24-bit significand, the leading bit implied. */
static struct fpval fp_of_double(double v)
{
    union { double d; unsigned int u; } cv;
    cv.d = v;
    struct fpval f;
    f.hi = 0;
    f.neg = (int)(cv.u >> 31);
    f.lo = cv.u & 0x7fffffULL;
    f.mant_bits = 24;
    int be = (int)((cv.u >> 23) & 0xff);
    if (be == 0xff) {
        f.cls = f.lo ? FP_NAN : FP_INF;
        f.e2 = 0;
    } else if (be == 0) {
        f.e2 = -149;                 /* subnormal: no implicit leading 1 */
        f.cls = f.lo ? FP_NORMAL : FP_ZERO;
    } else {
        f.lo |= 1ULL << 23;
        f.e2 = be - 150;
        f.cls = FP_NORMAL;
    }
    return f;
}
#else
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
#endif

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
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    /* big-endian (SPARC, mips64-none-elf): the high word's bytes first */
    for (int i = 0; i < 8; i++)   f.hi = (f.hi << 8) | b[i];
    for (int i = 8; i < 16; i++)  f.lo = (f.lo << 8) | b[i];
#else
    for (int i = 7; i >= 0; i--)  f.lo = (f.lo << 8) | b[i];
    for (int i = 15; i >= 8; i--) f.hi = (f.hi << 8) | b[i];
#endif
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
        if (k == (int)sizeof buf) { __fmt_emit(o, buf, (size_t)k); k = 0; }
    }
    if (k)
        __fmt_emit(o, buf, (size_t)k);
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

    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', spad);
    if (sign[0]) __fmt_emit(o, sign, 1);
    __fmt_emit(o, up ? "0X" : "0x", 2);
    __fmt_pad(o, '0', zpad);               /* zeros go after the 0x, not before */
    char c = hx[d[0]];
    __fmt_emit(o, &c, 1);
    if (dot) __fmt_emit(o, ".", 1);
    for (int j = 1; j <= p; j++) {
        c = j <= nfrac ? hx[d[j]] : '0';
        __fmt_emit(o, &c, 1);
    }
    __fmt_emit(o, eb, (size_t)nex);
    if (flags & F_MINUS) __fmt_pad(o, ' ', spad);
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
        if (!(flags & F_MINUS)) __fmt_pad(o, ' ', width - body);
        if (sign[0]) __fmt_emit(o, sign, 1);
        __fmt_emit(o, w, 3);
        if (flags & F_MINUS) __fmt_pad(o, ' ', width - body);
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

    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', spad);
    if (sign[0]) __fmt_emit(o, sign, 1);
    __fmt_pad(o, '0', zpad);
    if (expo) {
        emit_dg(o, dg, ndg, 0, 1);
        if (dot) __fmt_emit(o, ".", 1);
        emit_dg(o, dg, ndg, 1, prec);
        __fmt_emit(o, eb, (size_t)nex);
    } else {
        if (ip_len <= 0)
            __fmt_emit(o, "0", 1);
        else
            emit_dg(o, dg, ndg, 0, ip_len);
        if (dot) __fmt_emit(o, ".", 1);
        emit_dg(o, dg, ndg, ip_len, prec);
    }
    if (flags & F_MINUS) __fmt_pad(o, ' ', spad);
}

void __fmt_double(struct out *o, double v, char conv, int flags,
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
void __fmt_ldouble(struct out *o, long double v, char conv, int flags,
                   int width, int prec)
{
    unsigned int limbs[LD_LIMBS];
    char dg[LD_DIGITS];
    put_fp(o, fp_of_ldouble(v), conv, flags, width, prec,
           limbs, LD_LIMBS, dg, LD_DIGITS);
}

/* ---- %c and %s in a byte printf ----
 *
 * %lc and %ls take wide characters and write their UTF-8 -- a precision on
 * %ls counting bytes and never cutting a character in two. One that has
 * no UTF-8, a lone surrogate, is an encoding error: nothing more is
 * written, and the call returns -1 with errno EILSEQ (C11 7.21.6.1p14).
 * The wide printfs' own %c and %s are wide.c's. */

/* The bytes of the wide string s that fit in `prec` (all of them when it
 * is negative), written when `write` is set; -1 on an encoding error. */
static long mb_of_wide(struct out *o, const wchar_t *s, int prec, int write)
{
    char b[4];
    long n = 0;
    for (; *s; s++) {
        int r = __utf8_put((uint_least32_t)*s, b);
        if (r < 0)
            return -1;
        if (prec >= 0 && n + r > prec)
            break;
        if (write)
            __fmt_emit(o, b, (size_t)r);
        n += r;
    }
    return n;
}

static void put_char(struct out *o, int is_l, va_list *ap, int flags,
                     int width)
{
    char b[4];
    int n = 1;
    if (is_l) {
        n = __utf8_put((uint_least32_t)(wchar_t)va_arg(*ap, wint_t), b);
        if (n < 0) { o->bad = 1; return; }
    } else {
        b[0] = (char)va_arg(*ap, int);
    }
    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', width - n);
    __fmt_emit(o, b, (size_t)n);
    if (flags & F_MINUS) __fmt_pad(o, ' ', width - n);
}

static void put_str(struct out *o, int is_l, va_list *ap, int flags,
                    int width, int prec)
{
    long n;
    const wchar_t *ws = NULL;
    const char *s = NULL;
    if (is_l) {
        ws = va_arg(*ap, const wchar_t *);
        if (!ws) ws = L"(null)";
        n = mb_of_wide(o, ws, prec, 0);
        if (n < 0) { o->bad = 1; return; }
    } else {
        s = va_arg(*ap, const char *);
        if (!s) s = "(null)";
        /* A precision on %s is a MAXIMUM, and the string need not be
         * terminated within it -- so the length is strnlen, not strlen. */
        n = (long)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
    }
    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', width - (int)n);
    if (s) __fmt_emit(o, s, (size_t)n); else mb_of_wide(o, ws, prec, 1);
    if (flags & F_MINUS) __fmt_pad(o, ' ', width - (int)n);
}

#define FMT_CH char
#define FMT_RUN format_bytes
#define FMT_TEXT(o, s, n) __fmt_emit((o), (s), (n))
#define FMT_CHAR put_char
#define FMT_STR put_str
#include "format-run.h"

int __vformat(void (*sink)(void *, const char *, size_t), void *ctx,
              const char *fmt, va_list ap)
{
    struct out o = { sink, NULL, ctx, 0, 0 };
    va_list cp;
    va_copy(cp, ap);
    int r = format_bytes(&o, fmt, &cp);
    va_end(cp);
    return r;
}
