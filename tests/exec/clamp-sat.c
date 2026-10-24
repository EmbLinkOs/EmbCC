// expect-exit: 42
/* Clamps to a bit range, which the Cortex-M backend makes one ssat or usat
 * (src/arch/thumb/codegen.c, t_clamp_sat): both orders of the two halves,
 * each half written either way round, >= and <= as well as > and <, and
 * the near misses that must stay two clamps -- a range that is not a power
 * of two, an unsigned compare, a low bound of -1. Each is tried at its
 * bounds, one past them and at the ends of int. */

#include <limits.h>

#define NOINLINE __attribute__((noinline))

NOINLINE static int s16(int y)        /* ssat #16 */
{
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    return y;
}
NOINLINE static int s16_low_first(int y)
{
    if (y < -32768) y = -32768;
    if (y > 32767) y = 32767;
    return y;
}
NOINLINE static int s8_arms(int y)    /* the arms the other way round */
{
    y = y < 127 ? y : 127;
    y = y > -128 ? y : -128;
    return y;
}
NOINLINE static int s12_ge(int y)
{
    if (y >= 2047) y = 2047;
    if (y <= -2048) y = -2048;
    return y;
}
NOINLINE static int s1(int y)         /* ssat #1: [-1, 0] */
{
    if (y > 0) y = 0;
    if (y < -1) y = -1;
    return y;
}
NOINLINE static int u8(int y)         /* usat #8 */
{
    if (y > 255) y = 255;
    if (y < 0) y = 0;
    return y;
}
NOINLINE static int u1_low_first(int y)
{
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    return y;
}
NOINLINE static int u31(int y)
{
    if (y > INT_MAX) y = INT_MAX;     /* never: folded, or a clamp of one */
    if (y < 0) y = 0;
    return y;
}
/* near misses */
NOINLINE static int r100(int y)
{
    if (y > 100) y = 100;
    if (y < -100) y = -100;
    return y;
}
NOINLINE static int off_by_one(int y) /* [-1, 254]: not a usat */
{
    if (y > 254) y = 254;
    if (y < -1) y = -1;
    return y;
}
NOINLINE static unsigned unsigned_cmp(unsigned y)
{
    if (y > 255u) y = 255u;
    if (y < 16u) y = 16u;
    return y;
}

static int ref(long long y, long long lo, long long hi)
{
    return (int)(y < lo ? lo : y > hi ? hi : y);
}

static const int probe[] = {
    INT_MIN, INT_MIN + 1, -70000, -32769, -32768, -32767, -2049, -2048,
    -2047, -256, -129, -128, -127, -101, -100, -99, -2, -1, 0, 1, 2, 99,
    100, 101, 126, 127, 128, 254, 255, 256, 2046, 2047, 2048, 32766, 32767,
    32768, 70000, INT_MAX - 1, INT_MAX,
};

int main(void)
{
    for (unsigned k = 0; k < sizeof probe / sizeof probe[0]; k++) {
        int y = probe[k];
        if (s16(y) != ref(y, -32768, 32767)) return 1;
        if (s16_low_first(y) != ref(y, -32768, 32767)) return 2;
        if (s8_arms(y) != ref(y, -128, 127)) return 3;
        if (s12_ge(y) != ref(y, -2048, 2047)) return 4;
        if (s1(y) != ref(y, -1, 0)) return 5;
        if (u8(y) != ref(y, 0, 255)) return 6;
        if (u1_low_first(y) != ref(y, 0, 1)) return 7;
        if (u31(y) != ref(y, 0, INT_MAX)) return 8;
        if (r100(y) != ref(y, -100, 100)) return 9;
        if (off_by_one(y) != ref(y, -1, 254)) return 10;
        unsigned u = (unsigned)y;
        unsigned want = u > 255u ? 255u : u < 16u ? 16u : u;
        if (unsigned_cmp(u) != want) return 11;
    }
    return 42;
}
