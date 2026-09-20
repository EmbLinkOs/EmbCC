/* Compile-time long double arithmetic — see ldfloat.h for the contract.
 *
 * The integer side is a minimal arbitrary-precision unsigned integer:
 * little-endian 32-bit limbs, just the operations exact rounding needs.
 * Sizes stay modest — the widest value a literal can produce is about
 * 10^4951 (LDBL_MIN's digits), some 16 500 bits — so schoolbook
 * algorithms, and a bit-at-a-time long division that only ever has to
 * produce ~120 quotient bits, are plenty fast.
 */
#include "ldfloat.h"

#include <string.h>

#include "../driver/util.h"
#include "../arch/target.h"

/* ---- arbitrary-precision unsigned integers ---- */

struct big { unsigned *d; int n; };   /* n limbs in use, no leading zeros */

static struct big big_new(int cap)
{
    struct big b;
    b.d = xcalloc((size_t)(cap > 0 ? cap : 1), sizeof *b.d);
    b.n = 0;
    return b;
}

static void big_trim(struct big *b)
{
    while (b->n > 0 && b->d[b->n - 1] == 0)
        b->n--;
}

static struct big big_from_u64(unsigned long v)
{
    struct big b = big_new(2);
    b.d[0] = (unsigned)(v & 0xffffffffUL);
    b.d[1] = (unsigned)(v >> 32);
    b.n = 2;
    big_trim(&b);
    return b;
}

static struct big big_copy(struct big a)
{
    struct big b = big_new(a.n);
    if (a.n) memcpy(b.d, a.d, (size_t)a.n * sizeof *a.d);
    b.n = a.n;
    return b;
}

static int big_is_zero(struct big a) { return a.n == 0; }

static long big_bitlen(struct big a)
{
    if (a.n == 0) return 0;
    unsigned top = a.d[a.n - 1];
    int k = 0;
    while (top) { k++; top >>= 1; }
    return (long)(a.n - 1) * 32 + k;
}

static int big_bit(struct big a, long i)
{
    long w = i / 32;
    if (i < 0 || w >= a.n) return 0;
    return (int)((a.d[w] >> (i % 32)) & 1);
}

static int big_cmp(struct big a, struct big b)
{
    if (a.n != b.n) return a.n < b.n ? -1 : 1;
    for (int i = a.n - 1; i >= 0; i--)
        if (a.d[i] != b.d[i]) return a.d[i] < b.d[i] ? -1 : 1;
    return 0;
}

static struct big big_shl(struct big a, long k)
{
    if (a.n == 0 || k == 0) return big_copy(a);
    int w = (int)(k / 32), s = (int)(k % 32);
    struct big b = big_new(a.n + w + 1);
    for (int i = 0; i < a.n; i++) {
        unsigned long v = (unsigned long)a.d[i] << s;
        b.d[i + w] |= (unsigned)(v & 0xffffffffUL);
        b.d[i + w + 1] |= (unsigned)(v >> 32);
    }
    b.n = a.n + w + 1;
    big_trim(&b);
    return b;
}

/* a >> k; *sticky |= 1 if any bit shifted out was set */
static struct big big_shr(struct big a, long k, int *sticky)
{
    if (k <= 0) return big_copy(a);
    for (long i = 0; i < k && i < (long)a.n * 32; i++)
        if (big_bit(a, i)) { *sticky = 1; break; }
    int w = (int)(k / 32), s = (int)(k % 32);
    if (w >= a.n) return big_new(1);
    struct big b = big_new(a.n - w);
    for (int i = 0; i < a.n - w; i++) {
        unsigned long v = a.d[i + w] >> s;
        if (s && i + w + 1 < a.n)
            v |= ((unsigned long)a.d[i + w + 1] << (32 - s)) & 0xffffffffUL;
        b.d[i] = (unsigned)v;
    }
    b.n = a.n - w;
    big_trim(&b);
    return b;
}

static struct big big_add(struct big a, struct big b)
{
    int n = a.n > b.n ? a.n : b.n;
    struct big r = big_new(n + 1);
    unsigned long carry = 0;
    for (int i = 0; i < n; i++) {
        unsigned long s = carry;
        if (i < a.n) s += a.d[i];
        if (i < b.n) s += b.d[i];
        r.d[i] = (unsigned)(s & 0xffffffffUL);
        carry = s >> 32;
    }
    r.d[n] = (unsigned)carry;
    r.n = n + 1;
    big_trim(&r);
    return r;
}

/* a - b, requires a >= b */
static struct big big_sub(struct big a, struct big b)
{
    struct big r = big_new(a.n);
    long borrow = 0;
    for (int i = 0; i < a.n; i++) {
        long s = (long)a.d[i] - borrow - (i < b.n ? (long)b.d[i] : 0);
        borrow = s < 0;
        if (s < 0) s += 0x100000000L;
        r.d[i] = (unsigned)s;
    }
    r.n = a.n;
    big_trim(&r);
    return r;
}

static struct big big_mul(struct big a, struct big b)
{
    if (a.n == 0 || b.n == 0) return big_new(1);
    struct big r = big_new(a.n + b.n);
    for (int i = 0; i < a.n; i++) {
        unsigned long carry = 0;
        for (int j = 0; j < b.n; j++) {
            unsigned long t = (unsigned long)a.d[i] * b.d[j] + r.d[i + j] + carry;
            r.d[i + j] = (unsigned)(t & 0xffffffffUL);
            carry = t >> 32;
        }
        r.d[i + b.n] = (unsigned)carry;
    }
    r.n = a.n + b.n;
    big_trim(&r);
    return r;
}

static struct big big_mul_small_add(struct big a, unsigned m, unsigned add)
{
    struct big r = big_new(a.n + 1);
    unsigned long carry = add;
    for (int i = 0; i < a.n; i++) {
        unsigned long t = (unsigned long)a.d[i] * m + carry;
        r.d[i] = (unsigned)(t & 0xffffffffUL);
        carry = t >> 32;
    }
    r.d[a.n] = (unsigned)carry;
    r.n = a.n + 1;
    big_trim(&r);
    return r;
}

static struct big big_pow10(long k)
{
    struct big r = big_from_u64(1);
    while (k >= 9) { r = big_mul_small_add(r, 1000000000U, 0); k -= 9; }
    while (k-- > 0) r = big_mul_small_add(r, 10, 0);
    return r;
}

/* num / den (den != 0) by shift-and-subtract; *rem_nonzero set if the
 * division is inexact. Cost is proportional to the quotient's bit length,
 * which callers keep near the target precision. */
static struct big big_div(struct big num, struct big den, int *rem_nonzero)
{
    long s = big_bitlen(num) - big_bitlen(den);
    struct big q = big_new((int)(s / 32) + 2);
    q.n = (int)(s / 32) + 2;
    struct big r = big_copy(num);
    for (long i = s; i >= 0; i--) {
        struct big t = big_shl(den, i);
        if (big_cmp(r, t) >= 0) {
            r = big_sub(r, t);
            q.d[i / 32] |= 1U << (i % 32);
        }
    }
    big_trim(&q);
    *rem_nonzero = !big_is_zero(r);
    return q;
}

/* ---- the values ---- */

enum { LDF_FINITE, LDF_INF, LDF_NAN };

struct ldf {
    int kind;
    int neg;
    struct big m;       /* finite: value = (neg ? -1 : 1) * m * 2^e */
    long e;
};

struct fmtinfo { int p; long emin, emax; int bias; };

static struct fmtinfo finfo(enum ldf_fmt f)
{
    struct fmtinfo i;
    switch (f) {
    case LDF_X87:    i.p = 64;  i.emin = -16382; i.emax = 16383; i.bias = 16383; break;
    case LDF_QUAD:   i.p = 113; i.emin = -16382; i.emax = 16383; i.bias = 16383; break;
    case LDF_DOUBLE: i.p = 53;  i.emin = -1022;  i.emax = 1023;  i.bias = 1023;  break;
    default:         i.p = 24;  i.emin = -126;   i.emax = 127;   i.bias = 127;   break;
    }
    return i;
}

enum ldf_fmt ldf_target_fmt(void)
{
    return target_get() == TARGET_AARCH64 ? LDF_QUAD : LDF_X87;
}

static struct ldf *mk(int kind, int neg)
{
    struct ldf *v = xcalloc(1, sizeof *v);
    v->kind = kind;
    v->neg = neg;
    v->m = big_new(1);
    return v;
}

/* m * 2^e (with `sticky`: the exact value is a little above it) rounded to
 * nearest-even in fmt, including its subnormal range and overflow to inf. */
static struct ldf *round_to(struct big m, long e, int sticky, int neg,
                            enum ldf_fmt fmt)
{
    struct fmtinfo fi = finfo(fmt);
    struct ldf *v = mk(LDF_FINITE, neg);
    if (big_is_zero(m)) return v;
    long L = big_bitlen(m);
    long qe = e + L - fi.p;                 /* keep p bits ... */
    long lowq = fi.emin - (fi.p - 1);       /* ... but no finer than subnormal */
    if (qe < lowq) qe = lowq;
    long shift = qe - e;
    struct big mm;
    long e2;
    if (shift > 0) {
        int rest = sticky;
        struct big t = big_shr(m, shift - 1, &rest);   /* keeps the half bit */
        int half = big_bit(t, 0);
        int dummy = 0;
        mm = big_shr(t, 1, &dummy);
        if (half && (rest || big_bit(mm, 0)))
            mm = big_add(mm, big_from_u64(1));
        e2 = qe;
        if (big_bitlen(mm) > fi.p) {        /* rounded up to 2^p */
            mm = big_shr(mm, 1, &dummy);
            e2++;
        }
    } else {
        mm = big_copy(m);
        e2 = e;
    }
    if (!big_is_zero(mm) && e2 + big_bitlen(mm) - 1 > fi.emax)
        return mk(LDF_INF, neg);
    v->m = mm;
    v->e = e2;
    return v;
}

struct ldf *ldf_round(const struct ldf *a, enum ldf_fmt fmt)
{
    if (a->kind != LDF_FINITE) return mk(a->kind, a->neg);
    return round_to(a->m, a->e, 0, a->neg, fmt);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct ldf *ldf_from_text(const char *s, enum ldf_fmt fmt)
{
    struct big m = big_new(1);
    long e = 0, exp10 = 0;
    int any = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        int frac = 0;
        for (;; s++) {
            if (*s == '.' && !frac) { frac = 1; continue; }
            int h = hexval(*s);
            if (h < 0) break;
            m = big_mul_small_add(m, 16, (unsigned)h);
            if (frac) e -= 4;
            any = 1;
        }
        if (!any || (*s != 'p' && *s != 'P')) return NULL;
        s++;
        int eneg = 0;
        if (*s == '+' || *s == '-') eneg = *s++ == '-';
        long x = 0;
        if (*s < '0' || *s > '9') return NULL;
        while (*s >= '0' && *s <= '9') {
            if (x < 100000000L) x = x * 10 + (*s - '0');
            s++;
        }
        if (*s) return NULL;
        e += eneg ? -x : x;
        return round_to(m, e, 0, 0, fmt);
    }
    int frac = 0;
    for (;; s++) {
        if (*s == '.' && !frac) { frac = 1; continue; }
        if (*s < '0' || *s > '9') break;
        m = big_mul_small_add(m, 10, (unsigned)(*s - '0'));
        if (frac) exp10--;
        any = 1;
    }
    if (!any) return NULL;
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '+' || *s == '-') eneg = *s++ == '-';
        long x = 0;
        if (*s < '0' || *s > '9') return NULL;
        while (*s >= '0' && *s <= '9') {
            if (x < 100000000L) x = x * 10 + (*s - '0');
            s++;
        }
        exp10 += eneg ? -x : x;
    }
    if (*s) return NULL;
    if (big_is_zero(m)) return mk(LDF_FINITE, 0);
    /* Far outside any format: settle it without a gigantic power of ten. */
    if (exp10 > 6000) return mk(LDF_INF, 0);
    if (exp10 + (long)(big_bitlen(m) * 3 / 10) < -6000)
        return mk(LDF_FINITE, 0);
    if (exp10 >= 0)
        return round_to(big_mul(m, big_pow10(exp10)), 0, 0, 0, fmt);
    /* m / 10^k, with enough quotient bits for the rounding to be exact */
    struct big den = big_pow10(-exp10);
    struct fmtinfo fi = finfo(fmt);
    long k = big_bitlen(den) - big_bitlen(m) + fi.p + 3;
    if (k < 0) k = 0;
    int inexact = 0;
    struct big q = big_div(big_shl(m, k), den, &inexact);
    return round_to(q, -k, inexact, 0, fmt);
}

struct ldf *ldf_from_double(double d)
{
    unsigned long bits;
    memcpy(&bits, &d, sizeof bits);
    int neg = (int)(bits >> 63);
    long ex = (long)((bits >> 52) & 0x7ff);
    unsigned long frac = bits & 0xfffffffffffffUL;
    if (ex == 0x7ff) return mk(frac ? LDF_NAN : LDF_INF, neg);
    struct ldf *v = mk(LDF_FINITE, neg);
    if (ex == 0) {
        v->m = big_from_u64(frac);
        v->e = -1074;
    } else {
        v->m = big_from_u64(frac | (1UL << 52));
        v->e = ex - 1075;
    }
    return v;
}

struct ldf *ldf_from_int(long v, int is_unsigned)
{
    int neg = !is_unsigned && v < 0;
    unsigned long mag = neg ? 0UL - (unsigned long)v : (unsigned long)v;
    struct ldf *r = mk(LDF_FINITE, neg);
    r->m = big_from_u64(mag);
    return r;
}

struct ldf *ldf_neg(const struct ldf *a)
{
    struct ldf *r = mk(a->kind, !a->neg);
    r->m = big_copy(a->m);
    r->e = a->e;
    return r;
}

int ldf_is_zero(const struct ldf *a)
{
    return a->kind == LDF_FINITE && big_is_zero(a->m);
}

int ldf_cmp(const struct ldf *a, const struct ldf *b)
{
    if (a->kind == LDF_NAN || b->kind == LDF_NAN)
        return LDF_UNORDERED;

    /* +0 == -0, which is the one place IEEE says two different bit
     * patterns compare equal -- and the reason this cannot just compare
     * sign first and be done. */
    int az = ldf_is_zero(a), bz = ldf_is_zero(b);
    if (az && bz)
        return 0;
    if (az)
        return b->neg ? 1 : -1;
    if (bz)
        return a->neg ? -1 : 1;

    if (a->neg != b->neg)
        return a->neg ? -1 : 1;
    int sign = a->neg ? -1 : 1;          /* both the same sign */

    if (a->kind == LDF_INF || b->kind == LDF_INF) {
        if (a->kind == LDF_INF && b->kind == LDF_INF)
            return 0;
        return a->kind == LDF_INF ? sign : -sign;
    }

    /* Both finite, same sign: compare the significands at a common
     * exponent. Shifting rather than normalising keeps it exact. */
    long e = a->e < b->e ? a->e : b->e;
    struct big ma = big_shl(a->m, a->e - e), mb = big_shl(b->m, b->e - e);
    return sign * big_cmp(ma, mb);
}

struct ldf *ldf_binop(int op, const struct ldf *a, const struct ldf *b,
                      enum ldf_fmt fmt)
{
    if (a->kind == LDF_NAN || b->kind == LDF_NAN) return mk(LDF_NAN, 0);
    struct fmtinfo fi = finfo(fmt);
    int bneg = b->neg;
    if (op == '-') { bneg = !bneg; op = '+'; }
    if (op == '+') {
        if (a->kind == LDF_INF || b->kind == LDF_INF) {
            if (a->kind == LDF_INF && b->kind == LDF_INF && a->neg != bneg)
                return mk(LDF_NAN, 0);
            return mk(LDF_INF, a->kind == LDF_INF ? a->neg : bneg);
        }
        if (big_is_zero(a->m) && big_is_zero(b->m))
            return mk(LDF_FINITE, a->neg && bneg);   /* -0 + -0 = -0 */
        if (big_is_zero(a->m)) return round_to(b->m, b->e, 0, bneg, fmt);
        if (big_is_zero(b->m)) return round_to(a->m, a->e, 0, a->neg, fmt);
        long e = a->e < b->e ? a->e : b->e;
        struct big ma = big_shl(a->m, a->e - e), mb = big_shl(b->m, b->e - e);
        if (a->neg == bneg)
            return round_to(big_add(ma, mb), e, 0, a->neg, fmt);
        int c = big_cmp(ma, mb);
        if (c == 0) return mk(LDF_FINITE, 0);
        return c > 0 ? round_to(big_sub(ma, mb), e, 0, a->neg, fmt)
                     : round_to(big_sub(mb, ma), e, 0, bneg, fmt);
    }
    int neg = a->neg != b->neg;
    if (op == '*') {
        if (a->kind == LDF_INF || b->kind == LDF_INF) {
            if ((a->kind == LDF_FINITE && big_is_zero(a->m)) ||
                (b->kind == LDF_FINITE && big_is_zero(b->m)))
                return mk(LDF_NAN, 0);
            return mk(LDF_INF, neg);
        }
        return round_to(big_mul(a->m, b->m), a->e + b->e, 0, neg, fmt);
    }
    /* '/' */
    if (a->kind == LDF_INF)
        return b->kind == LDF_INF ? mk(LDF_NAN, 0) : mk(LDF_INF, neg);
    if (b->kind == LDF_INF) return mk(LDF_FINITE, neg);
    if (big_is_zero(b->m))
        return big_is_zero(a->m) ? mk(LDF_NAN, 0) : mk(LDF_INF, neg);
    if (big_is_zero(a->m)) return mk(LDF_FINITE, neg);
    long k = big_bitlen(b->m) - big_bitlen(a->m) + fi.p + 3;
    if (k < 0) k = 0;
    int inexact = 0;
    struct big q = big_div(big_shl(a->m, k), b->m, &inexact);
    return round_to(q, a->e - b->e - k, inexact, neg, fmt);
}

/* low 64 bits of (m >> k) */
static unsigned long low64(struct big m, long k)
{
    int dummy = 0;
    struct big t = big_shr(m, k, &dummy);
    unsigned long v = 0;
    if (t.n > 0) v = t.d[0];
    if (t.n > 1) v |= (unsigned long)t.d[1] << 32;
    return v;
}

void ldf_encode(const struct ldf *a, enum ldf_fmt fmt, unsigned char *out)
{
    struct fmtinfo fi = finfo(fmt);
    int nbytes = fmt == LDF_DOUBLE ? 8 : fmt == LDF_FLOAT ? 4 : 16;
    memset(out, 0, (size_t)nbytes);
    unsigned long biased = 0;
    struct big mant = big_new(1);            /* the stored significand field */
    if (a->kind == LDF_INF || a->kind == LDF_NAN) {
        biased = (unsigned long)(2 * fi.bias + 1);
        if (fmt == LDF_X87)   /* explicit integer bit (63), and quiet (62) */
            mant = a->kind == LDF_NAN ? big_shl(big_from_u64(3), 62)
                                      : big_shl(big_from_u64(1), 63);
        else if (a->kind == LDF_NAN)
            mant = big_shl(big_from_u64(1), fi.p - 2);
    } else {
        struct ldf *r = round_to(a->m, a->e, 0, a->neg, fmt);
        if (r->kind == LDF_INF) { ldf_encode(r, fmt, out); return; }
        if (!big_is_zero(r->m)) {
            long L = big_bitlen(r->m);
            long E = r->e + L - 1;
            if (E >= fi.emin) {
                struct big norm = big_shl(r->m, fi.p - L);
                biased = (unsigned long)(E + fi.bias);
                if (fmt == LDF_X87)
                    mant = norm;             /* the integer bit is stored */
                else
                    mant = big_sub(norm, big_shl(big_from_u64(1), fi.p - 1));
            } else {
                /* subnormal: the quantum is fixed at emin - (p-1) */
                mant = big_shl(r->m, r->e - (fi.emin - (fi.p - 1)));
                biased = 0;
            }
        }
    }
    unsigned long sign = (unsigned long)(a->neg != 0);
    unsigned long lo = low64(mant, 0), hi = 0;
    if (fmt == LDF_X87) {
        hi = biased | sign << 15;            /* bytes 8..9 */
    } else if (fmt == LDF_QUAD) {
        hi = low64(mant, 64) | biased << 48 | sign << 63;
    } else if (fmt == LDF_DOUBLE) {
        lo |= biased << 52 | sign << 63;
    } else {
        lo |= biased << 23 | sign << 31;
    }
    for (int i = 0; i < 8 && i < nbytes; i++)
        out[i] = (unsigned char)(lo >> (8 * i));
    if (fmt == LDF_X87) {
        out[8] = (unsigned char)hi;
        out[9] = (unsigned char)(hi >> 8);
    } else if (fmt == LDF_QUAD) {
        for (int i = 0; i < 8; i++)
            out[8 + i] = (unsigned char)(hi >> (8 * i));
    }
}

/* The exact inverse of ldf_encode: a value read back out of memory.
 *
 * Needed because a constant expression may STORE a long double and then
 * read it (`constexpr long double x = 2.5L; static_assert(x > 1);`), and
 * the evaluator has to get back the value it wrote rather than an
 * approximation. Nothing is rounded here -- every bit pattern in a format
 * denotes a value that format can hold exactly, which is why this is the
 * one conversion in the file with no fmtinfo rounding step.
 */
struct ldf *ldf_from_bytes(const unsigned char *in, enum ldf_fmt fmt)
{
    struct fmtinfo fi = finfo(fmt);
    int nbytes = fmt == LDF_DOUBLE ? 8 : fmt == LDF_FLOAT ? 4 : 16;

    unsigned long lo = 0, hi = 0;
    for (int i = 0; i < 8 && i < nbytes; i++)
        lo |= (unsigned long)in[i] << (8 * i);
    if (fmt == LDF_X87) {
        hi = (unsigned long)in[8] | (unsigned long)in[9] << 8;
    } else if (fmt == LDF_QUAD) {
        for (int i = 0; i < 8; i++)
            hi |= (unsigned long)in[8 + i] << (8 * i);
    }

    int neg;
    unsigned long biased;
    struct big mant;
    if (fmt == LDF_X87) {
        neg = (int)(hi >> 15) & 1;
        biased = hi & 0x7fff;
        mant = big_from_u64(lo);          /* the integer bit is IN here */
    } else if (fmt == LDF_QUAD) {
        neg = (int)(hi >> 63) & 1;
        biased = (hi >> 48) & 0x7fff;
        mant = big_add(big_shl(big_from_u64(hi & 0xffffffffffffUL), 64),
                       big_from_u64(lo));
    } else if (fmt == LDF_DOUBLE) {
        neg = (int)(lo >> 63) & 1;
        biased = (lo >> 52) & 0x7ff;
        mant = big_from_u64(lo & 0xfffffffffffffUL);
    } else {
        neg = (int)(lo >> 31) & 1;
        biased = (lo >> 23) & 0xff;
        mant = big_from_u64(lo & 0x7fffffUL);
    }

    if (biased == (unsigned long)(2 * fi.bias + 1)) {
        /* All-ones exponent. x87 stores an explicit integer bit, so the
         * significand there is the low 63 bits; everywhere else it is the
         * whole field. Nonzero means NaN, zero means infinity. */
        struct big payload = fmt == LDF_X87
            ? big_from_u64(lo & 0x7fffffffffffffffUL) : mant;
        return mk(big_is_zero(payload) ? LDF_INF : LDF_NAN, neg);
    }

    struct ldf *v = mk(LDF_FINITE, neg);
    if (biased == 0) {
        /* Zero or subnormal: no implicit bit, and the quantum is fixed. */
        v->m = mant;
        v->e = fi.emin - (fi.p - 1);
        if (big_is_zero(mant))
            v->e = 0;                     /* a clean signed zero */
        return v;
    }
    if (fmt != LDF_X87)
        mant = big_add(mant, big_shl(big_from_u64(1), fi.p - 1));
    v->m = mant;
    v->e = (long)biased - fi.bias - (fi.p - 1);
    return v;
}

double ldf_to_double(const struct ldf *a)
{
    unsigned char b[8];
    ldf_encode(a, LDF_DOUBLE, b);
    unsigned long bits = 0;
    for (int i = 7; i >= 0; i--)
        bits = bits << 8 | b[i];
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

long ldf_to_long(const struct ldf *a)
{
    if (a->kind != LDF_FINITE) return 0;
    unsigned long mag;
    int dummy = 0;
    if (a->e >= 0) {
        if (big_bitlen(a->m) + a->e > 63)
            return a->neg ? (long)(1UL << 63) : 0x7fffffffffffffffL;
        mag = low64(big_shl(a->m, a->e), 0);
    } else {
        struct big t = big_shr(a->m, -a->e, &dummy);
        if (big_bitlen(t) > 63)
            return a->neg ? (long)(1UL << 63) : 0x7fffffffffffffffL;
        mag = low64(t, 0);
    }
    return a->neg ? -(long)mag : (long)mag;
}
