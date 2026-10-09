/* IEEE-754 binary32, in software, for AVR.
 *
 * On this target `float` AND `double` are both four-byte binary32 -- that is
 * avr-gcc's documented default and what src/arch/target.c's data model says
 * -- so every floating-point operation is a call, and this file is what it
 * calls. There is no __adddf3 family here because there is no binary64 to
 * add: a `double` IS a `float`, and the backend emits the `sf` names for
 * both.
 *
 * ---- why this is not lib/rt/softfp.c --------------------------------
 *
 * That file implements binary32 by widening both operands to binary64, doing
 * the work there, and rounding back -- which gives the same answer, because
 * binary64 carries 53 significand bits and 53 >= 2p+2 for p = 24
 * (Figueroa's theorem). It cannot be used here: this machine has no binary64
 * at all, so there is nothing to widen INTO. binary32 has to be implemented
 * natively, which is what this file is.
 *
 * ---- the rule this file lives under (rt.h) --------------------------
 *
 * It may not use the operations it implements. Every routine takes and
 * returns `float` -- it has to, or the ABI would be wrong -- but the value is
 * converted to its BITS through a union on the way in and back on the way
 * out, and nothing in between is a floating-point operation. A single
 * `a * b` here would be a call to __mulsf3, which is this file.
 *
 * ---- everything is 32-bit, and that is a SIZE decision --------------
 *
 * The first version carried its significands in `unsigned long long`, which
 * reads more naturally: a 24x24 product is 48 bits and wants a type that
 * holds it. It came to 51 KB of flash on a part that has 32 -- round_pack
 * alone was 10.7 KB and addsub 14.7 KB -- because every 64-bit operation in
 * this backend is a byte-at-a-time chain through frame slots.
 *
 * So nothing here is wider than 32 bits. A significand with three guard bits
 * is 27, a sum is 28, a quotient is 27: all of them fit. The ONE thing that
 * does not is the multiply's product, and that is carried as two 32-bit words
 * with the split done by hand. The two 64-bit truncations at the end keep
 * `long long` because their RESULT is that wide and there is no way around
 * it; they are also the smallest routines in the file.
 *
 * ---- one group per object, because the part has 32 KB -------------
 *
 * binary32 in software is about 33 KB of AVR text all told, which does not
 * fit an ATmega328P even before the program. It does not have to fit: nothing
 * links a routine it never calls, PROVIDED the routines are in separate
 * objects. lib/rt/avr.c and lib/rt/avr64.c are already split for exactly this
 * reason -- with the 64-bit integer helpers in the same object as the 32-bit
 * ones, a program that never wrote `long long` still carried them, and the
 * execution test came to 34718 bytes on a part with 32768.
 *
 * So each group is its own object -- avrfpadd.c, avrfpmul.c, avrfpdiv.c,
 * avrfpcmp.c, avrfpi.c, avrfpi64.c -- and THIS file holds only what they all
 * share: unpack and round_pack. The accessors are small enough to be static
 * in avrfp.h.
 *
 * addsub is in its own object rather than here for the same reason as the
 * rest, and it is the case that pays best: it is the largest single routine
 * in the implementation, about 9 KB at -O0, and a program that multiplies or
 * divides floats without adding them does not need a byte of it. Keeping it
 * here put it in every float image.
 *
 * ---- what is implemented --------------------------------------------
 *
 * Round to nearest, ties to even -- the only mode IEEE-754 requires a
 * compiler to have and the only one C's default rounding direction names.
 * Subnormals, both as inputs and as results. Signed zeroes, infinities and
 * NaNs, with the quiet bit set on a produced NaN. No exception flags: this
 * machine has no FPSCR to raise them in, and the ABI tag says so.
 */
/* RX too: GCC rx-elf's double is binary32 (-m32bit-doubles), so it needs
 * exactly this native binary32 and no binary64. */
#if defined(__AVR__) || defined(__RX__)

#include "avrfp.h"

/* ---- unpacking -------------------------------------------------------
 *
 * Returns the significand with the implicit bit made explicit, and the
 * exponent as an unbiased power of two applied to that integer. A subnormal
 * has no implicit bit and its exponent is the smallest normal's, which is
 * what makes the arithmetic below need no separate subnormal path.
 */
void __avrfp_unpack(u32 u, u32 *m, int *e)
{
    int ex = fexp(u);
    if (ex == 0) {                      /* zero or subnormal */
        *m = fmant(u);
        *e = 1 - BIAS - MANTBITS;
    } else {
        *m = fmant(u) | 0x800000ul;
        *e = ex - BIAS - MANTBITS;
    }
}

/* ---- rounding -------------------------------------------------------
 *
 * ONE function, and every operation funnels through it. Two implementations
 * of round-to-nearest-even is how a soft-float library comes out off by one
 * unit in the last place in one operation and not another.
 *
 * It takes a significand with `extra` bits BELOW the 24 that will be kept,
 * `e` as the power of two applied to the significand's lowest bit, and
 * `sticky` saying whether anything below `m` was non-zero. The order matters
 * and is the standard one:
 *
 *   normalise to exactly 24 + extra bits, folding what falls off into sticky;
 *   compute the exponent field the NORMAL case would have;
 *   if that is zero or below, shift right into subnormal position and pin the
 *     field at zero -- the field is zero BY DEFINITION for a subnormal, and
 *     recomputing it from the adjusted exponent afterwards is what made
 *     1.0f * 1e-38f come out one exponent too high with the right mantissa;
 *   round on the extra bits;
 *   let a carry out of rounding raise the exponent;
 *   overflow to infinity.
 */
/* `extra` used to be a parameter, and every one of the seven call sites passed
 * 3. That cost more than a parameter: `(u32)1 << (24 + extra)` with a variable
 * shift amount is a runtime shift LOOP, and it sat inside the two
 * normalisation loops, so the compiler could not hoist it. As a constant the
 * bounds fold to immediates. */
#define EXTRA 3

float __avrfp_round(int s, u32 m, int e, int sticky)
{
    int ex, k;
    u32 half, low;

    if (m == 0)
        return u2f(pack(s, 0, 0));

    /* Exactly 24 + extra significant bits. */
    while (m >= (u32)1 << (24 + EXTRA)) {
        sticky |= (int)(m & 1);
        m >>= 1;
        e++;
    }
    while (m < (u32)1 << (23 + EXTRA)) {
        m += m;
        e--;
    }

    /* The exponent field a normal result would carry. `e` applies to the
     * lowest of the EXTRA guard bits, so the 24-bit value's exponent is
     * e + EXTRA. */
    ex = e + EXTRA + BIAS + MANTBITS;

    if (ex <= 0) {
        /* Subnormal. Shift right by one more than the shortfall and pin the
         * field at zero. Past the point where nothing can survive, the
         * result is a zero of the right sign. */
        int sh = 1 - ex;
        if (sh > 24 + EXTRA + 1)
            return u2f(pack(s, 0, 0));
        for (k = 0; k < sh; k++) {
            sticky |= (int)(m & 1);
            m >>= 1;
        }
        ex = 0;
    }

    /* Round to nearest, ties to even, on the EXTRA low bits. */
    half = (u32)1 << (EXTRA - 1);
    low  = m & (((u32)1 << EXTRA) - 1);
    m >>= EXTRA;
    if (low > half || (low == half && (sticky || (m & 1))))
        m++;

    /* The increment can carry out of 24 bits. */
    if (m >= (u32)1 << 24) {
        m >>= 1;
        ex++;
    }
    /* ...and a subnormal can round UP into the smallest normal, which is
     * exactly right: its field becomes 1 and its implicit bit appears. */
    if (ex == 0 && m >= (u32)1 << 23)
        ex = 1;

    if (ex >= EXPMAX)
        return u2f(pack(s, EXPMAX, 0));
    return u2f(pack(s, ex, m & 0x7ffffful));
}

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfp_is_not_this_target;

#endif /* __AVR__ || __RX__ */
