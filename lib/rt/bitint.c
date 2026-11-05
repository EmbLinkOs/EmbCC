/* _BitInt(N) wider than 64 bits: the arithmetic on its limbs.
 *
 * Such a value is an array of 64-bit limbs, least significant first, as
 * the x86-64 and AArch64 psABIs lay it out; the bits above N in the top
 * limb hold N's sign (signed) or zero (unsigned), as GCC keeps them. The
 * backend calls these for every operator on one (src/ir/irgen.c,
 * gen_bitint): the operands are addresses, the result goes where d
 * points, and `bits` is N -- negative for a signed type, which is what
 * the operations that differ by signedness look at, and what tells
 * norm() how to fill the top limb.
 *
 * Plain 64-bit arithmetic only, as everything here: nothing may use the
 * operation it implements. d may be either operand.
 */
#include "rt.h"

#define LIMBS(n) (((n) + 63) / 64)

static unsigned long width(long bits) { return (unsigned long)(bits < 0 ? -bits : bits); }

/* the bits above N: N's sign for a signed type, zero for an unsigned one */
static void norm(u64 *d, long bits)
{
    unsigned long n = width(bits), k = LIMBS(n), top = n % 64;
    if (!top)
        return;
    u64 mask = ((u64)1 << top) - 1;
    if (bits < 0 && ((d[k - 1] >> (top - 1)) & 1))
        d[k - 1] |= ~mask;
    else
        d[k - 1] &= mask;
}

static int negative(const u64 *a, long bits)
{
    unsigned long n = width(bits);
    return bits < 0 && ((a[(n - 1) / 64] >> ((n - 1) % 64)) & 1);
}

void __embcc_bi_copy(u64 *d, const u64 *a, long bits)
{
    unsigned long k = LIMBS(width(bits));
    for (unsigned long i = 0; i < k; i++)
        d[i] = a[i];
}

void __embcc_bi_add(u64 *d, const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 c = 0;
    for (unsigned long i = 0; i < k; i++) {
        u64 s = a[i] + c;
        u64 c1 = s < c;
        u64 t = s + b[i];
        c = c1 | (t < s);
        d[i] = t;
    }
    norm(d, bits);
}

void __embcc_bi_sub(u64 *d, const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 br = 0;
    for (unsigned long i = 0; i < k; i++) {
        u64 x = a[i], y = b[i];
        u64 t = x - y - br;
        br = (x < y) | ((x == y) & br);
        d[i] = t;
    }
    norm(d, bits);
}

/* a + k, k a small signed number: ++ and -- */
void __embcc_bi_addi(u64 *d, const u64 *a, s64 k, long bits)
{
    unsigned long n = LIMBS(width(bits));
    u64 add = (u64)k, ext = k < 0 ? ~(u64)0 : 0, c = 0;
    for (unsigned long i = 0; i < n; i++) {
        u64 y = i ? ext : add;
        u64 s = a[i] + c;
        u64 c1 = s < c;
        u64 t = s + y;
        c = c1 | (t < s);
        d[i] = t;
    }
    norm(d, bits);
}

void __embcc_bi_and(u64 *d, const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    for (unsigned long i = 0; i < k; i++)
        d[i] = a[i] & b[i];
}

void __embcc_bi_or(u64 *d, const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    for (unsigned long i = 0; i < k; i++)
        d[i] = a[i] | b[i];
}

void __embcc_bi_xor(u64 *d, const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    for (unsigned long i = 0; i < k; i++)
        d[i] = a[i] ^ b[i];
}

void __embcc_bi_not(u64 *d, const u64 *a, long bits)
{
    unsigned long k = LIMBS(width(bits));
    for (unsigned long i = 0; i < k; i++)
        d[i] = ~a[i];
    norm(d, bits);
}

void __embcc_bi_neg(u64 *d, const u64 *a, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 c = 1;
    for (unsigned long i = 0; i < k; i++) {
        u64 t = ~a[i] + c;
        c = c && t == 0;
        d[i] = t;
    }
    norm(d, bits);
}

/* the 128-bit product of two limbs, by 32-bit halves */
static void mul64(u64 x, u64 y, u64 *lo, u64 *hi)
{
    u64 xl = x & 0xffffffffu, xh = x >> 32, yl = y & 0xffffffffu, yh = y >> 32;
    u64 ll = xl * yl, lh = xl * yh, hl = xh * yl, hh = xh * yh;
    u64 mid = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
    *lo = (mid << 32) | (ll & 0xffffffffu);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* the low N bits of the product are the same for either signedness */
void __embcc_bi_mul(u64 *d, const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 r[k];
    for (unsigned long i = 0; i < k; i++)
        r[i] = 0;
    for (unsigned long i = 0; i < k; i++) {
        u64 carry = 0;
        for (unsigned long j = 0; i + j < k; j++) {
            u64 lo, hi;
            mul64(a[i], b[j], &lo, &hi);
            u64 s = r[i + j] + lo;
            u64 c1 = s < lo;
            u64 t = s + carry;
            u64 c2 = t < s;
            r[i + j] = t;
            carry = hi + c1 + c2;
        }
    }
    for (unsigned long i = 0; i < k; i++)
        d[i] = r[i];
    norm(d, bits);
}

static int ucmp(const u64 *a, const u64 *b, unsigned long k)
{
    for (unsigned long i = k; i-- > 0;)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* -1, 0 or 1: a against b */
int __embcc_bi_cmp(const u64 *a, const u64 *b, long bits)
{
    unsigned long k = LIMBS(width(bits));
    if (bits < 0) {
        int na = negative(a, bits), nb = negative(b, bits);
        if (na != nb)
            return na ? -1 : 1;
    }
    return ucmp(a, b, k);
}

int __embcc_bi_test(const u64 *a, long bits)
{
    unsigned long k = LIMBS(width(bits));
    for (unsigned long i = 0; i < k; i++)
        if (a[i])
            return 1;
    return 0;
}

/* shifts by count bits; past the width, all of it is shifted out */
void __embcc_bi_shl(u64 *d, const u64 *a, u64 count, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 r[k];
    unsigned long ls = count >= 64 * (u64)k ? k : (unsigned long)(count / 64);
    unsigned bs = (unsigned)(count % 64);
    for (unsigned long i = 0; i < k; i++) {
        u64 v = 0;
        if (i >= ls) {
            v = a[i - ls] << bs;
            if (bs && i > ls)
                v |= a[i - ls - 1] >> (64 - bs);
        }
        r[i] = v;
    }
    for (unsigned long i = 0; i < k; i++)
        d[i] = r[i];
    norm(d, bits);
}

/* >>: arithmetic for a signed type, logical for an unsigned one */
void __embcc_bi_shr(u64 *d, const u64 *a, u64 count, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 fill = negative(a, bits) ? ~(u64)0 : 0;
    u64 r[k];
    unsigned long ls = count >= 64 * (u64)k ? k : (unsigned long)(count / 64);
    unsigned bs = (unsigned)(count % 64);
    for (unsigned long i = 0; i < k; i++) {
        u64 lo = i + ls < k ? a[i + ls] : fill;
        u64 hi = i + ls + 1 < k ? a[i + ls + 1] : fill;
        r[i] = bs ? (lo >> bs) | (hi << (64 - bs)) : lo;
    }
    for (unsigned long i = 0; i < k; i++)
        d[i] = r[i];
    norm(d, bits);
}

/* unsigned division of k limbs, bit at a time: q and r */
static void udivmod(u64 *q, u64 *r, const u64 *a, const u64 *b, unsigned long k)
{
    for (unsigned long i = 0; i < k; i++)
        q[i] = r[i] = 0;
    for (unsigned long bit = 64 * k; bit-- > 0;) {
        /* r = r << 1 | a's bit */
        u64 carry = (a[bit / 64] >> (bit % 64)) & 1;
        for (unsigned long i = 0; i < k; i++) {
            u64 top = r[i] >> 63;
            r[i] = (r[i] << 1) | carry;
            carry = top;
        }
        if (carry || ucmp(r, b, k) >= 0) {
            u64 br = 0;
            for (unsigned long i = 0; i < k; i++) {
                u64 x = r[i], y = b[i];
                r[i] = x - y - br;
                br = (x < y) | ((x == y) & br);
            }
            q[bit / 64] |= (u64)1 << (bit % 64);
        }
    }
}

static void negate(u64 *x, unsigned long k)
{
    u64 c = 1;
    for (unsigned long i = 0; i < k; i++) {
        u64 t = ~x[i] + c;
        c = c && t == 0;
        x[i] = t;
    }
}

/* C's / and %: the quotient truncated toward zero, the remainder with the
 * dividend's sign. which: 0 the quotient, 1 the remainder. */
static void divmod(u64 *d, const u64 *a, const u64 *b, long bits, int which)
{
    unsigned long k = LIMBS(width(bits));
    u64 x[k], y[k], q[k], r[k];
    int na = negative(a, bits), nb = negative(b, bits);
    for (unsigned long i = 0; i < k; i++) {
        x[i] = a[i];
        y[i] = b[i];
    }
    if (na)
        negate(x, k);
    if (nb)
        negate(y, k);
    udivmod(q, r, x, y, k);
    if (which == 0 && na != nb)
        negate(q, k);
    if (which == 1 && na)
        negate(r, k);
    for (unsigned long i = 0; i < k; i++)
        d[i] = which ? r[i] : q[i];
    norm(d, bits);
}

void __embcc_bi_div(u64 *d, const u64 *a, const u64 *b, long bits) { divmod(d, a, b, bits, 0); }
void __embcc_bi_mod(u64 *d, const u64 *a, const u64 *b, long bits) { divmod(d, a, b, bits, 1); }

/* from a 64-bit integer, signed when sgn */
void __embcc_bi_from64(u64 *d, u64 v, int sgn, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 ext = sgn && (s64)v < 0 ? ~(u64)0 : 0;
    d[0] = v;
    for (unsigned long i = 1; i < k; i++)
        d[i] = ext;
    norm(d, bits);
}

/* the low 64 bits, which the caller truncates or extends further */
u64 __embcc_bi_to64(const u64 *a, long bits)
{
    (void)bits;
    return a[0];
}

/* from one _BitInt to another: extended by the source's signedness, then
 * cut to the destination's width */
void __embcc_bi_ext(u64 *d, long dbits, const u64 *a, long abits)
{
    unsigned long dk = LIMBS(width(dbits)), ak = LIMBS(width(abits));
    u64 fill = negative(a, abits) ? ~(u64)0 : 0;
    /* the source's top limb may hold bits above its width (norm keeps
     * them as the sign), so they are the extension already */
    for (unsigned long i = 0; i < dk; i++)
        d[i] = i < ak ? a[i] : fill;
    norm(d, dbits);
}

/* 2^e as a double, e >= 0 */
static double pow2(unsigned long e)
{
    double r = 1.0, p = 2.0;
    while (e) {
        if (e & 1)
            r *= p;
        p *= p;
        e >>= 1;
    }
    return r;
}

/* to a double, rounded once: the top 64 significant bits with a sticky
 * bit for the rest, converted, then scaled */
double __embcc_bi_to_double(const u64 *a, long bits)
{
    unsigned long k = LIMBS(width(bits));
    u64 x[k];
    int neg = negative(a, bits);
    for (unsigned long i = 0; i < k; i++)
        x[i] = a[i];
    if (neg)
        negate(x, k);
    long hi = -1;
    for (unsigned long i = k; i-- > 0;)
        if (x[i]) {
            unsigned long b = 63;
            while (!((x[i] >> b) & 1))
                b--;
            hi = (long)(64 * i + b);
            break;
        }
    if (hi < 0)
        return 0.0;
    double r;
    if (hi < 64) {
        r = (double)x[0];
    } else {
        unsigned long sh = (unsigned long)hi - 63;     /* bits below the window */
        unsigned long ls = sh / 64, bs = sh % 64;
        u64 top = x[ls] >> bs;
        if (bs && ls + 1 < k)
            top |= x[ls + 1] << (64 - bs);
        int sticky = (bs && (x[ls] & (((u64)1 << bs) - 1))) != 0;
        for (unsigned long i = 0; i < ls && !sticky; i++)
            sticky = x[i] != 0;
        top |= (u64)sticky;
        r = (double)top * pow2(sh);
    }
    return neg ? -r : r;
}

/* from a double, truncated toward zero; out of range is undefined, and
 * gives the low bits here */
void __embcc_bi_from_double(u64 *d, double v, long bits)
{
    unsigned long k = LIMBS(width(bits));
    int neg = v < 0;
    if (neg)
        v = -v;
    for (unsigned long i = 0; i < k; i++)
        d[i] = 0;
    /* the integer part, a limb at a time from the top that holds it */
    unsigned long n = 0;
    double scale = 1.0;
    while (v >= scale * 18446744073709551616.0 && n + 1 < k) {
        scale *= 18446744073709551616.0;
        n++;
    }
    for (unsigned long i = n + 1; i-- > 0;) {
        double q = v / scale;
        u64 limb = q >= 18446744073709551616.0 ? ~(u64)0 : (u64)q;
        d[i] = limb;
        v -= (double)limb * scale;
        scale /= 18446744073709551616.0;
    }
    if (neg)
        negate(d, k);
    norm(d, bits);
}
