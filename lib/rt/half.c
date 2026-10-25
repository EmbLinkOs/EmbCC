/* _Float16: the conversions between IEEE binary16 and the wider formats.
 *
 * EmbCC computes with a _Float16 in float -- every result rounded back to
 * half, which float's 24 bits do exactly (Figueroa: 24 >= 2*11 + 2) -- and
 * stores it as binary16. What it calls is these: the half's bits to float,
 * a float (or double, or long double) to the nearest half's bits, ties to
 * even. All of it on the bits, with integer arithmetic only, so a target
 * with no floating-point hardware pays for no soft-float call here, and
 * the answers are the hardware's: checked against the M-series FCVT for
 * all 65536 halves, all 2^32 floats and 400 million doubles.
 *
 * The names. Where the machine passes a _Float16 in the low bits of a
 * floating-point register -- x86-64 and AArch64, in libgcc's and
 * compiler-rt's convention -- they are libgcc's, __extendhfsf2 and its
 * kin, with the half travelling as those bits of a float, so an object of
 * ours links beside their runtime and calls theirs where there is no
 * librt of ours (macOS, Windows). On ARM they are libgcc's __gnu_*_ieee,
 * which pass the half as an unsigned short. Elsewhere no libgcc agrees on
 * a convention for a type it mostly does not have, so they are EmbCC's
 * own, __embcc_*, the half an unsigned short: a program linked against
 * somebody else's runtime gets a link error naming the routine, not a
 * half passed in the wrong register. src/ir/irgen.c (half_call) is the
 * caller, and keeps the same table. */
#include "rt.h"

static u32 h_widen(u32 h)
{
    u32 sign = (h & 0x8000UL) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e == 0x1f)                      /* infinity, or a NaN made quiet */
        return sign | 0x7f800000UL | (m ? (m | 0x200) << 13 : 0);
    if (e)
        return sign | ((e + 112) << 23) | (m << 13);
    if (!m)
        return sign;
    e = 113;                            /* a subnormal: normalized */
    while (!(m & 0x400)) {
        m <<= 1;
        e--;
    }
    return sign | (e << 23) | ((m & 0x3ff) << 13);
}

/* m >> sh, rounded to nearest by the bits shifted out, ties to even */
static u32 h_round(u64 m, int sh)
{
    u64 q = m >> sh, rem = m & (((u64)1 << sh) - 1), half = (u64)1 << (sh - 1);
    if (rem > half || (rem == half && (q & 1)))
        q++;
    return (u32)q;
}

static u32 h_narrow(u32 u)
{
    u32 sign = (u >> 16) & 0x8000UL, a = u & 0x7fffffffUL;
    if (a >= 0x7f800000UL)              /* a NaN keeps its top bits, quiet */
        return sign | 0x7c00 | (a > 0x7f800000UL ? 0x200 | ((a >> 13) & 0x3ff) : 0);
    if (a >= 0x477ff000UL)              /* 65520 and up: infinity */
        return sign | 0x7c00;
    if (a >= 0x38800000UL) {            /* 2^-14 and up: a normal half */
        u32 r = a - 0x38000000UL;       /* the exponent rebased, 127 to 15 */
        return sign | ((r + 0xfff + ((r >> 13) & 1)) >> 13);
    }
    if (a < 0x33000000UL)               /* below 2^-25: zero */
        return sign;
    return sign | h_round((a & 0x7fffffUL) | 0x800000UL, 126 - (int)(a >> 23));
}

#if __SIZEOF_DOUBLE__ == 8
static u64 d_bits(double d)
{
    union { double d; u64 u; } x;
    x.d = d;
    return x.u;
}

static u32 h_narrow64(u64 u)
{
    u32 sign = (u32)(u >> 48) & 0x8000UL;
    u64 a = u & 0x7fffffffffffffffULL;
    int e = (int)(a >> 52);
    if (e == 0x7ff)
        return sign | 0x7c00 |
               (a & 0xfffffffffffffULL ? 0x200 | ((u32)(a >> 42) & 0x3ff) : 0);
    if (a >= 0x40effe0000000000ULL)     /* 65520 and up */
        return sign | 0x7c00;
    if (e >= 1009)                      /* 2^-14 and up: the exponent rides
                                         * above the fraction, and a carry
                                         * out of it is the next binade */
        return sign | h_round((a & 0xfffffffffffffULL) | ((u64)(e - 1008) << 52), 42);
    if (a < 0x3e60000000000000ULL)      /* below 2^-25 */
        return sign;
    return sign | h_round((a & 0xfffffffffffffULL) | 0x10000000000000ULL, 1051 - e);
}
#endif

#if __SIZEOF_LONG_DOUBLE__ > __SIZEOF_DOUBLE__ && __SIZEOF_DOUBLE__ == 8
/* A long double rounds to double first -- ROUNDED TO ODD: when that is
 * inexact the last bit is made 1, standing for everything below it, and
 * 53 bits are then enough for the second rounding to give the first's
 * answer (Boldo and Melquiond). Rounding to nearest twice would not: a
 * value just above a half-way point can land on it. */
static u32 h_narrowl(long double x)
{
    double d = (double)x;
    long double back = (long double)d;
    u64 b = d_bits(d);
    if (x == x && back != x && !(b & 1))
        b += ((x > back) != (x < 0)) ? 1 : (u64)-1;   /* toward x */
    return h_narrow64(b);
}
#endif

static float f_of(u32 u)
{
    union { float f; u32 u; } x;
    x.u = u;
    return x.f;
}

static u32 f_bits(float f)
{
    union { float f; u32 u; } x;
    x.f = f;
    return x.u;
}

#if defined(__x86_64__) || defined(__aarch64__)
float __extendhfsf2(float h) { return f_of(h_widen(f_bits(h) & 0xffff)); }
float __truncsfhf2(float f) { return f_of(h_narrow(f_bits(f))); }
float __truncdfhf2(double d) { return f_of(h_narrow64(d_bits(d))); }
#if defined(__x86_64__)
float __truncxfhf2(long double x) { return f_of(h_narrowl(x)); }
#elif __SIZEOF_LONG_DOUBLE__ == 16
float __trunctfhf2(long double x) { return f_of(h_narrowl(x)); }
#endif
#elif defined(__arm__)
float __gnu_h2f_ieee(unsigned short h) { return f_of(h_widen(h)); }
unsigned short __gnu_f2h_ieee(float f) { return (unsigned short)h_narrow(f_bits(f)); }
unsigned short __gnu_d2h_ieee(double d) { return (unsigned short)h_narrow64(d_bits(d)); }
#else
float __embcc_h2f(unsigned short h) { return f_of(h_widen(h)); }
unsigned short __embcc_f2h(float f) { return (unsigned short)h_narrow(f_bits(f)); }
#if __SIZEOF_DOUBLE__ == 8
unsigned short __embcc_d2h(double d) { return (unsigned short)h_narrow64(d_bits(d)); }
#endif
#if __SIZEOF_LONG_DOUBLE__ > __SIZEOF_DOUBLE__ && __SIZEOF_DOUBLE__ == 8
unsigned short __embcc_ld2h(long double x) { return (unsigned short)h_narrowl(x); }
#endif
#endif
