/* sqrt: one instruction where the machine has one, integer arithmetic
 * where it does not.
 *
 * x86-64 has sqrtsd and aarch64 fsqrt, both correctly rounded, and
 * __builtin_sqrt emits them: faster and more accurate than any software
 * core, and fdlibm's asin/acos/hypot reach the square root through here
 * too.
 *
 * Every other target EmbCC has lacks one for a double -- RISC-V without
 * the D extension, a Cortex-M whose FPU is single precision or absent,
 * AVR -- and there the compiler turns __builtin_sqrt into a CALL to
 * sqrt, as gcc does, because that is what a program asking for the
 * builtin needs. Inside sqrt itself that is a call to itself: this
 * function used to be `return __builtin_sqrt(x);` everywhere, and on
 * all four embedded targets sqrt, hypot and cabs never returned. The
 * compiler now refuses that shape by name; this file is the other half.
 *
 * The list below is target_has_sqrt(8)'s (src/arch/target.c).
 */
#include <math.h>

#if defined(__x86_64__) || defined(__aarch64__)

double sqrt(double x)
{
    return __builtin_sqrt(x);
}

#else

/* The digit-by-digit square root, two radicand bits per step, of the
 * significand scaled so that its root is exactly as wide as the result:
 * correctly rounded by construction, because the remainder says on
 * which side of the half-way point the true root lies (it is never ON
 * it -- an exact half would need a square with more bits than the
 * radicand has). Integer only: there is no floating-point operation here
 * for a soft-float target to call out to, so nothing else is linked.
 *
 * `double` is binary64 everywhere but AVR, where it is binary32, and the
 * same code does both: MANT is the stored significand's width. */
#if __DBL_MANT_DIG__ == 53
typedef unsigned long long bits_t;
#define MANT 52
#define BIAS 1023
#define EMAX 0x7ff
#define SIGN 63
#else
typedef unsigned long bits_t;
#define MANT 23
#define BIAS 127
#define EMAX 0xff
#define SIGN 31
#endif

double sqrt(double x)
{
    union { double d; bits_t u; } v;
    bits_t m, rem = 0, root = 0;
    int e, ex;

    v.d = x;
    e = (int)(v.u >> MANT) & EMAX;
    m = v.u & (((bits_t)1 << MANT) - 1);
    /* The special cases by their bits too: `(x - x) / (x - x)` would be a
     * NaN, and on AVR it would also be the add and divide routines --
     * two thirds of the part's software float -- linked into anything
     * that takes a square root. None of these targets has an FP
     * environment for FE_INVALID to be raised in. */
    if (v.u >> SIGN) {
        if (e == 0 && m == 0)
            return x;                      /* sqrt(-0) is -0 */
        v.u = (bits_t)EMAX << MANT | (bits_t)1 << (MANT - 1);
        return v.d;                        /* the default quiet NaN */
    }
    if (e == EMAX) {
        if (m)
            v.u |= (bits_t)1 << (MANT - 1);    /* a NaN, quieted */
        return v.d;                        /* or +inf */
    }
    if (e == 0) {
        if (m == 0)
            return x;                      /* +0 */
        ex = 1 - BIAS;                     /* subnormal: normalize */
        while (!(m >> MANT & 1)) {
            m <<= 1;
            ex--;
        }
    } else {
        m |= (bits_t)1 << MANT;
        ex = e - BIAS;
    }
    /* x = m * 2^(ex - MANT). An even exponent halves exactly, so an odd
     * one moves a bit into the significand: m < 2^(MANT + 2). */
    if (ex & 1) {
        m <<= 1;
        ex--;
    }
    /* root = floor(sqrt(m * 2^MANT)), MANT + 1 bits, from the radicand's
     * pairs top down; m supplies the high ones and zeros the rest. The
     * remainder stays below 2 * root + 1, so nothing here overflows. */
    for (int i = 0; i <= MANT; i++) {
        bits_t trial;
        rem = rem << 2 | (m >> MANT & 3);
        m = m << 2 & (((bits_t)1 << (MANT + 2)) - 1);
        trial = root << 2 | 1;
        root <<= 1;
        if (rem >= trial) {
            rem -= trial;
            root |= 1;
        }
    }
    /* The next bit of the root is 1 exactly when rem > root (4 * rem >=
     * 4 * root + 1); with no tie possible, that is round to nearest. */
    if (rem > root)
        root++;
    /* sqrt(x) = root * 2^(ex/2 - MANT). root carries the implicit bit, so
     * ADDING it to the exponent field one below is the whole encoding --
     * and a root that rounded up to 2^(MANT+1) carries into the exponent
     * as it should. The field is ex/2 + BIAS - 1, halved as an unsigned
     * shift (ex + 2 * BIAS is even and positive): `ex / 2` was a call to
     * __divsi3 on AVR. */
    v.u = ((bits_t)(((unsigned)(ex + 2 * BIAS) >> 1) - 1) << MANT) + root;
    return v.d;
}

#endif
