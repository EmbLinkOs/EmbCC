/* 128-bit integer arithmetic on two eightbytes (w128.h). */
#include "w128.h"

struct w128 w_make(unsigned long lo, unsigned long hi)
{
    struct w128 r;
    r.lo = lo;
    r.hi = hi;
    return r;
}

struct w128 w_from(long v, int sign)
{
    return w_make((unsigned long)v, sign && v < 0 ? ~0UL : 0);
}

struct w128 w_add(struct w128 a, struct w128 b)
{
    unsigned long lo = a.lo + b.lo;
    return w_make(lo, a.hi + b.hi + (lo < a.lo));
}

struct w128 w_neg(struct w128 a)
{
    return w_add(w_make(~a.lo, ~a.hi), w_make(1, 0));
}

struct w128 w_mul(struct w128 a, struct w128 b)
{
    unsigned long a0 = a.lo & 0xffffffffUL, a1 = a.lo >> 32;
    unsigned long b0 = b.lo & 0xffffffffUL, b1 = b.lo >> 32;
    unsigned long p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    unsigned long mid = (p00 >> 32) + (p01 & 0xffffffffUL) +
                        (p10 & 0xffffffffUL);
    struct w128 r;
    r.lo = (p00 & 0xffffffffUL) | (mid << 32);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32) +
           a.lo * b.hi + a.hi * b.lo;
    return r;
}

struct w128 w_shl(struct w128 a, int n)
{
    n &= 127;
    if (n == 0)
        return a;
    if (n >= 64)
        return w_make(0, a.lo << (n - 64));
    return w_make(a.lo << n, (a.hi << n) | (a.lo >> (64 - n)));
}

struct w128 w_shr(struct w128 a, int n, int arith)
{
    n &= 127;
    unsigned long fill = arith && (long)a.hi < 0 ? ~0UL : 0;
    if (n == 0)
        return a;
    if (n >= 64)
        return w_make(n == 64 ? a.hi : (a.hi >> (n - 64)) |
                                       (fill << (128 - n)), fill);
    return w_make((a.lo >> n) | (a.hi << (64 - n)),
                  (a.hi >> n) | (fill << (64 - n)));
}

int w_ult(struct w128 a, struct w128 b)
{
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}

int w_slt(struct w128 a, struct w128 b)
{
    if (a.hi != b.hi)
        return (long)a.hi < (long)b.hi;
    return a.lo < b.lo;
}

void w_udivmod(struct w128 a, struct w128 b, struct w128 *q, struct w128 *r)
{
    struct w128 quo = w_make(0, 0), rem = w_make(0, 0);
    for (int i = 127; i >= 0; i--) {
        rem = w_shl(rem, 1);
        rem.lo |= (i >= 64 ? a.hi >> (i - 64) : a.lo >> i) & 1;
        if (!w_ult(rem, b)) {
            rem = w_add(rem, w_neg(b));
            if (i >= 64)
                quo.hi |= 1UL << (i - 64);
            else
                quo.lo |= 1UL << i;
        }
    }
    *q = quo;
    *r = rem;
}

int w_div(struct w128 a, struct w128 b, int sign, int mod, struct w128 *out)
{
    if (!b.lo && !b.hi)
        return 0;
    int na = sign && (long)a.hi < 0, nb = sign && (long)b.hi < 0;
    struct w128 q, r;
    w_udivmod(na ? w_neg(a) : a, nb ? w_neg(b) : b, &q, &r);
    if (!mod)
        *out = na != nb ? w_neg(q) : q;
    else
        *out = na ? w_neg(r) : r;
    return 1;
}
