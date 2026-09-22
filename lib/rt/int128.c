/* 128-bit integer arithmetic: the operations neither machine has.
 *
 * x86-64 and aarch64 both multiply 64 bits by 64 bits into 128, and
 * neither divides 128 by 128 or shifts one by a variable amount, so
 * codegen emits a call for multiply, divide, remainder and the three
 * shifts. The names are libgcc's, because that is what every other
 * toolchain on these targets uses and an object of ours has to be
 * linkable beside one of theirs.
 *
 * See rt.h for why none of this uses `__int128` arithmetic.
 */
#include "rt.h"

/* ---- multiply -----------------------------------------------------------
 *
 * (ah:al) * (bh:bl), keeping the low 128 bits. The cross terms ah*bl and
 * al*bh contribute only to the high half -- their own high halves fall
 * off the top of the result -- so the only full 64x64->128 product
 * needed is al*bl, and that one is done in four 32-bit pieces because
 * neither the C language nor this library may assume a wider type. */
static u64 mul64_hi(u64 a, u64 b, u64 *lo_out)
{
    u64 al = a & 0xFFFFFFFFULL, ah = a >> 32;
    u64 bl = b & 0xFFFFFFFFULL, bh = b >> 32;

    u64 ll = al * bl;
    u64 lh = al * bh;
    u64 hl = ah * bl;
    u64 hh = ah * bh;

    /* The two middle products each straddle bit 32, so they are added
     * in with a carry out of the low half rather than simply shifted. */
    u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);

    *lo_out = (ll & 0xFFFFFFFFULL) | (mid << 32);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

u128 __multi3(u128 a, u128 b)
{
    u64 al = lo64(a), ah = hi64(a);
    u64 bl = lo64(b), bh = hi64(b);
    u64 lo, hi = mul64_hi(al, bl, &lo);

    hi += ah * bl + al * bh;       /* the cross terms, low 64 bits only */
    return mk(hi, lo);
}

/* ---- shifts -------------------------------------------------------------
 *
 * A shift of 64 or more moves the whole low half into the high one (or
 * the reverse), and -- the part that is easy to get wrong -- the
 * complementary shift `64 - n` is UNDEFINED when n is 0, because
 * shifting a 64-bit value by 64 is undefined in C. So the two cases are
 * separate branches rather than one expression, which is also why a
 * naive implementation appears to work until it is handed a zero. */
u128 __ashlti3(u128 a, int n)
{
    u64 lo = lo64(a), hi = hi64(a);
    if (n == 0)
        return a;
    if (n >= 64)
        return mk(lo << (n - 64), 0);
    return mk((hi << n) | (lo >> (64 - n)), lo << n);
}

u128 __lshrti3(u128 a, int n)
{
    u64 lo = lo64(a), hi = hi64(a);
    if (n == 0)
        return a;
    if (n >= 64)
        return mk(0, hi >> (n - 64));
    return mk(hi >> n, (lo >> n) | (hi << (64 - n)));
}

s128 __ashrti3(s128 a, int n)
{
    union w128 w;
    w.s = a;
    {
        u64 lo = w.h.lo;
        s64 hi = (s64)w.h.hi;
        if (n == 0)
            return a;
        if (n >= 64) {
            /* The sign fills the whole high half. */
            s64 top = hi >> 63;
            w.h.lo = (u64)(hi >> (n - 64));
            w.h.hi = (u64)top;
            return w.s;
        }
        w.h.lo = (lo >> n) | ((u64)hi << (64 - n));
        w.h.hi = (u64)(hi >> n);
        return w.s;
    }
}

/* ---- divide -------------------------------------------------------------
 *
 * Restoring shift-and-subtract, one bit at a time, most significant
 * first. 128 iterations for every division, which is slow and is the
 * honest starting point: it is correct for every input including the
 * ones a faster algorithm gets wrong (a divisor whose high half is
 * zero, a dividend smaller than the divisor, the full-width case).
 * Nothing outside this file knows, so a Knuth-D implementation later is
 * a local change.
 *
 * Division by zero is left to trap the way the machine's own divide
 * would: the C standard calls it undefined, and returning a number
 * would hide it. */
static void udivmod(u128 a, u128 b, u128 *quo, u128 *rem)
{
    u64 rlo = 0, rhi = 0;          /* the running remainder */
    u64 qlo = 0, qhi = 0;
    u64 alo = lo64(a), ahi = hi64(a);
    u64 blo = lo64(b), bhi = hi64(b);
    int i;

    for (i = 127; i >= 0; i--) {
        /* remainder <<= 1, and shift in bit i of the dividend */
        rhi = (rhi << 1) | (rlo >> 63);
        rlo <<= 1;
        if (i >= 64)
            rlo |= (ahi >> (i - 64)) & 1;
        else
            rlo |= (alo >> i) & 1;

        /* if remainder >= divisor: subtract, and set bit i of the quotient */
        if (rhi > bhi || (rhi == bhi && rlo >= blo)) {
            u64 borrow = rlo < blo;
            rlo -= blo;
            rhi -= bhi + borrow;
            if (i >= 64)
                qhi |= 1ULL << (i - 64);
            else
                qlo |= 1ULL << i;
        }
    }
    if (quo) *quo = mk(qhi, qlo);
    if (rem) *rem = mk(rhi, rlo);
}

u128 __udivti3(u128 a, u128 b) { u128 q; udivmod(a, b, &q, 0); return q; }
u128 __umodti3(u128 a, u128 b) { u128 r; udivmod(a, b, 0, &r); return r; }

/* The signed pair, by magnitude. C99 requires truncation toward zero and
 * a remainder with the DIVIDEND's sign, which is what negating after the
 * unsigned division gives -- and the two-step negation below is needed
 * because this file may not write `-x` on a 128-bit value either. */
static u128 neg128(u128 x)
{
    u64 lo = lo64(x), hi = hi64(x);
    u64 nlo = ~lo + 1;
    u64 nhi = ~hi + (nlo == 0);
    return mk(nhi, nlo);
}

static int is_neg(u128 x) { return (hi64(x) >> 63) != 0; }

s128 __divti3(s128 a, s128 b)
{
    union w128 wa, wb, wq;
    int neg;
    wa.s = a; wb.s = b;
    neg = is_neg(wa.u) ^ is_neg(wb.u);
    if (is_neg(wa.u)) wa.u = neg128(wa.u);
    if (is_neg(wb.u)) wb.u = neg128(wb.u);
    udivmod(wa.u, wb.u, &wq.u, 0);
    if (neg) wq.u = neg128(wq.u);
    return wq.s;
}

s128 __modti3(s128 a, s128 b)
{
    union w128 wa, wb, wr;
    int neg;
    wa.s = a; wb.s = b;
    neg = is_neg(wa.u);            /* the remainder takes the DIVIDEND's sign */
    if (is_neg(wa.u)) wa.u = neg128(wa.u);
    if (is_neg(wb.u)) wb.u = neg128(wb.u);
    udivmod(wa.u, wb.u, 0, &wr.u);
    if (neg) wr.u = neg128(wr.u);
    return wr.s;
}

s128 __negti2(s128 a)
{
    union w128 w;
    w.s = a;
    w.u = neg128(w.u);
    return w.s;
}
