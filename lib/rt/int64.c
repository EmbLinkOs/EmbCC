/* 64-bit integer division, for a machine whose registers are 32 bits.
 *
 * ARMv7-M divides 32 by 32 in hardware and has no instruction for 64 by
 * 64, so the Thumb backend emits a call for divide and remainder — the
 * same shape as the __int128 helpers beside this file, one size down.
 * Multiply and the shifts are NOT here: those the backend does inline,
 * because they are a handful of instructions each (umull and two mlas
 * for the product, a branch and five shifts for a shift).
 *
 * The names are libgcc's, so an object of ours links beside one of
 * theirs and a program that already pulls in libgcc gets these from
 * there instead.
 *
 * Nothing here divides: a 64-bit division inside the implementation of
 * 64-bit division is how this file would call itself forever. The loop
 * below uses only comparison, subtraction and single-bit shifts, all of
 * which the backend lowers without help.
 */

/* Only where the machine's registers are narrower than the type. On
 * LP64 the backends emit a divide instruction and never call these, and
 * defining them there would put a second `__divdi3` in the archive
 * beside libgcc's for no reason. */
/* Not on AVR, which has its own: lib/rt/avr64.c defines the same four names
 * plus __muldi3, written for a machine with no hardware multiply wider than
 * 8x8 and tested on the part. Both used to compile there -- this guard asked
 * only whether `long long` is wider than a pointer, which on a 2-byte-pointer
 * machine it very much is -- so an AVR librt.a held two __udivdi3s and which
 * one a program got depended on the order the archive was built in. */
#if __SIZEOF_LONG_LONG__ > __SIZEOF_POINTER__ && !defined(__AVR__)

typedef unsigned long long u64;
typedef long long s64;

/* Restoring division, one bit at a time, most significant first: the
 * remainder is shifted up with the next bit of the dividend and the
 * divisor subtracted when it fits. 64 iterations, no table and no
 * estimate — small and obviously right, which is what a routine every
 * other one depends on should be. */
static void udivmod64(u64 a, u64 b, u64 *quo, u64 *rem)
{
    u64 q = 0, r = 0;
    int i;

    if (b == 0) {                 /* undefined in C; do not hang */
        if (quo) *quo = 0;
        if (rem) *rem = 0;
        return;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1u);
        if (r >= b) {
            r -= b;
            q |= (u64)1 << i;
        }
    }
    if (quo) *quo = q;
    if (rem) *rem = r;
}

u64 __udivdi3(u64 a, u64 b)
{
    u64 q;
    udivmod64(a, b, &q, 0);
    return q;
}

u64 __umoddi3(u64 a, u64 b)
{
    u64 r;
    udivmod64(a, b, 0, &r);
    return r;
}

/* The signed forms, by magnitude. C99 rounds toward zero, so the
 * quotient's sign is the operands' exclusive-or and the remainder takes
 * the DIVIDEND's — which is why the two are not derived from each other
 * here but each given its own sign. */
s64 __divdi3(s64 a, s64 b)
{
    int neg = 0;
    u64 q;

    if (a < 0) { a = -a; neg = !neg; }
    if (b < 0) { b = -b; neg = !neg; }
    udivmod64((u64)a, (u64)b, &q, 0);
    return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
    int neg = 0;
    u64 r;

    if (a < 0) { a = -a; neg = 1; }
    if (b < 0) b = -b;
    udivmod64((u64)a, (u64)b, 0, &r);
    return neg ? -(s64)r : (s64)r;
}

#if defined(__mcoldfire__)
/* ColdFire also calls for a 64-bit multiply -- it has no 32 x 32 -> 64
 * product -- and for a 64-bit shift by a variable count. libgcc's names
 * again, and written with 32-bit operations only: a 64-bit multiply or
 * variable shift in here would be a call to itself. Shifts by a constant
 * 32 are inline (a word moves), so they split and join the halves. */
typedef unsigned int u32;

u64 __ashldi3(u64 a, int b)
{
    u32 hi = (u32)(a >> 32), lo = (u32)a;
    b &= 63;
    if (b == 0)
        return a;
    if (b >= 32) {
        hi = lo << (b - 32);
        lo = 0;
    } else {
        hi = (hi << b) | (lo >> (32 - b));
        lo <<= b;
    }
    return ((u64)hi << 32) | lo;
}

u64 __lshrdi3(u64 a, int b)
{
    u32 hi = (u32)(a >> 32), lo = (u32)a;
    b &= 63;
    if (b == 0)
        return a;
    if (b >= 32) {
        lo = hi >> (b - 32);
        hi = 0;
    } else {
        lo = (lo >> b) | (hi << (32 - b));
        hi >>= b;
    }
    return ((u64)hi << 32) | lo;
}

s64 __ashrdi3(s64 a, int b)
{
    int hi = (int)((u64)a >> 32);
    u32 lo = (u32)a;
    b &= 63;
    if (b == 0)
        return a;
    if (b >= 32) {
        lo = (u32)(hi >> (b - 32));
        hi = hi < 0 ? -1 : 0;
    } else {
        lo = (lo >> b) | ((u32)hi << (32 - b));
        hi >>= b;
    }
    return (s64)(((u64)(u32)hi << 32) | lo);
}

/* The low 64 bits of a product: the low words' full 64-bit product from
 * four 16 x 16 pieces, and the cross terms into the high word. */
u64 __muldi3(u64 a, u64 b)
{
    u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
    u32 a0 = al & 0xffff, a1 = al >> 16, b0 = bl & 0xffff, b1 = bl >> 16;
    u32 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u32 mid = (p00 >> 16) + (p01 & 0xffff) + (p10 & 0xffff);
    u32 hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    u32 lo = (mid << 16) | (p00 & 0xffff);
    hi += al * bh + ah * bl;
    return ((u64)hi << 32) | lo;
}
#endif

#else
/* A translation unit needs a declaration, and this one has none to
 * make on a machine that divides 64 bits by 64 in hardware. */
typedef int rt_int64_not_needed_here;
#endif
