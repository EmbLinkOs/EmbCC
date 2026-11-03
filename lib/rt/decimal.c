/* C23's decimal floating types, in BID (the binary-integer encoding the
 * x86-64 and AArch64 psABIs use, as GCC's libgcc does): the operations,
 * comparisons and conversions the backend calls for _Decimal32, 64 and
 * 128 (src/ir/irgen.c, gen_decimal), under libgcc's names, so that an
 * object GCC compiled links with this runtime and the other way round.
 *
 * A value is decoded to a sign, a coefficient and an exponent (the
 * quantum: value = coefficient * 10^exponent); the result is computed
 * exactly in a big integer and rounded once to the format's digits,
 * half to even -- the default rounding of IEEE 754-2008 decimal
 * arithmetic -- with overflow to infinity and gradual underflow. The
 * result's exponent is the preferred one the standard gives each
 * operation, where the result is exact.
 *
 * The values travel as the binary type of the same size and register
 * class (float, double, and long double on AArch64 -- binary128 in a q
 * register -- or unsigned __int128 on x86-64), and only their bits are
 * used. Nothing here uses a decimal type, which would be a call into
 * this file.
 */
#include "rt.h"

#if defined(__x86_64__) || defined(__aarch64__)

typedef unsigned int bid32;
#if defined(__aarch64__)
typedef long double td_carrier;
#else
typedef unsigned __int128 td_carrier;
#endif

/* ---- big integers: little-endian 64-bit limbs ---------------------------- */

#define BL 48                       /* 3072 bits: a double's value exactly */

struct big { u64 l[BL]; int n; };   /* n limbs in use (0: the value 0) */

static void b_set(struct big *b, u64 v)
{
    b->l[0] = v;
    b->n = v != 0;
}

static void b_trim(struct big *b)
{
    while (b->n && !b->l[b->n - 1])
        b->n--;
}

static int b_zero(const struct big *b) { return b->n == 0; }

/* b = b * m + a, m and a small */
static void b_muladd(struct big *b, u64 m, u64 a)
{
    u64 carry = a;
    for (int i = 0; i < b->n; i++) {
        u64 x = b->l[i];
        u64 xl = x & 0xffffffffu, xh = x >> 32, ml = m & 0xffffffffu, mh = m >> 32;
        u64 ll = xl * ml, lh = xl * mh, hl = xh * ml, hh = xh * mh;
        u64 mid = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
        u64 lo = (mid << 32) | (ll & 0xffffffffu);
        u64 hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
        lo += carry;
        hi += lo < carry;
        b->l[i] = lo;
        carry = hi;
    }
    if (carry && b->n < BL)
        b->l[b->n++] = carry;
}

/* b /= d (small), the remainder returned */
static u64 b_divsmall(struct big *b, u64 d)
{
    u64 r = 0;
    for (int i = b->n; i-- > 0;) {
        /* (r:l[i]) / d, by 32-bit halves: r < d < 2^32 here */
        u64 hi = (r << 32) | (b->l[i] >> 32);
        u64 qh = hi / d;
        r = hi % d;
        u64 lo = (r << 32) | (b->l[i] & 0xffffffffu);
        u64 ql = lo / d;
        r = lo % d;
        b->l[i] = (qh << 32) | ql;
    }
    b_trim(b);
    return r;
}

static void b_mul10k(struct big *b, int k)
{
    while (k >= 9) {
        b_muladd(b, 1000000000u, 0);
        k -= 9;
    }
    static const u64 p10[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000,
                                100000000 };
    if (k)
        b_muladd(b, p10[k], 0);
}

static int b_cmp(const struct big *a, const struct big *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n; i-- > 0;)
        if (a->l[i] != b->l[i])
            return a->l[i] < b->l[i] ? -1 : 1;
    return 0;
}

static void b_add(struct big *a, const struct big *b)
{
    u64 c = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        u64 x = i < a->n ? a->l[i] : 0, y = i < b->n ? b->l[i] : 0;
        u64 s = x + c;
        u64 c1 = s < c;
        u64 t = s + y;
        c = c1 | (t < s);
        a->l[i] = t;
    }
    a->n = n;
    if (c && a->n < BL)
        a->l[a->n++] = c;
}

/* a -= b, a >= b */
static void b_sub(struct big *a, const struct big *b)
{
    u64 br = 0;
    for (int i = 0; i < a->n; i++) {
        u64 x = a->l[i], y = i < b->n ? b->l[i] : 0;
        u64 t = x - y - br;
        br = (x < y) | ((x == y) & br);
        a->l[i] = t;
    }
    b_trim(a);
}

static void b_mul(struct big *r, const struct big *a, const struct big *b)
{
    struct big t;
    t.n = a->n + b->n;
    if (t.n > BL)
        t.n = BL;
    for (int i = 0; i < t.n; i++)
        t.l[i] = 0;
    for (int i = 0; i < a->n; i++) {
        u64 carry = 0;
        for (int j = 0; j < b->n && i + j < BL; j++) {
            u64 x = a->l[i], y = b->l[j];
            u64 xl = x & 0xffffffffu, xh = x >> 32, yl = y & 0xffffffffu, yh = y >> 32;
            u64 ll = xl * yl, lh = xl * yh, hl = xh * yl, hh = xh * yh;
            u64 mid = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
            u64 lo = (mid << 32) | (ll & 0xffffffffu);
            u64 hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
            u64 s = t.l[i + j] + lo;
            u64 c1 = s < lo;
            u64 s2 = s + carry;
            u64 c2 = s2 < s;
            t.l[i + j] = s2;
            carry = hi + c1 + c2;
        }
        if (i + b->n < BL)
            t.l[i + b->n] += carry;
    }
    b_trim(&t);
    *r = t;
}

static int b_bits(const struct big *b)
{
    if (!b->n)
        return 0;
    int k = 64 * (b->n - 1);
    for (u64 t = b->l[b->n - 1]; t; t >>= 1)
        k++;
    return k;
}

static int b_bit(const struct big *b, int i)
{
    return i / 64 < b->n ? (int)((b->l[i / 64] >> (i % 64)) & 1) : 0;
}

/* q = a / d, r = a % d, by shift and subtract */
static void b_divmod(struct big *q, struct big *r, const struct big *a, const struct big *d)
{
    struct big qq, rr;
    qq.n = a->n;
    for (int i = 0; i < qq.n; i++)
        qq.l[i] = 0;
    rr.n = 0;
    for (int bit = b_bits(a); bit-- > 0;) {
        /* rr = rr * 2 + a's bit */
        u64 c = (u64)b_bit(a, bit);
        for (int i = 0; i < rr.n; i++) {
            u64 top = rr.l[i] >> 63;
            rr.l[i] = (rr.l[i] << 1) | c;
            c = top;
        }
        if (c && rr.n < BL)
            rr.l[rr.n++] = c;
        if (b_cmp(&rr, d) >= 0) {
            b_sub(&rr, d);
            qq.l[bit / 64] |= (u64)1 << (bit % 64);
        }
    }
    b_trim(&qq);
    if (q)
        *q = qq;
    if (r)
        *r = rr;
}

static int b_digits(const struct big *b)
{
    struct big t = *b;
    int d = 0;
    while (!b_zero(&t)) {
        b_divsmall(&t, 10);
        d++;
    }
    return d;
}

/* ---- the formats ------------------------------------------------------- */

enum { F32, F64, F128 };

struct fmt {
    int p;          /* digits */
    int qmin, qmax; /* the quantum exponent's range */
    int bias;
};

static const struct fmt fmts[3] = {
    { 7, -101, 90, 101 },
    { 16, -398, 369, 398 },
    { 34, -6176, 6111, 6176 },
};

enum { K_FIN, K_INF, K_QNAN, K_SNAN };

struct dec {
    int kind;
    int neg;
    int exp;
    struct big c;
};

/* the bits, as two 64-bit halves (hi is 0 below 128) */
struct bits { u64 lo, hi; };

static void unpack(int f, struct bits v, struct dec *d)
{
    d->neg = 0;
    d->exp = 0;
    d->kind = K_FIN;
    d->c.n = 0;
    if (f == F128) {
        u64 hi = v.hi, lo = v.lo;
        d->neg = (int)(hi >> 63);
        if (((hi >> 58) & 0x1f) == 0x1f) {
            d->kind = (hi >> 57) & 1 ? K_SNAN : K_QNAN;
            return;
        }
        if (((hi >> 58) & 0x1f) == 0x1e) {
            d->kind = K_INF;
            return;
        }
        if (((hi >> 61) & 3) == 3) {
            /* the large-coefficient form: past 10^34, non-canonical, 0 */
            d->exp = (int)((hi >> 47) & 0x3fff) - fmts[f].bias;
            return;
        }
        d->exp = (int)((hi >> 49) & 0x3fff) - fmts[f].bias;
        d->c.l[0] = lo;
        d->c.l[1] = hi & 0x1ffffffffffffULL;
        d->c.n = 2;
        b_trim(&d->c);
        /* over 10^34 - 1 is non-canonical: zero */
        struct big lim;
        b_set(&lim, 1);
        b_mul10k(&lim, 34);
        if (b_cmp(&d->c, &lim) >= 0)
            d->c.n = 0;
        return;
    }
    int w = f == F32 ? 32 : 64, ew = f == F32 ? 8 : 10, t = w - 1 - ew;  /* 23, 53 */
    u64 x = v.lo;
    d->neg = (int)(x >> (w - 1));
    u64 g = (x >> (w - 6)) & 0x1f;
    if (g == 0x1f) {
        d->kind = (x >> (w - 7)) & 1 ? K_SNAN : K_QNAN;
        return;
    }
    if (g == 0x1e) {
        d->kind = K_INF;
        return;
    }
    u64 coef;
    if (((x >> (w - 3)) & 3) == 3) {
        d->exp = (int)((x >> (t - 2)) & ((1u << ew) - 1)) - fmts[f].bias;
        coef = ((u64)4 << (t - 2)) | (x & (((u64)1 << (t - 2)) - 1));
    } else {
        d->exp = (int)((x >> t) & ((1u << ew) - 1)) - fmts[f].bias;
        coef = x & (((u64)1 << t) - 1);
    }
    u64 lim = f == F32 ? 9999999u : 9999999999999999ULL;
    if (coef > lim)
        coef = 0;                    /* non-canonical */
    b_set(&d->c, coef);
}

static struct bits pack_special(int f, int neg, int kind)
{
    struct bits r = { 0, 0 };
    if (f == F128) {
        r.hi = (u64)neg << 63 | (kind == K_INF ? (u64)0x1e << 58 : (u64)0x1f << 58) |
               (kind == K_SNAN ? (u64)1 << 57 : 0);
        return r;
    }
    int w = f == F32 ? 32 : 64;
    r.lo = (u64)neg << (w - 1) | (kind == K_INF ? (u64)0x1e << (w - 6) : (u64)0x1f << (w - 6)) |
           (kind == K_SNAN ? (u64)1 << (w - 7) : 0);
    return r;
}

/* a finite value whose coefficient has at most p digits and exponent is
 * in range */
static struct bits pack_finite(int f, int neg, int exp, const struct big *c)
{
    struct bits r = { 0, 0 };
    u64 be = (u64)(exp + fmts[f].bias);
    if (f == F128) {
        r.lo = c->n > 0 ? c->l[0] : 0;
        r.hi = (u64)neg << 63 | be << 49 | (c->n > 1 ? c->l[1] : 0);
        return r;
    }
    int w = f == F32 ? 32 : 64, t = f == F32 ? 23 : 53;
    u64 coef = c->n ? c->l[0] : 0;
    if (coef >> t)                   /* the large-coefficient form */
        r.lo = (u64)neg << (w - 1) | (u64)3 << (w - 3) | be << (t - 2) |
               (coef & (((u64)1 << (t - 2)) - 1));
    else
        r.lo = (u64)neg << (w - 1) | be << t | coef;
    return r;
}

/* The exact value neg * c * 10^exp, with sticky saying more nonzero digits
 * lay below it, rounded half-even to the format and packed. */
static struct bits round_pack(int f, int neg, int exp, struct big c, int sticky)
{
    const struct fmt *F = &fmts[f];
    int round = 0;                   /* the last digit dropped */
    int digits = b_digits(&c);
    /* too many digits, or below the smallest exponent: drop digits */
    int drop = digits - F->p;
    if (exp + drop < F->qmin)
        drop = F->qmin - exp;
    if (drop > 0) {
        if (drop > digits + 1) {      /* everything goes */
            sticky |= !b_zero(&c);
            c.n = 0;
            round = 0;
        } else {
            for (int i = 0; i < drop; i++) {
                if (round)
                    sticky = 1;
                round = (int)b_divsmall(&c, 10);
            }
        }
        exp += drop;
        /* half to even */
        int up = round > 5 || (round == 5 && (sticky || (c.n && (c.l[0] & 1))));
        if (up) {
            b_muladd(&c, 1, 1);
            if (b_digits(&c) > F->p) {
                b_divsmall(&c, 10);
                exp++;
            }
        }
    }
    if (b_zero(&c)) {
        if (exp > F->qmax)
            exp = F->qmax;
        if (exp < F->qmin)
            exp = F->qmin;
        return pack_finite(f, neg, exp, &c);
    }
    /* too large an exponent: pad the coefficient with zeros if it fits */
    while (exp > F->qmax && b_digits(&c) < F->p) {
        b_mul10k(&c, 1);
        exp--;
    }
    if (exp > F->qmax)
        return pack_special(f, neg, K_INF);
    return pack_finite(f, neg, exp, &c);
}

static struct bits qnan(int f) { return pack_special(f, 0, K_QNAN); }

/* ---- the carriers --------------------------------------------------------- */

static struct bits from32(float v)
{
    union { float f; bid32 u; } x;
    x.f = v;
    struct bits b = { x.u, 0 };
    return b;
}

static float to32(struct bits b)
{
    union { float f; bid32 u; } x;
    x.u = (bid32)b.lo;
    return x.f;
}

static struct bits from64(double v)
{
    union { double d; u64 u; } x;
    x.d = v;
    struct bits b = { x.u, 0 };
    return b;
}

static double to64(struct bits b)
{
    union { double d; u64 u; } x;
    x.u = b.lo;
    return x.d;
}

static struct bits from128(td_carrier v)
{
    union { td_carrier t; u64 h[2]; } x;
    x.t = v;
    struct bits b = { x.h[0], x.h[1] };
    return b;
}

static td_carrier to128(struct bits b)
{
    union { td_carrier t; u64 h[2]; } x;
    x.h[0] = b.lo;
    x.h[1] = b.hi;
    return x.t;
}

/* ---- arithmetic ------------------------------------------------------------- */

static struct bits d_add(int f, struct bits x, struct bits y, int negate_y)
{
    struct dec a, b;
    unpack(f, x, &a);
    unpack(f, y, &b);
    if (negate_y)
        b.neg ^= 1;
    if (a.kind >= K_QNAN || b.kind >= K_QNAN)
        return qnan(f);
    if (a.kind == K_INF || b.kind == K_INF) {
        if (a.kind == K_INF && b.kind == K_INF && a.neg != b.neg)
            return qnan(f);
        return pack_special(f, a.kind == K_INF ? a.neg : b.neg, K_INF);
    }
    /* align to the smaller exponent; a much smaller operand only rounds */
    int e = a.exp < b.exp ? a.exp : b.exp;
    struct dec *hi = a.exp >= b.exp ? &a : &b, *lo = hi == &a ? &b : &a;
    int diff = hi->exp - lo->exp;
    int sticky = 0;
    if (diff > fmts[f].p + 3 && !b_zero(&hi->c)) {
        /* lo is below the last digit of any rounding: an epsilon */
        sticky = !b_zero(&lo->c);
        int k = fmts[f].p + 3;
        b_mul10k(&hi->c, k);
        e = hi->exp - k;
        if (sticky) {
            /* one unit far below, with the right sign */
            struct big one;
            b_set(&one, 1);
            if (lo->neg == hi->neg) {
                b_mul10k(&hi->c, 1);
                b_add(&hi->c, &one);
            } else {
                b_mul10k(&hi->c, 1);
                b_sub(&hi->c, &one);
            }
            e--;
            sticky = 0;
        }
        return round_pack(f, hi->neg, e, hi->c, sticky);
    }
    b_mul10k(&hi->c, diff);
    hi->exp = e;
    if (hi->neg == lo->neg) {
        b_add(&hi->c, &lo->c);
        return round_pack(f, hi->neg, e, hi->c, 0);
    }
    int c = b_cmp(&hi->c, &lo->c);
    if (c == 0) {
        struct big z;
        z.n = 0;
        return round_pack(f, 0, e, z, 0);     /* x - x is +0 */
    }
    if (c > 0) {
        b_sub(&hi->c, &lo->c);
        return round_pack(f, hi->neg, e, hi->c, 0);
    }
    b_sub(&lo->c, &hi->c);
    return round_pack(f, lo->neg, e, lo->c, 0);
}

static struct bits d_mul(int f, struct bits x, struct bits y)
{
    struct dec a, b;
    unpack(f, x, &a);
    unpack(f, y, &b);
    int neg = a.neg ^ b.neg;
    if (a.kind >= K_QNAN || b.kind >= K_QNAN)
        return qnan(f);
    if (a.kind == K_INF || b.kind == K_INF) {
        if ((a.kind == K_FIN && b_zero(&a.c)) || (b.kind == K_FIN && b_zero(&b.c)))
            return qnan(f);
        return pack_special(f, neg, K_INF);
    }
    struct big c;
    b_mul(&c, &a.c, &b.c);
    return round_pack(f, neg, a.exp + b.exp, c, 0);
}

static struct bits d_div(int f, struct bits x, struct bits y)
{
    struct dec a, b;
    unpack(f, x, &a);
    unpack(f, y, &b);
    int neg = a.neg ^ b.neg;
    if (a.kind >= K_QNAN || b.kind >= K_QNAN)
        return qnan(f);
    if (a.kind == K_INF)
        return b.kind == K_INF ? qnan(f) : pack_special(f, neg, K_INF);
    if (b.kind == K_INF) {
        struct big z;
        z.n = 0;
        return round_pack(f, neg, fmts[f].qmin, z, 0);
    }
    if (b_zero(&b.c))
        return b_zero(&a.c) ? qnan(f) : pack_special(f, neg, K_INF);
    int pref = a.exp - b.exp;
    if (b_zero(&a.c)) {
        struct big z;
        z.n = 0;
        return round_pack(f, neg, pref, z, 0);
    }
    /* enough digits that the quotient has p + 1 of them */
    int k = fmts[f].p + 1 + b_digits(&b.c) - b_digits(&a.c);
    if (k < 0)
        k = 0;
    struct big n = a.c, q, r;
    b_mul10k(&n, k);
    b_divmod(&q, &r, &n, &b.c);
    int e = pref - k;
    if (b_zero(&r)) {
        /* exact: as few digits as the preferred exponent allows */
        while (e < pref && !b_zero(&q)) {
            struct big t = q;
            if (b_divsmall(&t, 10))
                break;
            q = t;
            e++;
        }
        return round_pack(f, neg, e, q, 0);
    }
    return round_pack(f, neg, e, q, 1);
}

/* -1, 0, 1, or 2 unordered */
static int d_cmp(int f, struct bits x, struct bits y)
{
    struct dec a, b;
    unpack(f, x, &a);
    unpack(f, y, &b);
    if (a.kind >= K_QNAN || b.kind >= K_QNAN)
        return 2;
    int az = a.kind == K_FIN && b_zero(&a.c), bz = b.kind == K_FIN && b_zero(&b.c);
    if (az && bz)
        return 0;
    int sa = az ? 0 : a.neg ? -1 : 1, sb = bz ? 0 : b.neg ? -1 : 1;
    if (sa != sb)
        return sa < sb ? -1 : 1;
    /* the same sign, both nonzero: by magnitude */
    int m;
    if (a.kind == K_INF || b.kind == K_INF) {
        m = a.kind == b.kind ? 0 : a.kind == K_INF ? 1 : -1;
    } else {
        /* scale to the smaller exponent, bounded: past 6200 digits apart the
         * larger exponent wins outright for nonzero coefficients */
        int da = b_digits(&a.c) + a.exp, db = b_digits(&b.c) + b.exp;
        if (da != db) {
            m = da < db ? -1 : 1;
        } else {
            if (a.exp > b.exp)
                b_mul10k(&a.c, a.exp - b.exp);
            else
                b_mul10k(&b.c, b.exp - a.exp);
            m = b_cmp(&a.c, &b.c);
        }
    }
    return sa < 0 ? -m : m;
}

/* ---- integers --------------------------------------------------------------- */

static struct bits d_from_int(int f, u64 v, int neg)
{
    struct big c;
    b_set(&c, v);
    return round_pack(f, neg, 0, c, 0);
}

/* toward zero; *ok 0 for a NaN or infinity */
static int d_to_int(int f, struct bits x, struct big *mag, int *neg)
{
    struct dec a;
    unpack(f, x, &a);
    *neg = a.neg;
    if (a.kind != K_FIN) {
        mag->n = 0;
        return 0;
    }
    *mag = a.c;
    if (a.exp >= 0) {
        if (a.exp > 40) {
            mag->n = 0;              /* far out of range: undefined */
            return 0;
        }
        b_mul10k(mag, a.exp);
    } else {
        for (int i = 0; i < -a.exp && !b_zero(mag); i++)
            b_divsmall(mag, 10);
    }
    return 1;
}

static s64 d_to_s64(int f, struct bits x)
{
    struct big m;
    int neg;
    if (!d_to_int(f, x, &m, &neg))
        return 0;
    u64 v = m.n ? m.l[0] : 0;
    return neg ? (s64)(0 - v) : (s64)v;
}

static u64 d_to_u64(int f, struct bits x)
{
    struct big m;
    int neg;
    if (!d_to_int(f, x, &m, &neg))
        return 0;
    return m.n ? m.l[0] : 0;
}

/* ---- binary floating point ---------------------------------------------- */

/* from a double: m * 2^q exactly, as a decimal m * 5^-q * 10^q */
static struct bits d_from_double(int f, double v)
{
    union { double d; u64 u; } x;
    x.d = v;
    int neg = (int)(x.u >> 63);
    int be = (int)((x.u >> 52) & 0x7ff);
    u64 m = x.u & 0xfffffffffffffULL;
    if (be == 0x7ff)
        return m ? qnan(f) : pack_special(f, neg, K_INF);
    if (be)
        m |= (u64)1 << 52;
    else
        be = 1;
    int q = be - 1075;
    struct big c;
    b_set(&c, m);
    if (!m)
        return round_pack(f, neg, 0, c, 0);
    if (q >= 0) {
        for (int i = 0; i < q; i++)
            b_muladd(&c, 2, 0);
        return round_pack(f, neg, 0, c, 0);
    }
    for (int i = 0; i < -q; i++)
        b_muladd(&c, 5, 0);
    return round_pack(f, neg, q, c, 0);
}

/* to a double, rounded once: the coefficient scaled to at least 64
 * significant bits and divided by 10^-exp with a sticky remainder */
static double d_to_double(int f, struct bits x)
{
    struct dec a;
    unpack(f, x, &a);
    if (a.kind >= K_QNAN)
        return __builtin_nan("");
    if (a.kind == K_INF)
        return a.neg ? -__builtin_inf() : __builtin_inf();
    if (b_zero(&a.c))
        return a.neg ? -0.0 : 0.0;
    /* far outside a double's range: there is no rounding to do, and the
     * scaled value would not fit the big integer */
    int mag = b_digits(&a.c) + a.exp;            /* value < 10^mag */
    if (mag > 310)
        return a.neg ? -__builtin_inf() : __builtin_inf();
    if (mag < -326)
        return a.neg ? -0.0 : 0.0;
    struct big n = a.c;
    int shift = 0;                   /* the value is n * 2^-shift */
    if (a.exp >= 0) {
        b_mul10k(&n, a.exp);
    } else {
        struct big d;
        b_set(&d, 1);
        b_mul10k(&d, -a.exp);
        /* n << s with enough bits that the quotient has 66 */
        int s = b_bits(&d) - b_bits(&n) + 66;
        if (s < 0)
            s = 0;
        for (int i = 0; i < s; i++)
            b_muladd(&n, 2, 0);
        struct big q, r;
        b_divmod(&q, &r, &n, &d);
        n = q;
        if (!b_zero(&r))
            n.l[0] |= 1;             /* sticky */
        shift = s;
    }
    /* n's top 64 bits with a sticky bit, converted once, then scaled */
    int nb = b_bits(&n);
    u64 top;
    int sh = nb > 64 ? nb - 64 : 0;
    if (sh) {
        top = 0;
        for (int i = 0; i < 64; i++)
            top |= (u64)b_bit(&n, sh + i) << i;
        int st = 0;
        for (int i = 0; i < sh && !st; i++)
            st = b_bit(&n, i);
        top |= (u64)st;
    } else {
        top = n.n ? n.l[0] : 0;
    }
    /* the binary exponent may be out of a double's range: scale by halves */
    double r = (double)top;
    int e2 = sh - shift;
    while (e2 > 0) {
        int k = e2 > 1000 ? 1000 : e2;
        double p = 1.0;
        for (int i = 0; i < k; i++)
            p *= 2.0;
        r *= p;
        e2 -= k;
    }
    while (e2 < 0) {
        int k = -e2 > 1000 ? 1000 : -e2;
        double p = 1.0;
        for (int i = 0; i < k; i++)
            p *= 2.0;
        r /= p;
        e2 += k;
    }
    return a.neg ? -r : r;
}

/* between decimal formats: exact when widening, rounded once narrowing */
static struct bits d_convert(int to, int from, struct bits x)
{
    struct dec a;
    unpack(from, x, &a);
    if (a.kind != K_FIN)
        return pack_special(to, a.neg, a.kind == K_INF ? K_INF : K_QNAN);
    return round_pack(to, a.neg, a.exp, a.c, 0);
}

/* ---- the libgcc names --------------------------------------------------- */

#define ARITH(sfx, F, T, from, to)                                                    \
    T __bid_add##sfx##3(T a, T b) { return to(d_add(F, from(a), from(b), 0)); }       \
    T __bid_sub##sfx##3(T a, T b) { return to(d_add(F, from(a), from(b), 1)); }       \
    T __bid_mul##sfx##3(T a, T b) { return to(d_mul(F, from(a), from(b))); }          \
    T __bid_div##sfx##3(T a, T b) { return to(d_div(F, from(a), from(b))); }          \
    int __bid_eq##sfx##2(T a, T b) { return d_cmp(F, from(a), from(b)) != 0; }       \
    int __bid_ne##sfx##2(T a, T b) { return d_cmp(F, from(a), from(b)) != 0; }       \
    int __bid_lt##sfx##2(T a, T b) { int c = d_cmp(F, from(a), from(b)); return c == 2 ? 1 : c; } \
    int __bid_le##sfx##2(T a, T b) { int c = d_cmp(F, from(a), from(b)); return c == 2 ? 1 : c; } \
    int __bid_gt##sfx##2(T a, T b) { int c = d_cmp(F, from(a), from(b)); return c == 2 ? -1 : c; } \
    int __bid_ge##sfx##2(T a, T b) { int c = d_cmp(F, from(a), from(b)); return c == 2 ? -1 : c; } \
    int __bid_unord##sfx##2(T a, T b) { return d_cmp(F, from(a), from(b)) == 2; }     \
    T __bid_floatsi##sfx(int v) { return to(d_from_int(F, v < 0 ? 0 - (u64)(s64)v : (u64)v, v < 0)); } \
    T __bid_floatdi##sfx(s64 v) { return to(d_from_int(F, v < 0 ? 0 - (u64)v : (u64)v, v < 0)); } \
    T __bid_floatunssi##sfx(unsigned v) { return to(d_from_int(F, v, 0)); }             \
    T __bid_floatunsdi##sfx(u64 v) { return to(d_from_int(F, v, 0)); }                  \
    int __bid_fix##sfx##si(T a) { return (int)d_to_s64(F, from(a)); }                   \
    s64 __bid_fix##sfx##di(T a) { return d_to_s64(F, from(a)); }                        \
    unsigned __bid_fixuns##sfx##si(T a) { return (unsigned)d_to_u64(F, from(a)); }      \
    u64 __bid_fixuns##sfx##di(T a) { return d_to_u64(F, from(a)); }

ARITH(sd, F32, float, from32, to32)
ARITH(dd, F64, double, from64, to64)
ARITH(td, F128, td_carrier, from128, to128)

/* -x: the sign flipped, which IEEE negation is (for every value, NaN too) */
static struct bits flip(int f, struct bits b)
{
    if (f == F128)
        b.hi ^= (u64)1 << 63;
    else
        b.lo ^= (u64)1 << (f == F32 ? 31 : 63);
    return b;
}

float __embcc_bid_negsd(float a) { return to32(flip(F32, from32(a))); }
double __embcc_bid_negdd(double a) { return to64(flip(F64, from64(a))); }
td_carrier __embcc_bid_negtd(td_carrier a) { return to128(flip(F128, from128(a))); }

double __bid_extendsddf(float a) { return d_to_double(F32, from32(a)); }
float __bid_truncsdsf(float a) { return (float)d_to_double(F32, from32(a)); }
double __bid_truncdddf(double a) { return d_to_double(F64, from64(a)); }
float __bid_truncddsf(double a) { return (float)d_to_double(F64, from64(a)); }
double __bid_trunctddf(td_carrier a) { return d_to_double(F128, from128(a)); }
float __bid_trunctdsf(td_carrier a) { return (float)d_to_double(F128, from128(a)); }

float __bid_extendsfsd(float v) { return to32(d_from_double(F32, (double)v)); }
double __bid_extendsfdd(float v) { return to64(d_from_double(F64, (double)v)); }
td_carrier __bid_extendsftd(float v) { return to128(d_from_double(F128, (double)v)); }
float __bid_truncdfsd(double v) { return to32(d_from_double(F32, v)); }
double __bid_extenddfdd(double v) { return to64(d_from_double(F64, v)); }
td_carrier __bid_extenddftd(double v) { return to128(d_from_double(F128, v)); }

double __bid_extendsddd2(float a) { return to64(d_convert(F64, F32, from32(a))); }
td_carrier __bid_extendsdtd2(float a) { return to128(d_convert(F128, F32, from32(a))); }
td_carrier __bid_extendddtd2(double a) { return to128(d_convert(F128, F64, from64(a))); }
float __bid_truncddsd2(double a) { return to32(d_convert(F32, F64, from64(a))); }
float __bid_trunctdsd2(td_carrier a) { return to32(d_convert(F32, F128, from128(a))); }
double __bid_trunctddd2(td_carrier a) { return to64(d_convert(F64, F128, from128(a))); }

#endif
