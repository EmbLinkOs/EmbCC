/* `long double`: the 128-bit conversions, and complex.
 *
 * `long double` means two different types on the two targets EmbCC has,
 * and this file is split down that line:
 *
 *   x86-64   the x87 80-bit format, a 64-bit mantissa the HARDWARE
 *            does. The conversions below are a few instructions of
 *            ordinary arithmetic, and they are guarded because there is
 *            no 80-bit type on the other machine to write them for.
 *   aarch64  IEEE binary128, a 113-bit mantissa with no instruction
 *            behind it. Its arithmetic and conversions are softtf.c,
 *            and the conversions here would be a second, worse copy.
 *
 * The COMPLEX half is built for both, because it needs no conversions:
 * it is arithmetic on `long double`, which one machine does in hardware
 * and the other does through softtf.c. Only the symbol name differs.
 *
 * What makes the x86-64 conversions short is that a 64-bit integer fits
 * EXACTLY in an x87 mantissa, so splitting a 128-bit value in half and
 * converting the halves separately loses nothing: each conversion is
 * exact, the scaling by 2^64 is an exponent adjustment, and the single
 * addition at the end is the only rounding -- which is what "correctly
 * rounded" means.
 */
#include "rt.h"

#ifdef __x86_64__

#define TWO64 18446744073709551616.0L      /* 2^64, exactly */

long double __floatuntixf(u128 a)
{
    return (long double)hi64(a) * TWO64 + (long double)lo64(a);
}

long double __floattixf(s128 a)
{
    union w128 w;
    w.s = a;
    if ((hi64(w.u) >> 63) == 0)
        return __floatuntixf(w.u);
    {
        u64 lo = lo64(w.u), hi = hi64(w.u);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        return -__floatuntixf(mk(nhi, nlo));
    }
}

u128 __fixunsxfti(long double a)
{
    long double hi_part;
    u64 hi, lo;

    if (a != a || a < 1.0L)            /* NaN, negative, or below one */
        return mk(0, 0);
    if (a >= TWO64 * TWO64)            /* 2^128: saturate, as the rest does */
        return mk(~0ULL, ~0ULL);
    /* a / 2^64 is an exponent adjustment and therefore exact, so its
     * integer part is the high half with nothing lost. Subtracting it
     * back out is exact too: both operands are representable and the
     * result is smaller than either. */
    hi_part = a / TWO64;
    hi = (u64)hi_part;                 /* truncates, which is what is wanted */
    lo = (u64)(a - (long double)hi * TWO64);
    return mk(hi, lo);
}

s128 __fixxfti(long double a)
{
    union w128 w;
    if (a != a)
        return (s128)0;
    if (a >= TWO64 * (TWO64 / 2.0L))   /* 2^127 */
        return (s128)mk(0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
    if (a <= -(TWO64 * (TWO64 / 2.0L)))
        return (s128)mk(0x8000000000000000ULL, 0);
    if (a < 0.0L) {
        u128 mag = __fixunsxfti(-a);
        u64 lo = lo64(mag), hi = hi64(mag);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        w.u = mk(nhi, nlo);
        return w.s;
    }
    w.u = __fixunsxfti(a);
    return w.s;
}

#endif   /* __x86_64__ : the conversions above are x87's */

/* ---- complex long double -------------------------------------------------
 *
 * The same two algorithms as complex.c, at the third width -- and this
 * half is built for BOTH targets, because it needs no conversions: it
 * is arithmetic on `long double`, which x86-64 does in hardware and
 * aarch64 does through softtf.c. Only the NAME differs, because libgcc
 * names these after the mode and the two machines' long double is not
 * the same mode: `xf` is x87's 80-bit, `tf` is IEEE binary128.
 *
 * See complex.c for why each step is there. */
#ifdef __x86_64__
#define CMUL __mulxc3
#define CDIV __divxc3
#else
#define CMUL __multc3
#define CDIV __divtc3
#endif

static int is_nan_l(long double x) { return x != x; }
static int is_inf_l(long double x)
{
    return x == x && (x - x != 0.0L);
}
static long double fabs_l(long double x) { return x < 0.0L ? -x : x; }
static long double copysign_l(long double m, long double s)
{
    long double a = m < 0.0L ? -m : m;
    /* A zero's sign cannot be read by comparing, so it is read by
     * dividing: 1/-0.0 is -inf and 1/+0.0 is +inf. */
    if (s < 0.0L || (s == 0.0L && 1.0L / s < 0.0L))
        return -a;
    return a;
}

long double _Complex CMUL(long double a, long double b,
                          long double c, long double d)
{
    long double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    long double re = ac - bd, im = ad + bc;
    long double _Complex z;

    if (is_nan_l(re) && is_nan_l(im)) {
        int recalc = 0;
        if (is_inf_l(a) || is_inf_l(b)) {
            a = copysign_l(is_inf_l(a) ? 1.0L : 0.0L, a);
            b = copysign_l(is_inf_l(b) ? 1.0L : 0.0L, b);
            if (is_nan_l(c)) c = copysign_l(0.0L, c);
            if (is_nan_l(d)) d = copysign_l(0.0L, d);
            recalc = 1;
        }
        if (is_inf_l(c) || is_inf_l(d)) {
            c = copysign_l(is_inf_l(c) ? 1.0L : 0.0L, c);
            d = copysign_l(is_inf_l(d) ? 1.0L : 0.0L, d);
            if (is_nan_l(a)) a = copysign_l(0.0L, a);
            if (is_nan_l(b)) b = copysign_l(0.0L, b);
            recalc = 1;
        }
        if (!recalc &&
            (is_inf_l(ac) || is_inf_l(bd) || is_inf_l(ad) || is_inf_l(bc))) {
            if (is_nan_l(a)) a = copysign_l(0.0L, a);
            if (is_nan_l(b)) b = copysign_l(0.0L, b);
            if (is_nan_l(c)) c = copysign_l(0.0L, c);
            if (is_nan_l(d)) d = copysign_l(0.0L, d);
            recalc = 1;
        }
        if (recalc) {
            long double inf = 1.0L / 0.0L;
            re = inf * (a * c - b * d);
            im = inf * (a * d + b * c);
        }
    }
    __real__ z = re;
    __imag__ z = im;
    return z;
}

long double _Complex CDIV(long double a, long double b,
                          long double c, long double d)
{
    long double re, im, denom, ratio;
    long double _Complex z;

    if (c == 0.0L && d == 0.0L && !(is_nan_l(a) && is_nan_l(b))) {
        long double inf = 1.0L / 0.0L;
        __real__ z = copysign_l(inf, c) * a;
        __imag__ z = copysign_l(inf, c) * b;
        return z;
    }
    if (fabs_l(c) >= fabs_l(d)) {
        ratio = d / c;
        denom = c + d * ratio;
        re = (a + b * ratio) / denom;
        im = (b - a * ratio) / denom;
    } else {
        ratio = c / d;
        denom = c * ratio + d;
        re = (a * ratio + b) / denom;
        im = (b * ratio - a) / denom;
    }

    if (is_nan_l(re) && is_nan_l(im)) {
        long double inf = 1.0L / 0.0L;
        if ((is_inf_l(a) || is_inf_l(b)) && !is_inf_l(c) && !is_inf_l(d)) {
            a = copysign_l(is_inf_l(a) ? 1.0L : 0.0L, a);
            b = copysign_l(is_inf_l(b) ? 1.0L : 0.0L, b);
            re = inf * (a * c + b * d);
            im = inf * (b * c - a * d);
        } else if ((is_inf_l(c) || is_inf_l(d)) &&
                   !is_nan_l(a) && !is_nan_l(b)) {
            c = copysign_l(is_inf_l(c) ? 1.0L : 0.0L, c);
            d = copysign_l(is_inf_l(d) ? 1.0L : 0.0L, d);
            re = 0.0L * (a * c + b * d);
            im = 0.0L * (b * c - a * d);
        }
    }
    __real__ z = re;
    __imag__ z = im;
    return z;
}
