/* The compiler runtime's own shared vocabulary.
 *
 * Every routine here is one the BACKEND calls: an operation the machine
 * has no instruction for, which codegen turns into a call rather than
 * into code. That is a different job from a C library's, which is why
 * this is not `lib/libc` -- libc implements what a PROGRAM asks for by
 * name, and nothing in a program ever writes `__multi3`.
 *
 * ---- the constraint that shapes every file here ---------------------------
 *
 * These routines may not use the operations they implement. `__multi3`
 * cannot multiply two `__int128`s, because that is a call to `__multi3`;
 * `__ashlti3` cannot shift one. So a 128-bit value is only ever taken
 * apart and put together through this union, and everything in between
 * is 64-bit arithmetic the machine really has.
 *
 * The union is not a trick: `__int128` is two 64-bit halves in memory on
 * both targets, little-endian, low half first, which is the same layout
 * the ABI passes it in. Reading `.h.lo` is a load, not a conversion.
 */
#ifndef EMBCC_RT_H
#define EMBCC_RT_H

typedef unsigned long long u64;
typedef long long          s64;
/* u32 is THIRTY-TWO bits, which `unsigned int` is not everywhere: on AVR an
 * `int` is sixteen. This was `typedef unsigned int u32` while every target
 * that included it had a 32-bit int, and it would have stayed silently wrong
 * on AVR -- lib/rt/complex.c reads a float's bits through a `u32` union
 * member, and a two-byte member reads half of them. */
#if __SIZEOF_INT__ >= 4
typedef unsigned int       u32;
#else
typedef unsigned long      u32;
#endif

/* Everything below that names __int128 exists only where the type does.
 * It was unconditional, so ANY file including this header failed on a 32- or
 * 8-bit target -- lib/rt/complex.c among them, which uses none of it, so
 * `float _Complex` multiply and divide had no runtime on any embedded
 * target. rt_mul64 is outside the guard: it is 64-bit arithmetic only. */
#ifdef __SIZEOF_INT128__
typedef unsigned __int128  u128;
typedef __int128           s128;

/* The halves in MEMORY order: the low one first little-endian, the high
 * one first big-endian (MIPS64's mips64-none-elf, the first big-endian
 * target with __int128 -- where {lo, hi} named each half the other). */
union w128 {
    u128 u;
    s128 s;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    struct { u64 hi, lo; } h;
#else
    struct { u64 lo, hi; } h;
#endif
};

static inline u128 mk(u64 hi, u64 lo)
{
    union w128 w;
    w.h.lo = lo;
    w.h.hi = hi;
    return w.u;
}

static inline u64 hi64(u128 x) { union w128 w; w.u = x; return w.h.hi; }
static inline u64 lo64(u128 x) { union w128 w; w.u = x; return w.h.lo; }
#endif /* __SIZEOF_INT128__ */

/* 64 x 64 -> 128, in four 32-bit pieces.
 *
 * The one primitive the whole library is built on, and the reason it is
 * written out rather than done in `u128` is the rule in the comment
 * above: a 128-bit multiply IS __multi3, so the routine that implements
 * __multi3 cannot use one. Shared because the soft-float needs it too --
 * a binary128 significand is 113 bits, so multiplying two of them is
 * four of these. */
static inline u64 rt_mul64(u64 a, u64 b, u64 *lo_out)
{
    u64 al = a & 0xFFFFFFFFULL, ah = a >> 32;
    u64 bl = b & 0xFFFFFFFFULL, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    /* The two middle products each straddle bit 32, so they are added
     * in with a carry out of the low half rather than simply shifted. */
    u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);

    *lo_out = (ll & 0xFFFFFFFFULL) | (mid << 32);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

#endif
