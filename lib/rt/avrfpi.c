/* binary32 <-> 32-bit integer, for AVR. Its own object: see lib/rt/avrfp.c.
 *
 * float->int TRUNCATES toward zero, which is what C requires of the cast and
 * is not what round_pack does; int->float rounds to nearest, because 32 bits
 * do not all fit in 24 and something has to give.
 */
/* RX too: GCC rx-elf's double is binary32 (-m32bit-doubles), so it needs
 * exactly this native binary32 and no binary64. */
#if defined(__AVR__) || defined(__RX__)

#include "avrfp.h"

float __floatunsisf(unsigned long v)
{
    if (v == 0)
        return u2f(pack(0, 0, 0));
    /* Round to nearest happens in round_pack; three guard bits are kept by
     * shifting the value up until its top bit is in place. */
    /* round_pack wants the value with three guard bits below the 24 it keeps,
     * and a 32-bit input with three guard bits is 35 -- too wide for the type.
     * So bring it down to 27 bits HERE, folding what falls off into sticky:
     * the three guard bits are then the bottom of what is left, and the
     * rounding has a real tie to break rather than only a sticky flag. */
    {
        u32 m = v;
        int e = 0, sticky = 0;
        while (m >= (u32)1 << 27) {
            sticky |= (int)(m & 1);
            m >>= 1;
            e++;
        }
        return __avrfp_round(0, m, e, sticky);
    }
}

float __floatsisf(long v)
{
    if (v < 0)
        return u2f(f2u(__floatunsisf(-(unsigned long)v)) ^ 0x80000000ul);
    return __floatunsisf((unsigned long)v);
}

/* A `long long` conversion, for a program that has both. */

unsigned long __fixunssfsi(float f)
{
    u32 u = f2u(f);
    u32 m;
    int e;

    if (is_nan(u) || fsign(u))
        return 0;
    if (is_inf(u))
        return ~(unsigned long)0;
    __avrfp_unpack(u, &m, &e);
    if (e >= 0) {
        u32 v = m;
        int k;
        for (k = 0; k < e; k++) {
            if (v > (0xfffffffful >> 1))
                return ~(unsigned long)0;
            v += v;
        }
        return v;
    }
    {
        int sh = -e, k;
        u32 v = m;
        if (sh > 31)
            return 0;
        for (k = 0; k < sh; k++)
            v >>= 1;
        return v;
    }
}

long __fixsfsi(float f)
{
    u32 u = f2u(f);
    if (is_nan(u))
        return 0;
    if (fsign(u)) {
        unsigned long v = __fixunssfsi(u2f(u & 0x7ffffffful));
        if (v > 0x80000000ul)
            return -0x7fffffffl - 1;
        return -(long)v;
    }
    {
        unsigned long v = __fixunssfsi(f);
        if (v > 0x7ffffffful)
            return (long)0x7ffffffful;           /* LONG_MAX */
        return (long)v;
    }
}

/* The 64-bit truncations. irgen uses a 64-bit intermediate for some
 * conversions -- so that an `unsigned long` result lands right rather than
 * saturating through a signed one -- so these are reached even by a program
 * whose own types are all 32-bit. */

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpi_is_not_this_target;

#endif /* __AVR__ || __RX__ */
