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

#if defined(__XTENSA__)
/* The 64-bit product's low 64 bits, for Xtensa: the LX6 core EmbCC
 * targets has mull (the low 32 bits of a 32-bit product) and no
 * instruction for the high word, so the backend calls this, as GCC calls
 * libgcc's. The high word of al * bl comes from the four 16-bit partial
 * products, each of which mull computes exactly; the cross terms reach
 * only the high word. No 64-bit multiply here: that would be this. */
typedef unsigned int u32_;
u64 __muldi3(u64 a, u64 b)
{
    u32_ al = (u32_)a, ah = (u32_)(a >> 32), bl = (u32_)b, bh = (u32_)(b >> 32);
    u32_ a0 = al & 0xffffu, a1 = al >> 16, b0 = bl & 0xffffu, b1 = bl >> 16;
    u32_ p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u32_ mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
    u32_ lo = (p00 & 0xffffu) | (mid << 16);
    u32_ hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    hi += al * bh + ah * bl;
    return ((u64)hi << 32) | lo;
}
#endif

#else
/* A translation unit needs a declaration, and this one has none to
 * make on a machine that divides 64 bits by 64 in hardware. */
typedef int rt_int64_not_needed_here;
#endif
