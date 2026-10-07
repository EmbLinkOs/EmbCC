/* IEEE-754 binary32 add and subtract, for AVR. Its own object: see
 * lib/rt/avrfp.c.
 *
 * The largest single routine in the implementation, and the one with the most
 * cases: the exponents have to be aligned before the significands can meet,
 * the smaller operand can vanish into the sticky bit entirely, a subtraction
 * of equal magnitudes has to produce +0 rather than -0, and every combination
 * of zero, subnormal, infinity and NaN has its own answer.
 */
/* RX too: GCC rx-elf's double is binary32 (-m32bit-doubles), so it needs
 * exactly this native binary32 and no binary64. */
#if defined(__AVR__) || defined(__RX__)

#include "avrfp.h"

/* ---- add and subtract ------------------------------------------------ */

static float addsub(u32 ua, u32 ub, int subtract)
{
    int sa = fsign(ua), sb = fsign(ub) ^ (subtract & 1);
    u32 ma, mb;
    int ea, eb;

    if (is_nan(ua) || is_nan(ub))
        return u2f(QNAN);
    if (is_inf(ua) || is_inf(ub)) {
        if (is_inf(ua) && is_inf(ub) && sa != sb)
            return u2f(QNAN);                  /* inf - inf */
        return u2f(pack(is_inf(ua) ? sa : sb, EXPMAX, 0));
    }
    __avrfp_unpack(ua, &ma, &ea);
    __avrfp_unpack(ub, &mb, &eb);
    if (ma == 0 && mb == 0) {
        /* Both zero. The sign is negative only when both are, except that
         * round-to-nearest makes (-0) + (+0) a positive zero. */
        return u2f(pack(sa && sb ? 1 : 0, 0, 0));
    }
    if (ma == 0) return u2f(pack(sb, fexp(ub), fmant(ub)));
    if (mb == 0) return u2f(pack(sa, fexp(ua), fmant(ua)));

    /* Align. Three guard bits are kept below the significand so that the
     * rounding decision has something to see; anything shifted out below
     * them becomes sticky. */
    {
        u32 xa = ma << 3, xb = mb << 3;
        int e = ea - 3, sticky = 0;
        int d = ea - eb;
        if (d > 0) {
            if (d > 32) { sticky = (xb != 0); xb = 0; }
            else {
                int k;
                for (k = 0; k < d; k++) { sticky |= (int)(xb & 1); xb >>= 1; }
            }
        } else if (d < 0) {
            d = -d;
            e = eb - 3;
            if (d > 32) { sticky = (xa != 0); xa = 0; }
            else {
                int k;
                for (k = 0; k < d; k++) { sticky |= (int)(xa & 1); xa >>= 1; }
            }
        }
        if (sa == sb)
            return __avrfp_round(sa, xa + xb, e, sticky);
        /* Opposite signs: a subtraction. The sticky bit belongs to the
         * SMALLER operand, so it makes the difference slightly larger in
         * magnitude for whichever side lost bits -- which is why it is
         * subtracted from the surviving value rather than ignored. */
        /* Exact cancellation FIRST: equal magnitudes with nothing shifted
         * out is an exact zero, and IEEE-754 makes that POSITIVE under
         * round-to-nearest whatever the operands' signs were. Testing it
         * after the magnitude comparison let (-1) + 1 keep the left operand's
         * sign and produce -0. */
        if (xa == xb && !sticky)
            return u2f(pack(0, 0, 0));
        /* The sticky bit belongs to the SMALLER operand, so it makes the
         * difference slightly smaller in magnitude on the side that lost
         * bits -- which is why it is subtracted rather than ignored. */
        if (xa > xb || (xa == xb && sticky && sb))
            return __avrfp_round(sa, xa - xb - (u32)(sticky ? 1 : 0), e,
                              sticky);
        return __avrfp_round(sb, xb - xa - (u32)(sticky ? 1 : 0), e, sticky);
    }
}

float __addsf3(float a, float b) { return addsub(f2u(a), f2u(b), 0); }
float __subsf3(float a, float b) { return addsub(f2u(a), f2u(b), 1); }
float __negsf2(float a) { return u2f(f2u(a) ^ 0x80000000ul); }

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpadd_is_not_this_target;

#endif /* __AVR__ || __RX__ */
