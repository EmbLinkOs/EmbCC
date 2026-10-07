/* The ARMv6-M (Cortex-M0, M0+, M1) compiler runtime: the routines the
 * backend calls for what Thumb-1 has no instruction for.
 *
 *   __aeabi_idiv, __aeabi_uidiv              32-bit division
 *   __aeabi_idivmod, __aeabi_uidivmod        quotient in r0, remainder in r1
 *   __aeabi_lmul                             64-bit multiply (no UMULL)
 *   __aeabi_llsl, __aeabi_llsr, __aeabi_lasr 64-bit shifts by a variable
 *   __aeabi_memcpy, __aeabi_memclr           a block copy or clear longer
 *                                            than the inline limit
 *   __atomic_*_N, __sync_val_compare_...     read-modify-writes: there are
 *                                            no exclusives (LDREX/STREX)
 *
 * The names are the ARM run-time ABI's (and libatomic's), because they are
 * what clang and GCC call for this architecture: an object of theirs links
 * against this archive and one of ours against libgcc.
 *
 * THE RULE of lib/rt (README.md): a routine may not use the operation it
 * implements. Division here is shift-and-subtract; the 64-bit multiply is
 * built from 16x16 products, which MULS does exactly; the shifts are done
 * on the two 32-bit words; the block routines are loops whose count is a
 * variable, which the optimizer cannot turn back into a call to them.
 *
 * Compiled to nothing on every other target: tools/build-rt.sh compiles
 * every lib/rt file for every triple.
 */
#if defined(__ARM_ARCH_6M__)

/* Weak, all of them: a program that brings its own (a freestanding one
 * built for clang often defines __aeabi_memcpy) keeps it. */
#define WEAK __attribute__((weak))

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

/* ---- division ------------------------------------------------------------- */

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

/* ---- 64-bit multiply and shifts -------------------------------------------- */

/* The full 64-bit product of two words, from four 16x16 products: each
 * fits 32 bits, which is all MULS gives. */
static void umul32(u32 a, u32 b, u32 *lo, u32 *hi)
{
    u32 a0 = a & 0xffffu, a1 = a >> 16, b0 = b & 0xffffu, b1 = b >> 16;
    u32 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u32 mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
    *lo = (mid << 16) | (p00 & 0xffffu);
    *hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
}

WEAK u64 __aeabi_lmul(u64 a, u64 b)
{
    u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
    u32 lo, hi;
    umul32(al, bl, &lo, &hi);
    hi += al * bh + ah * bl;
    return ((u64)hi << 32) | lo;
}

WEAK u64 __aeabi_llsl(u64 a, int n)
{
    u32 lo = (u32)a, hi = (u32)(a >> 32);
    n &= 63;
    if (n >= 32) {
        hi = lo << (n - 32);
        lo = 0;
    } else if (n) {
        hi = (hi << n) | (lo >> (32 - n));
        lo <<= n;
    }
    return ((u64)hi << 32) | lo;
}

WEAK u64 __aeabi_llsr(u64 a, int n)
{
    u32 lo = (u32)a, hi = (u32)(a >> 32);
    n &= 63;
    if (n >= 32) {
        lo = hi >> (n - 32);
        hi = 0;
    } else if (n) {
        lo = (lo >> n) | (hi << (32 - n));
        hi >>= n;
    }
    return ((u64)hi << 32) | lo;
}

WEAK s64 __aeabi_lasr(s64 a, int n)
{
    u32 lo = (u32)a;
    s32 hi = (s32)((u64)a >> 32);
    n &= 63;
    if (n >= 32) {
        lo = (u32)(hi >> (n - 32));
        hi = hi >> 31;
    } else if (n) {
        lo = (lo >> n) | ((u32)hi << (32 - n));
        hi >>= n;
    }
    return (s64)(((u64)(u32)hi << 32) | lo);
}

/* ---- blocks -----------------------------------------------------------------
 *
 * Words when both addresses are word-aligned -- ARMv6-M faults on an
 * unaligned one -- and bytes otherwise. */
WEAK void __aeabi_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((unsigned long)d | (unsigned long)s) & 3u) == 0) {
        while (n >= 4) {
            *(u32 *)d = *(const u32 *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n) {
        *d++ = *s++;
        n--;
    }
}

WEAK void __aeabi_memcpy4(void *dst, const void *src, unsigned long n)
{
    __aeabi_memcpy(dst, src, n);
}

WEAK void __aeabi_memclr(void *dst, unsigned long n)
{
    unsigned char *d = dst;
    if (((unsigned long)d & 3u) == 0) {
        while (n >= 4) {
            *(u32 *)d = 0;
            d += 4;
            n -= 4;
        }
    }
    while (n) {
        *d++ = 0;
        n--;
    }
}

WEAK void __aeabi_memclr4(void *dst, unsigned long n)
{
    __aeabi_memclr(dst, n);
}

/* ---- atomics ------------------------------------------------------------------
 *
 * With interrupts masked: on a single Cortex-M0 there is nothing else to
 * exclude, so a sequence with PRIMASK set is atomic. PRIMASK is saved and
 * restored rather than cleared, so these nest inside a critical section.
 *
 * Weak, because masking interrupts is a policy: an RTOS whose tasks run
 * unprivileged (where CPSID is ignored) or a part with a second bus master
 * defines its own and replaces these. */
static u32 irq_off(void)
{
    u32 m;
    __asm__ volatile ("mrs %0, primask" : "=r"(m));
    __asm__ volatile ("cpsid i" ::: "memory");
    return m;
}

static void irq_restore(u32 m)
{
    __asm__ volatile ("msr primask, %0" : : "r"(m) : "memory");
}

#define RMW(N, T, NAME, EXPR)                                              \
    WEAK T __atomic_##NAME##_##N(volatile T *p, T v, int order)            \
    {                                                                      \
        u32 m = irq_off();                                                 \
        T old = *p;                                                        \
        (void)order;                                                       \
        *p = (T)(EXPR);                                                    \
        irq_restore(m);                                                    \
        return old;                                                        \
    }

#define ALL(N, T)                                                          \
    RMW(N, T, exchange, v)                                                 \
    RMW(N, T, fetch_add, old + v)                                          \
    RMW(N, T, fetch_sub, old - v)                                          \
    RMW(N, T, fetch_and, old & v)                                          \
    RMW(N, T, fetch_or, old | v)                                           \
    RMW(N, T, fetch_xor, old ^ v)                                          \
    RMW(N, T, fetch_nand, ~(old & v))                                      \
    WEAK T __sync_val_compare_and_swap_##N(volatile T *p, T expect, T want) \
    {                                                                      \
        u32 m = irq_off();                                                 \
        T old = *p;                                                        \
        if (old == expect)                                                 \
            *p = want;                                                     \
        irq_restore(m);                                                    \
        return old;                                                        \
    }                                                                      \
    WEAK _Bool __atomic_compare_exchange_##N(volatile T *p, T *expect,      \
                                             T want, _Bool weak,            \
                                             int success, int failure)      \
    {                                                                      \
        u32 m = irq_off();                                                 \
        T old = *p;                                                        \
        _Bool ok = old == *expect;                                         \
        (void)weak; (void)success; (void)failure;                          \
        if (ok)                                                            \
            *p = want;                                                     \
        else                                                               \
            *expect = old;                                                 \
        irq_restore(m);                                                    \
        return ok;                                                         \
    }                                                                      \
    WEAK T __atomic_load_##N(const volatile T *p, int order)               \
    {                                                                      \
        u32 m = irq_off();                                                 \
        T v = *p;                                                          \
        (void)order;                                                       \
        irq_restore(m);                                                    \
        return v;                                                          \
    }                                                                      \
    WEAK void __atomic_store_##N(volatile T *p, T v, int order)            \
    {                                                                      \
        u32 m = irq_off();                                                 \
        (void)order;                                                       \
        *p = v;                                                            \
        irq_restore(m);                                                    \
    }

ALL(1, unsigned char)
ALL(2, unsigned short)
ALL(4, unsigned int)

#endif
