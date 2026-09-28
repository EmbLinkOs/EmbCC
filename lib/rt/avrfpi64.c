/* A 64-bit integer TO binary32, for AVR. Its own object: see lib/rt/avrfp.c.
 *
 * Split from the other direction (lib/rt/avrfpfix64.c) rather than sharing an
 * object with it, which is one split further than the rest of this
 * implementation goes. The reason is the size: at -O0 the two directions
 * together are 19 KB, because a 64-bit shift in this backend is a
 * byte-at-a-time chain through frame slots and both of them shift in a loop.
 * That is more than half the part's flash for two casts.
 *
 * These two are the smaller pair. The value is brought down to 27 bits with
 * everything below folded into the sticky bit, and rounded from there --
 * 64 bits do not fit in 24, so something has to give, and what gives is
 * decided by round-to-nearest rather than by truncation.
 */
#ifdef __AVR__

#include "avrfp.h"

/* The only 64-bit type in the whole float implementation, and only because
 * these routines' operand or result is that wide. */
typedef unsigned long long u64;

float __floatundisf(unsigned long long v)
{
    if (v == 0)
        return u2f(pack(0, 0, 0));
    {
        unsigned long long t = v;
        u32 m;
        int e = 0, sticky = 0;
        while (t >= (unsigned long long)1 << 27) {
            sticky |= (int)(t & 1);
            t >>= 1;
            e++;
        }
        m = (u32)t;
        return __avrfp_round(0, m, e, sticky);
    }
}

float __floatdisf(long long v)
{
    if (v < 0)
        return u2f(f2u(__floatundisf(-(unsigned long long)v)) ^ 0x80000000ul);
    return __floatundisf((unsigned long long)v);
}

/* Truncation toward zero, which is what C requires of a cast. */

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpi64_is_not_this_target;

#endif /* __AVR__ */
