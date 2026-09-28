/* IEEE-754 binary32 divide, for AVR. Its own object: see lib/rt/avrfp.c.
 *
 * Restoring division, a bit at a time, 27 times. The pre-scale before the
 * loop is not an optimisation: without it the invariant `rem < den` does not
 * hold on the first iteration and 3.0f / 1.0f came out 2.0f.
 */
#ifdef __AVR__

#include "avrfp.h"

float __divsf3(float a, float b)
{
    u32 ua = f2u(a), ub = f2u(b);
    int s = fsign(ua) ^ fsign(ub);
    u32 ma, mb;
    int ea, eb;

    if (is_nan(ua) || is_nan(ub))
        return u2f(QNAN);
    if (is_inf(ua)) {
        if (is_inf(ub))
            return u2f(QNAN);                  /* inf / inf */
        return u2f(pack(s, EXPMAX, 0));
    }
    if (is_inf(ub))
        return u2f(pack(s, 0, 0));             /* finite / inf */
    __avrfp_unpack(ua, &ma, &ea);
    __avrfp_unpack(ub, &mb, &eb);
    if (mb == 0) {
        if (ma == 0)
            return u2f(QNAN);                  /* 0 / 0 */
        return u2f(pack(s, EXPMAX, 0));        /* x / 0 is infinity */
    }
    if (ma == 0)
        return u2f(pack(s, 0, 0));
    /* Both significands have to be NORMALISED before the division, because
     * unpack deliberately leaves a subnormal's alone -- that is what lets the
     * add and multiply paths treat subnormals without a special case. A
     * division cannot: its pre-scale and its one-subtraction-per-step both
     * assume the operands are in [2^23, 2^24). Without this, every division
     * with a subnormal operand was wrong, and only those. */
    while (ma < 0x800000ul) { ma += ma; ea--; }
    while (mb < 0x800000ul) { mb += mb; eb--; }
    {
        /* Restoring division, and the pre-scale is the part that matters.
         *
         * The algorithm needs the remainder to stay BELOW the divisor, which
         * means starting with it below: both significands are in
         * [2^23, 2^24), so the dividend can be up to twice the divisor. One
         * conditional doubling puts the ratio in [1, 2), after which the
         * leading quotient bit is known to be 1 and every step subtracts at
         * most once. Without it the remainder grew without bound and 3.0/1.0
         * came out as 2.0.
         *
         * 27 bits of quotient: the 24 that are kept and three guard bits. */
        u32 rem, q;
        int e = ea - eb - 26, k;
        if (ma < mb) {
            ma += ma;
            e--;
        }
        rem = ma - mb;
        q = 1;
        for (k = 0; k < 26; k++) {
            rem += rem;
            q += q;
            if (rem >= mb) {
                rem -= mb;
                q |= 1;
            }
        }
        return __avrfp_round(s, q, e, rem != 0);
    }
}

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpdiv_is_not_this_target;

#endif /* __AVR__ */
