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
typedef unsigned int       u32;
typedef unsigned __int128  u128;
typedef __int128           s128;

union w128 {
    u128 u;
    s128 s;
    struct { u64 lo, hi; } h;
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

#endif
