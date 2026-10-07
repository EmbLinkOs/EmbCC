/* IEEE-754 binary32 comparisons, for AVR. Its own object: see lib/rt/avrfp.c.
 *
 * All seven libgcc entry points come out of one three-way compare. They differ
 * only in what an unordered pair answers, which is what `nan_result` is: the
 * sign that makes the CALLER's test fail. __gtsf2 and __gesf2 must answer
 * negative for unordered and the other five positive, because the backend
 * tests the result against zero and both directions have to be false.
 */
/* RX too: GCC rx-elf's double is binary32 (-m32bit-doubles), so it needs
 * exactly this native binary32 and no binary64. */
#if defined(__AVR__) || defined(__RX__)

#include "avrfp.h"

/* ---- comparisons ----------------------------------------------------
 *
 * libgcc's conventions, which the backend's fp_cmp_name family names:
 * each returns a value whose sign answers the question, and any comparison
 * involving a NaN answers "not equal" and "not less" and "not greater" --
 * which the callers spell by testing the result against zero.
 */
static int cmp2(u32 ua, u32 ub, int nan_result)
{
    int sa, sb;

    if (is_nan(ua) || is_nan(ub))
        return nan_result;
    /* Both zeroes compare equal whatever their signs. */
    if ((ua & 0x7ffffffful) == 0 && (ub & 0x7ffffffful) == 0)
        return 0;
    sa = fsign(ua);
    sb = fsign(ub);
    if (sa != sb)
        return sa ? -1 : 1;
    /* Same sign: the bit patterns of positive floats order the same way the
     * values do, and for negatives the order is reversed. */
    if ((ua & 0x7ffffffful) == (ub & 0x7ffffffful))
        return 0;
    if ((ua & 0x7ffffffful) < (ub & 0x7ffffffful))
        return sa ? 1 : -1;
    return sa ? -1 : 1;
}

int __cmpsf2(float a, float b)   { return cmp2(f2u(a), f2u(b), 1); }
int __eqsf2(float a, float b)    { return cmp2(f2u(a), f2u(b), 1); }
int __nesf2(float a, float b)    { return cmp2(f2u(a), f2u(b), 1); }
int __ltsf2(float a, float b)    { return cmp2(f2u(a), f2u(b), 1); }
int __lesf2(float a, float b)    { return cmp2(f2u(a), f2u(b), 1); }
int __gtsf2(float a, float b)    { return cmp2(f2u(a), f2u(b), -1); }
int __gesf2(float a, float b)    { return cmp2(f2u(a), f2u(b), -1); }
int __unordsf2(float a, float b) { return is_nan(f2u(a)) || is_nan(f2u(b)); }

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpcmp_is_not_this_target;

#endif /* __AVR__ || __RX__ */
