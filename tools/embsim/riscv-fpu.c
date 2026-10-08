/* riscv-fpu.c -- the RISC-V F and D extensions.
 *
 * The arithmetic is IEEE 754's, done here in integers rather than by the
 * host, so a result and its flags are the same on every host and with
 * any compiler: the five rounding modes, the accrued flags (fflags), the
 * tininess RISC-V detects after rounding, and the architecture's NaNs --
 * every NaN an operation produces is the canonical one, and a single in
 * a 64-bit register is NaN-boxed (its upper half all ones; anything else
 * reads as the canonical NaN). FMIN and FMAX are IEEE 754-2019's
 * minimumNumber and maximumNumber, as QEMU's are.
 *
 * A value is unpacked to a sign, an exponent and a significand, worked
 * on in 64 or 128 bits with the bits shifted out kept as a sticky bit,
 * and rounded and packed once (round_pack). The instructions are at the
 * end of the file. */
#include <string.h>

#include "riscv.h"

enum { RNE, RTZ, RDN, RUP, RMM };

/* ---- formats: d is 0 for single, 1 for double ------------------------- */

static int mbits(int d) { return d ? 52 : 23; }
static int bias(int d) { return d ? 1023 : 127; }
static u64 emask(int d) { return d ? 0x7ff : 0xff; }
static u64 mmask(int d) { return (1ull << mbits(d)) - 1; }
static int sgn(int d, u64 a) { return (int)(a >> (d ? 63 : 31)) & 1; }
static u64 expf(int d, u64 a) { return (a >> mbits(d)) & emask(d); }

static u64 pack(int d, int s, u64 e, u64 m)
{
    return (u64)s << (d ? 63 : 31) | e << mbits(d) | m;
}

static u64 canon(int d)
{
    return d ? 0x7ff8000000000000ull : 0x7fc00000u;
}

static u64 inf(int d, int s)
{
    return pack(d, s, emask(d), 0);
}

static int isnan_(int d, u64 a)
{
    return expf(d, a) == emask(d) && (a & mmask(d));
}

static int issnan(int d, u64 a)
{
    return isnan_(d, a) && !((a >> (mbits(d) - 1)) & 1);
}

static int isinf_(int d, u64 a)
{
    return expf(d, a) == emask(d) && !(a & mmask(d));
}

static int iszero(int d, u64 a)
{
    return !(a & (d ? ~(1ull << 63) : 0x7fffffffu));
}

/* ---- 128-bit integers --------------------------------------------------- */

struct u128 {
    u64 hi, lo;
};

static struct u128 mul64(u64 a, u64 b)
{
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    struct u128 r;
    r.lo = (mid << 32) | (u32)ll;
    r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return r;
}

static int bitlen64(u64 v)
{
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

static int bitlen128(struct u128 v)
{
    return v.hi ? 64 + bitlen64(v.hi) : bitlen64(v.lo);
}

static struct u128 shl128(struct u128 v, int n)
{
    struct u128 r;
    if (n == 0)
        return v;
    if (n >= 128) {
        r.hi = r.lo = 0;
    } else if (n >= 64) {
        r.hi = v.lo << (n - 64);
        r.lo = 0;
    } else {
        r.hi = v.hi << n | v.lo >> (64 - n);
        r.lo = v.lo << n;
    }
    return r;
}

/* shifted right, any bit shifted out ORed into the lowest */
static struct u128 shr128_jam(struct u128 v, int n)
{
    struct u128 r;
    int lost;
    if (n == 0)
        return v;
    if (n >= 128) {
        r.hi = 0;
        r.lo = (v.hi | v.lo) != 0;
        return r;
    }
    if (n >= 64) {
        lost = v.lo != 0 || (n > 64 && (v.hi << (128 - n)) != 0);
        r.lo = n == 64 ? v.hi : v.hi >> (n - 64);
        r.hi = 0;
    } else {
        lost = (v.lo << (64 - n)) != 0;
        r.lo = v.lo >> n | v.hi << (64 - n);
        r.hi = v.hi >> n;
    }
    r.lo |= (u64)lost;
    return r;
}

static int ge128(struct u128 a, struct u128 b)
{
    return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo);
}

static struct u128 add128(struct u128 a, struct u128 b)
{
    struct u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static struct u128 sub128(struct u128 a, struct u128 b)
{
    struct u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

/* ---- rounding ------------------------------------------------------------ */

/* sig >> sh rounded by rm (sh >= 1); *inexact when bits were lost */
static u64 rnd(u64 sig, int sh, int s, int rm, int *inexact)
{
    u64 r = sig >> sh, rb = sig & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    int up = 0;
    *inexact = rb != 0;
    switch (rm) {
    case RNE: up = rb > half || (rb == half && (r & 1)); break;
    case RTZ: up = 0; break;
    case RDN: up = s && rb; break;
    case RUP: up = !s && rb; break;
    default: up = rb >= half; break;
    }
    return r + (u64)up;
}

/* The value (-1)^s * sig * 2^(e - 62), rounded to format d by rm and
 * packed, with its flags. sig is any nonzero 64-bit integer whose low bit
 * may be a sticky bit. Tininess is detected after rounding. */
static u64 round_pack(int d, int s, int e, u64 sig, int rm, u32 *fl)
{
    int p = mbits(d) + 1, sh = 63 - p, emin = 1 - bias(d), ix;
    if (!sig)
        return pack(d, s, 0, 0);
    while (!(sig >> 62)) {
        sig <<= 1;
        e--;
    }
    if (sig >> 63) {
        sig = sig >> 1 | (sig & 1);
        e++;
    }
    if (e < emin) {
        int tiny = 1;
        if (e == emin - 1 && (rnd(sig, sh, s, rm, &ix) >> p))
            tiny = 0;                   /* rounds up to the smallest normal */
        int dd = emin - e;
        sig = dd >= 63 ? sig != 0 : sig >> dd | ((sig & ((1ull << dd) - 1)) != 0);
        u64 r = rnd(sig, sh, s, rm, &ix);
        if (ix) {
            *fl |= FF_NX;
            if (tiny)
                *fl |= FF_UF;
        }
        if (r >> (p - 1))
            return pack(d, s, 1, r & mmask(d));
        return pack(d, s, 0, r);
    }
    u64 r = rnd(sig, sh, s, rm, &ix);
    if (r >> p) {
        r >>= 1;
        e++;
    }
    if (ix)
        *fl |= FF_NX;
    if (e > bias(d)) {
        *fl |= FF_OF | FF_NX;
        int to_inf = rm == RNE || rm == RMM || (rm == RUP && !s) ||
                     (rm == RDN && s);
        return to_inf ? inf(d, s) : pack(d, s, emask(d) - 1, mmask(d));
    }
    return pack(d, s, (u64)(e + bias(d)), r & mmask(d));
}

/* a 128-bit significand: (-1)^s * v * 2^e2 */
static u64 round128(int d, int s, struct u128 v, int e2, int rm, u32 *fl)
{
    int n = bitlen128(v), sh = n > 63 ? n - 63 : 0;
    struct u128 t = shr128_jam(v, sh);
    return round_pack(d, s, e2 + sh + 62, t.lo, rm, fl);
}

/* a finite nonzero value's integer significand and its exponent:
 * value = m * 2^e */
static void unpack(int d, u64 a, u64 *m, int *e)
{
    u64 x = expf(d, a);
    *m = a & mmask(d);
    if (x) {
        *m |= 1ull << mbits(d);
        *e = (int)x - bias(d) - mbits(d);
    } else {
        *e = 1 - bias(d) - mbits(d);
    }
}

/* ---- the operations ------------------------------------------------------ */

/* the NaN result of an operation with a NaN operand, flagging a
 * signalling one */
static u64 nan_result(int d, const u64 *ops, int n, u32 *fl)
{
    for (int i = 0; i < n; i++)
        if (issnan(d, ops[i]))
            *fl |= FF_NV;
    return canon(d);
}

/* (-1)^sa * A * 2^ea + (-1)^sb * B * 2^eb, A and B nonzero, exactly
 * then rounded once */
static u64 add_core(int d, int sa, struct u128 A, int ea, int sb,
                    struct u128 B, int eb, int rm, u32 *fl)
{
    /* both with their top bit at 125 */
    int na = bitlen128(A), nb = bitlen128(B);
    A = shl128(A, 126 - na);
    ea -= 126 - na;
    B = shl128(B, 126 - nb);
    eb -= 126 - nb;
    int e;
    if (ea >= eb) {
        B = shr128_jam(B, ea - eb > 127 ? 128 : ea - eb);
        e = ea;
    } else {
        A = shr128_jam(A, eb - ea > 127 ? 128 : eb - ea);
        e = eb;
    }
    struct u128 r;
    int s;
    if (sa == sb) {
        r = add128(A, B);
        s = sa;
    } else if (ge128(A, B)) {
        r = sub128(A, B);
        s = sa;
    } else {
        r = sub128(B, A);
        s = sb;
    }
    if (!r.hi && !r.lo)
        return pack(d, rm == RDN, 0, 0);
    return round128(d, s, r, e, rm, fl);
}

static struct u128 w128(u64 v)
{
    struct u128 r;
    r.hi = 0;
    r.lo = v;
    return r;
}

static u64 f_add(int d, u64 a, u64 b, int sub, int rm, u32 *fl)
{
    u64 ops[2] = { a, b };
    if (isnan_(d, a) || isnan_(d, b))
        return nan_result(d, ops, 2, fl);
    int sa = sgn(d, a), sb = sgn(d, b) ^ sub;
    if (isinf_(d, a) || isinf_(d, b)) {
        if (isinf_(d, a) && isinf_(d, b) && sa != sb) {
            *fl |= FF_NV;
            return canon(d);
        }
        return isinf_(d, a) ? inf(d, sa) : inf(d, sb);
    }
    if (iszero(d, a) && iszero(d, b))
        return pack(d, sa == sb ? sa : rm == RDN, 0, 0);
    if (iszero(d, b))
        return a;
    if (iszero(d, a))
        return b ^ ((u64)sub << (d ? 63 : 31));
    u64 ma, mb;
    int ea, eb;
    unpack(d, a, &ma, &ea);
    unpack(d, b, &mb, &eb);
    return add_core(d, sa, w128(ma), ea, sb, w128(mb), eb, rm, fl);
}

static u64 f_mul(int d, u64 a, u64 b, int rm, u32 *fl)
{
    u64 ops[2] = { a, b };
    int s = sgn(d, a) ^ sgn(d, b);
    if (isnan_(d, a) || isnan_(d, b))
        return nan_result(d, ops, 2, fl);
    if (isinf_(d, a) || isinf_(d, b)) {
        if (iszero(d, a) || iszero(d, b)) {
            *fl |= FF_NV;
            return canon(d);
        }
        return inf(d, s);
    }
    if (iszero(d, a) || iszero(d, b))
        return pack(d, s, 0, 0);
    u64 ma, mb;
    int ea, eb;
    unpack(d, a, &ma, &ea);
    unpack(d, b, &mb, &eb);
    return round128(d, s, mul64(ma, mb), ea + eb, rm, fl);
}

/* (a * b) + c, the product's sign flipped by negp and c's by negc */
static u64 f_fma(int d, u64 a, u64 b, u64 c, int negp, int negc, int rm,
                 u32 *fl)
{
    u64 ops[3] = { a, b, c };
    int inval = (isinf_(d, a) && iszero(d, b)) || (iszero(d, a) && isinf_(d, b));
    if (isnan_(d, a) || isnan_(d, b) || isnan_(d, c)) {
        u64 r = nan_result(d, ops, 3, fl);
        if (inval)
            *fl |= FF_NV;
        return r;
    }
    if (inval) {
        *fl |= FF_NV;
        return canon(d);
    }
    int sp = sgn(d, a) ^ sgn(d, b) ^ negp, sc = sgn(d, c) ^ negc;
    if (isinf_(d, a) || isinf_(d, b)) {
        if (isinf_(d, c) && sc != sp) {
            *fl |= FF_NV;
            return canon(d);
        }
        return inf(d, sp);
    }
    if (isinf_(d, c))
        return inf(d, sc);
    u64 cc = negc ? c ^ ((u64)1 << (d ? 63 : 31)) : c;
    if (iszero(d, a) || iszero(d, b)) {
        if (iszero(d, c))
            return pack(d, sp == sc ? sp : rm == RDN, 0, 0);
        return cc;
    }
    u64 ma, mb, mc;
    int ea, eb, ec;
    unpack(d, a, &ma, &ea);
    unpack(d, b, &mb, &eb);
    if (iszero(d, c))
        return round128(d, sp, mul64(ma, mb), ea + eb, rm, fl);
    unpack(d, c, &mc, &ec);
    return add_core(d, sp, mul64(ma, mb), ea + eb, sc, w128(mc), ec, rm, fl);
}

static u64 f_div(int d, u64 a, u64 b, int rm, u32 *fl)
{
    u64 ops[2] = { a, b };
    int s = sgn(d, a) ^ sgn(d, b);
    if (isnan_(d, a) || isnan_(d, b))
        return nan_result(d, ops, 2, fl);
    if (isinf_(d, a)) {
        if (isinf_(d, b)) {
            *fl |= FF_NV;
            return canon(d);
        }
        return inf(d, s);
    }
    if (isinf_(d, b))
        return pack(d, s, 0, 0);
    if (iszero(d, b)) {
        if (iszero(d, a)) {
            *fl |= FF_NV;
            return canon(d);
        }
        *fl |= FF_DZ;
        return inf(d, s);
    }
    if (iszero(d, a))
        return pack(d, s, 0, 0);
    u64 ma, mb;
    int ea, eb;
    unpack(d, a, &ma, &ea);
    unpack(d, b, &mb, &eb);
    /* both with their top bit at 61, the dividend not below the divisor */
    int la = bitlen64(ma), lb = bitlen64(mb);
    ma <<= 62 - la;
    ea -= 62 - la;
    mb <<= 62 - lb;
    eb -= 62 - lb;
    if (ma < mb) {
        ma <<= 1;
        ea--;
    }
    u64 q = 0, r = ma;
    for (int i = 0; i < 63; i++) {
        q <<= 1;
        if (r >= mb) {
            r -= mb;
            q |= 1;
        }
        r <<= 1;
    }
    /* q / 2^62 is ma / mb */
    return round_pack(d, s, ea - eb, q | (r != 0), rm, fl);
}

static u64 f_sqrt(int d, u64 a, int rm, u32 *fl)
{
    if (isnan_(d, a))
        return nan_result(d, &a, 1, fl);
    if (iszero(d, a))
        return a;
    if (sgn(d, a)) {
        *fl |= FF_NV;
        return canon(d);
    }
    if (isinf_(d, a))
        return a;
    u64 m;
    int e;
    unpack(d, a, &m, &e);
    if (e & 1) {
        m <<= 1;
        e--;
    }
    /* the radicand m << sh, its top bit at 122 or 123, sh even */
    int L = bitlen64(m), sh = 124 - L;
    if (sh & 1)
        sh--;
    struct u128 R = shl128(w128(m), sh);
    u64 root = 0, rem = 0;
    for (int i = 0; i < 62; i++) {
        /* the radicand's next two bits, from the top of its 124 */
        int pos = 122 - 2 * i;
        u64 two = pos >= 64 ? (R.hi >> (pos - 64)) & 3
                : pos == 63 ? ((R.hi & 1) << 1) | (R.lo >> 63)
                : (R.lo >> pos) & 3;
        rem = rem << 2 | two;
        u64 trial = root << 2 | 1;
        root <<= 1;
        if (rem >= trial) {
            rem -= trial;
            root |= 1;
        }
    }
    /* root is sqrt(m << sh), about 2^61: value = root * 2^((e - sh) / 2) */
    return round_pack(d, 0, (e - sh) / 2 + 62, root | (rem != 0), rm, fl);
}

/* to an integer of `bits` (32 or 64), signed or not, saturating */
static u64 f_to_int(int d, u64 a, int bits, int uns, int rm, u32 *fl)
{
    int s = sgn(d, a);
    u64 max = uns ? (bits == 64 ? ~0ull : 0xffffffffull)
                  : (bits == 64 ? 0x7fffffffffffffffull : 0x7fffffffull);
    u64 min = uns ? 0 : (bits == 64 ? 1ull << 63 : 0xffffffff80000000ull);
    if (isnan_(d, a)) {
        *fl |= FF_NV;
        return max;
    }
    if (isinf_(d, a)) {
        *fl |= FF_NV;
        return s ? min : max;
    }
    if (iszero(d, a))
        return 0;
    u64 m;
    int e, ix = 0;
    unpack(d, a, &m, &e);
    /* the magnitude m * 2^e, rounded to an integer */
    u64 mag;
    if (e >= 0) {
        if (e + bitlen64(m) > 64)
            goto range;
        mag = m << e;
    } else if (-e > 64) {
        mag = rnd(m == 0 ? 0 : 1, 2, s, rm, &ix);  /* well below a half */
        ix = 1;
    } else if (-e == 64) {
        /* m < 2^53 < 2^63: below a half */
        mag = rnd(1, 2, s, rm, &ix);
        ix = 1;
    } else {
        mag = rnd(m, -e, s, rm, &ix);
    }
    if (uns) {
        if (s && mag)
            goto range;
        if (bits == 32 && mag > max)
            goto range;
    } else {
        if (!s && mag > max)
            goto range;
        if (s && mag > (bits == 64 ? 1ull << 63 : 0x80000000ull))
            goto range;
    }
    if (ix)
        *fl |= FF_NX;
    return s ? (u64)0 - mag : mag;
range:
    *fl |= FF_NV;
    return s ? min : max;
}

/* from an integer: `v` of `bits`, signed or not */
static u64 f_from_int(int d, u64 v, int bits, int uns, int rm, u32 *fl)
{
    int s = 0;
    if (bits == 32)
        v = uns ? (u64)(u32)v : (u64)(s64)(s32)(u32)v;
    if (!uns && (s64)v < 0) {
        s = 1;
        v = (u64)0 - v;
    }
    if (!v)
        return 0;
    return round_pack(d, s, 62, v, rm, fl);
}

/* single to double (exact) and double to single (rounded) */
static u64 f_convert(int to_d, u64 a, int rm, u32 *fl)
{
    int from = !to_d;
    if (isnan_(from, a)) {
        if (issnan(from, a))
            *fl |= FF_NV;
        return canon(to_d);
    }
    int s = sgn(from, a);
    if (isinf_(from, a))
        return inf(to_d, s);
    if (iszero(from, a))
        return pack(to_d, s, 0, 0);
    u64 m;
    int e;
    unpack(from, a, &m, &e);
    return round_pack(to_d, s, e + 62, m, rm, fl);
}

/* FEQ (quiet), FLT and FLE (signalling) */
static int f_cmp(int d, u64 a, u64 b, int op, u32 *fl)
{
    if (isnan_(d, a) || isnan_(d, b)) {
        if (op != 2 || issnan(d, a) || issnan(d, b))
            *fl |= FF_NV;
        return 0;
    }
    if (iszero(d, a) && iszero(d, b))
        return op != 1;                         /* equal */
    int sa = sgn(d, a), sb = sgn(d, b);
    u64 ma = a & ~((u64)1 << (d ? 63 : 31)), mb = b & ~((u64)1 << (d ? 63 : 31));
    int lt;
    if (sa != sb)
        lt = sa;
    else
        lt = sa ? ma > mb : ma < mb;
    if (op == 2)
        return a == b;
    return op == 1 ? lt : lt || a == b;
}

/* FMIN and FMAX: minimumNumber and maximumNumber */
static u64 f_minmax(int d, u64 a, u64 b, int max, u32 *fl)
{
    int na = isnan_(d, a), nb = isnan_(d, b);
    if (issnan(d, a) || issnan(d, b))
        *fl |= FF_NV;
    if (na && nb)
        return canon(d);
    if (na)
        return b;
    if (nb)
        return a;
    u32 dummy = 0;
    int lt = f_cmp(d, a, b, 1, &dummy);
    if (iszero(d, a) && iszero(d, b) && sgn(d, a) != sgn(d, b))
        lt = sgn(d, a);                         /* -0 below +0 */
    return (lt ^ max) ? a : b;
}

static u64 f_class(int d, u64 a)
{
    int s = sgn(d, a);
    if (isinf_(d, a))
        return s ? 1 : 1u << 7;
    if (isnan_(d, a))
        return issnan(d, a) ? 1u << 8 : 1u << 9;
    if (iszero(d, a))
        return s ? 1u << 3 : 1u << 4;
    if (!expf(d, a))
        return s ? 1u << 2 : 1u << 5;
    return s ? 1u << 1 : 1u << 6;
}

/* ---- the registers --------------------------------------------------------- */

static u64 rd_s(u32 r)
{
    u64 v = rs->f[r];
    return (v >> 32) == 0xffffffffu ? (u32)v : 0x7fc00000u;
}

static u64 rd_f(int d, u32 r)
{
    return d ? rs->f[r] : rd_s(r);
}

static void wr_f(int d, u32 r, u64 v)
{
    rs->f[r] = d ? v : 0xffffffff00000000ull | (u32)v;
    rv_fp_dirty();
}

/* the rounding mode of an instruction, or -1 for an illegal one */
static int rmode(u32 f3)
{
    int rm = (int)(f3 == 7 ? rs->frm : f3);
    return rm > 4 ? -1 : rm;
}

static void flags(u32 fl)
{
    if (fl) {
        rs->fflags |= fl;
        rv_fp_dirty();
    }
}

/* ---- the instructions ------------------------------------------------------- */

int rv_fp_exec(u32 i)
{
    u32 op = i & 0x7f, rd = (i >> 7) & 31, f3 = (i >> 12) & 7;
    u32 r1 = (i >> 15) & 31, r2 = (i >> 20) & 31, f7 = i >> 25;
    int rv64 = rs->xlen == 64;
    u32 fl = 0;
    if (op != 0x07 && op != 0x27 && op != 0x43 && op != 0x47 &&
        op != 0x4b && op != 0x4f && op != 0x53)
        return 0;
    if (!(rs->mstatus & MSTATUS_FS)) {
        rv_illegal();
        return 1;
    }
    if (op == 0x07 || op == 0x27) {             /* FLW FLD FSW FSD */
        if (f3 != 2 && f3 != 3) {
            rv_illegal();
            return 1;
        }
        int n = f3 == 2 ? 4 : 8;
        if (op == 0x07) {
            u64 a = rv_xl(rs->x[r1] + (u64)(s64)((s32)i >> 20));
            u64 v = rv_load(a, n);
            if (!rs->trap)
                wr_f(f3 == 3, rd, v);
        } else {
            s64 imm = (s64)(((s32)i >> 25) << 5) | ((i >> 7) & 31);
            rv_store(rv_xl(rs->x[r1] + (u64)imm), n, rs->f[r2]);
        }
        return 1;
    }
    int fmt = (int)(f7 & 3);
    if (fmt > 1) {
        rv_illegal();
        return 1;
    }
    int d = fmt;
    if (op != 0x53) {                           /* the fused forms */
        int rm = rmode(f3);
        if (rm < 0) {
            rv_illegal();
            return 1;
        }
        u64 r = f_fma(d, rd_f(d, r1), rd_f(d, r2), rd_f(d, i >> 27),
                      op == 0x4b || op == 0x4f, op == 0x47 || op == 0x4f, rm,
                      &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    }
    u64 a = rd_f(d, r1), b = rd_f(d, r2), r;
    u32 f5 = f7 >> 2;
    int rm = rmode(f3);
    switch (f5) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x0b:
        if (rm < 0 || (f5 == 0x0b && r2)) {
            rv_illegal();
            return 1;
        }
        r = f5 == 0x00 ? f_add(d, a, b, 0, rm, &fl)
          : f5 == 0x01 ? f_add(d, a, b, 1, rm, &fl)
          : f5 == 0x02 ? f_mul(d, a, b, rm, &fl)
          : f5 == 0x03 ? f_div(d, a, b, rm, &fl) : f_sqrt(d, a, rm, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x04: {                                /* FSGNJ, FSGNJN, FSGNJX */
        u64 sb = (u64)1 << (d ? 63 : 31), s;
        if (f3 > 2) {
            rv_illegal();
            return 1;
        }
        s = f3 == 0 ? b & sb : f3 == 1 ? ~b & sb : (a ^ b) & sb;
        wr_f(d, rd, (a & ~sb) | s);
        return 1;
    }
    case 0x05:                                  /* FMIN, FMAX */
        if (f3 > 1) {
            rv_illegal();
            return 1;
        }
        r = f_minmax(d, a, b, (int)f3, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x08:                                  /* FCVT.S.D, FCVT.D.S */
        if (rm < 0 || r2 != (u32)!d) {
            rv_illegal();
            return 1;
        }
        r = f_convert(d, rd_f(!d, r1), rm, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x14:                                  /* FLE FLT FEQ */
        if (f3 > 2) {
            rv_illegal();
            return 1;
        }
        r = (u64)f_cmp(d, a, b, (int)f3, &fl);
        rv_wx(rd, r);
        flags(fl);
        return 1;
    case 0x18:                                  /* FCVT.W[U]/L[U].fmt */
        if (rm < 0 || r2 > 3 || (r2 > 1 && !rv64)) {
            rv_illegal();
            return 1;
        }
        r = f_to_int(d, a, r2 > 1 ? 64 : 32, (int)(r2 & 1), rm, &fl);
        rv_wx(rd, r2 > 1 ? r : (u64)(s64)(s32)(u32)r);
        flags(fl);
        return 1;
    case 0x1a:                                  /* FCVT.fmt.W[U]/L[U] */
        if (rm < 0 || r2 > 3 || (r2 > 1 && !rv64)) {
            rv_illegal();
            return 1;
        }
        r = f_from_int(d, rs->x[r1], r2 > 1 ? 64 : 32, (int)(r2 & 1), rm, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x1c:                                  /* FMV.X.W/D, FCLASS */
        if (r2 || f3 > 1 || (f3 == 0 && d && !rv64)) {
            rv_illegal();
            return 1;
        }
        if (f3 == 1)
            rv_wx(rd, f_class(d, a));
        else
            rv_wx(rd, d ? rs->f[r1] : (u64)(s64)(s32)(u32)rs->f[r1]);
        return 1;
    case 0x1e:                                  /* FMV.W/D.X */
        if (r2 || f3 || (d && !rv64)) {
            rv_illegal();
            return 1;
        }
        wr_f(d, rd, rs->x[r1]);
        return 1;
    }
    rv_illegal();
    return 1;
}
