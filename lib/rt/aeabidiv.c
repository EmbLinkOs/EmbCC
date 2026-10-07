/* The RTABI's 32-bit division for an ARM core that has no divide
 * instruction: ARMv6-M (Cortex-M0, M0+, M1), and ARMv7-A in ARM state
 * (armv7a-none-eabi -- the Cortex-A7 and A15 have SDIV/UDIV, base ARMv7-A
 * does not, and the backend calls these for every / and % there).
 *
 *   __aeabi_idiv, __aeabi_uidiv              the quotient
 *   __aeabi_idivmod, __aeabi_uidivmod        quotient in r0, remainder in r1
 *
 * The names are the ARM run-time ABI's, which clang and GCC call too, so
 * an object of theirs links against this archive. Every core that HAS the
 * instruction (__ARM_FEATURE_IDIV: ARMv7-M, v7E-M, v8-M Mainline) gets
 * these from aeabi.c as one sdiv/udiv each instead.
 *
 * THE RULE of lib/rt (README.md): a routine may not use the operation it
 * implements. Division here is shift-and-subtract.
 *
 * Compiled to nothing on every other target: tools/build-rt.sh compiles
 * every lib/rt file for every triple.
 */
#if defined(__ARM_EABI__) && !defined(__ARM_FEATURE_IDIV)

/* Weak, all of them: a program that brings its own keeps it. */
#define WEAK __attribute__((weak))

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;

/* The quotient and remainder of n / d, d nonzero: restoring division one
 * bit at a time, after lining the divisor up under the dividend's top bit
 * so a small quotient costs few steps. */
static u32 udivmod(u32 n, u32 d, u32 *rem)
{
    u32 q = 0, bit = 1;
    if (d == 0) {                     /* undefined in C; do not hang */
        *rem = 0;
        return 0;
    }
    while (d <= n && !(d & 0x80000000u)) {
        d <<= 1;
        bit <<= 1;
    }
    while (bit) {
        if (n >= d) {
            n -= d;
            q |= bit;
        }
        d >>= 1;
        bit >>= 1;
    }
    *rem = n;
    return q;
}

WEAK u32 __aeabi_uidiv(u32 n, u32 d)
{
    u32 r;
    return udivmod(n, d, &r);
}

WEAK s32 __aeabi_idiv(s32 n, s32 d)
{
    u32 r, un = n < 0 ? 0u - (u32)n : (u32)n, ud = d < 0 ? 0u - (u32)d : (u32)d;
    u32 q = udivmod(un, ud, &r);
    return (n < 0) != (d < 0) ? (s32)(0u - q) : (s32)q;
}

/* {quotient, remainder} in r0 and r1: a 64-bit value's two words, low
 * first, which is how AAPCS32 returns one. */
WEAK u64 __aeabi_uidivmod(u32 n, u32 d)
{
    u32 r, q = udivmod(n, d, &r);
    return ((u64)r << 32) | q;
}

WEAK u64 __aeabi_idivmod(s32 n, s32 d)
{
    u32 r, un = n < 0 ? 0u - (u32)n : (u32)n, ud = d < 0 ? 0u - (u32)d : (u32)d;
    u32 q = udivmod(un, ud, &r);
    if ((n < 0) != (d < 0))
        q = 0u - q;
    if (n < 0)                        /* the remainder has the dividend's sign */
        r = 0u - r;
    return ((u64)r << 32) | q;
}

#endif
