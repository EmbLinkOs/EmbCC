/* Conversions between 128-bit integers and floating point.
 *
 * Neither machine converts a 128-bit integer to or from a float in one
 * instruction, so codegen emits a call. The hard half is not the
 * arithmetic, it is the ROUNDING: a 128-bit integer has up to 128
 * significant bits and a double has 53, so all but the widest values
 * lose some, and losing them the way IEEE-754 says (to nearest, ties to
 * even) is the whole job. Getting it almost right produces answers that
 * are off by one unit in the last place, which no casual test notices.
 *
 * The method is the standard one: keep the top 55 bits, fold everything
 * below them into a single STICKY bit, and let the hardware's own
 * 64-bit-integer-to-double conversion do the rounding. That works
 * because 55 bits is two more than a double's 53 -- one for the guard
 * and one for the sticky -- and because OR-ing the sticky into the
 * lowest kept bit cannot change a value that was already inexact there.
 *
 * See rt.h for why none of this uses `__int128` arithmetic.
 */
#include "rt.h"

/* 2^n as a double, built rather than computed: no libm here, and this
 * is exact for every n these conversions produce. */
static double pow2(int n)
{
    union { u64 u; double d; } v;
    v.u = (u64)(1023 + n) << 52;
    return v.d;
}

static float pow2f(int n)
{
    union { u32 u; float f; } v;
    v.u = (u32)(127 + n) << 23;
    return v.f;
}

static int clz64(u64 x)
{
    int n = 0;
    if (x == 0) return 64;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8;  x <<= 8;  }
    if (!(x >> 60)) { n += 4;  x <<= 4;  }
    if (!(x >> 62)) { n += 2;  x <<= 2;  }
    if (!(x >> 63)) { n += 1; }
    return n;
}

/* The top `keep` bits of a, right-justified, with everything below them
 * folded into bit 0. `keep` is at most 64 and the value has more than
 * `keep` significant bits, so the result is exactly `keep` bits wide
 * apart from that fold. */
static u64 top_bits_sticky(u128 a, int nbits, int keep, int *shift_out)
{
    u64 lo = lo64(a), hi = hi64(a);
    int s = nbits - keep;          /* how far down the kept bits sit */
    u64 v, lost;

    *shift_out = s;
    if (s >= 64) {
        v = hi >> (s - 64);
        lost = lo | (s > 64 ? (hi & ((1ULL << (s - 64)) - 1)) : 0);
    } else if (s > 0) {
        v = (hi << (64 - s)) | (lo >> s);
        lost = lo & ((1ULL << s) - 1);
    } else {
        return lo;                 /* nothing lost: s == 0 */
    }
    return v | (lost != 0);
}

/* ---- integer -> floating ------------------------------------------------ */

double __floatuntidf(u128 a)
{
    u64 hi = hi64(a);
    int nbits, s;
    u64 v;

    if (hi == 0)
        return (double)lo64(a);    /* the machine converts this one itself */
    nbits = 128 - clz64(hi);
    if (nbits <= 55)
        return (double)lo64(a);    /* cannot happen with hi != 0, but exact */
    v = top_bits_sticky(a, nbits, 55, &s);
    return (double)v * pow2(s);
}

double __floattidf(s128 a)
{
    union w128 w;
    w.s = a;
    if ((hi64(w.u) >> 63) == 0)
        return __floatuntidf(w.u);
    /* Negate into the unsigned domain, convert, negate back. The most
     * negative value negates to itself, and that is the right answer:
     * its magnitude IS 2^127, which the unsigned conversion handles. */
    {
        u64 lo = lo64(w.u), hi = hi64(w.u);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        return -__floatuntidf(mk(nhi, nlo));
    }
}

float __floatuntisf(u128 a)
{
    u64 hi = hi64(a);
    int nbits, s;
    u64 v;

    if (hi == 0)
        return (float)lo64(a);
    nbits = 128 - clz64(hi);
    v = top_bits_sticky(a, nbits, 26, &s);   /* 24 bits + guard + sticky */
    return (float)v * pow2f(s);
}

float __floattisf(s128 a)
{
    union w128 w;
    w.s = a;
    if ((hi64(w.u) >> 63) == 0)
        return __floatuntisf(w.u);
    {
        u64 lo = lo64(w.u), hi = hi64(w.u);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        return -__floatuntisf(mk(nhi, nlo));
    }
}

/* ---- floating -> integer ------------------------------------------------
 *
 * Truncation toward zero, as C requires of a cast. A value too large
 * for the result type is UNDEFINED in C, and this saturates at the
 * type's extremes -- not because any standard says so, but because
 * saturating is monotone, so a wrong answer still moves in the
 * direction of its input and a reader can follow it. See __fixdfti for
 * why that choice is not checked against another compiler. */
static u128 shift_left_128(u64 m, int s)
{
    if (s <= 0) {
        if (s <= -64)
            return mk(0, s <= -128 ? 0 : 0);
        return mk(0, m >> -s);
    }
    if (s >= 128)
        return mk(0, 0);
    if (s >= 64)
        return mk(m << (s - 64), 0);
    return mk(m >> (64 - s), m << s);
}

u128 __fixunsdfti(double a)
{
    union { double d; u64 u; } v;
    int e;
    u64 m;

    v.d = a;
    if (a != a || a < 1.0)         /* NaN, negative, or below one */
        return mk(0, 0);
    e = (int)((v.u >> 52) & 0x7FF) - 1023;
    if (e >= 128)
        return mk(~0ULL, ~0ULL);   /* saturate, as libgcc does */
    m = (v.u & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);   /* implicit bit */
    return shift_left_128(m, e - 52);
}

s128 __fixdfti(double a)
{
    union w128 w;
    double lim = pow2(127);        /* the first value that does not fit */

    if (a != a)
        return (s128)0;
    /* Saturate at the SIGNED extremes, not at the unsigned ones. Going
     * through __fixunsdfti and negating gave -1 for +1.7e308 and +1 for
     * -1.7e308: the unsigned conversion saturated to all-ones, which
     * read as a signed value is minus one. Saturating is at least
     * MONOTONE -- a bigger input does not produce a smaller output --
     * which is the property that makes a wrong answer debuggable.
     *
     * This is our choice and not a standard one. C leaves the
     * conversion undefined once the value does not fit, and checking it
     * against another compiler turned out to prove nothing: clang
     * answered 0x7fff... for one out-of-range value and a folded
     * bit-pattern for another, because at any optimisation level it
     * does not call its own runtime for a constant -- it folds it. So
     * tests/golden/rt.sh compares the IN-RANGE conversions against the
     * host and checks this behaviour separately, as ours.
     *
     * -2^127 is exactly representable and IS the minimum, so the lower
     * test includes it rather than excluding it. */
    if (a >= lim)
        return (s128)mk(0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
    if (a <= -lim)
        return (s128)mk(0x8000000000000000ULL, 0);
    if (a < 0.0) {
        u128 mag = __fixunsdfti(-a);
        u64 lo = lo64(mag), hi = hi64(mag);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        w.u = mk(nhi, nlo);
        return w.s;
    }
    w.u = __fixunsdfti(a);
    return w.s;
}

u128 __fixunssfti(float a) { return __fixunsdfti((double)a); }
s128 __fixsfti(float a)    { return __fixdfti((double)a); }
