/* binary32 TO a 64-bit integer, for AVR. Its own object: see lib/rt/avrfp.c
 * for the split and lib/rt/avrfpi64.c for why this direction is separate from
 * the other one.
 *
 * TRUNCATES toward zero, which is what C requires of the cast -- not what
 * round_pack does, so these two do not use it. The significand is 24 bits and
 * the exponent decides where it lands; a value with a negative exponent has
 * nothing left after truncation and is zero.
 */
#ifdef __AVR__

#include "avrfp.h"

typedef unsigned long long u64;

unsigned long long __fixunssfdi(float f)
{
    u32 u = f2u(f);
    u32 m;
    int e, k;

    if (is_nan(u) || fsign(u))
        return 0;
    if (is_inf(u))
        return ~(u64)0;
    __avrfp_unpack(u, &m, &e);
    if (e >= 0) {
        u64 v = m;
        for (k = 0; k < e; k++) {
            if (v > (~(u64)0 >> 1))
                return ~(u64)0;
            v += v;
        }
        return v;
    }
    {
        u64 v = m;
        int sh = -e;
        if (sh > 63)
            return 0;
        for (k = 0; k < sh; k++)
            v >>= 1;
        return v;
    }
}

long long __fixsfdi(float f)
{
    u32 u = f2u(f);

    if (is_nan(u))
        return 0;
    if (fsign(u)) {
        u64 v = __fixunssfdi(u2f(u & 0x7ffffffful));
        if (v > ((u64)1 << 63))
            return -(long long)((u64)1 << 63);      /* LLONG_MIN */
        return -(long long)v;
    }
    {
        u64 v = __fixunssfdi(f);
        if (v > (~(u64)0 >> 1))
            return (long long)(~(u64)0 >> 1);       /* LLONG_MAX */
        return (long long)v;
    }
}

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpfix64_is_not_this_target;

#endif /* __AVR__ */
