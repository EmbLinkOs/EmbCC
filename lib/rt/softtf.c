/* IEEE-754 binary128, in software. aarch64 only.
 *
 * On aarch64 `long double` is binary128 -- a 113-bit significand -- and
 * the machine has no instruction for any of it. Not the arithmetic, not
 * the comparisons, not the conversions: `a + b` on two long doubles is
 * a call to `__addtf3`. x86-64's `long double` is the x87 80-bit format
 * and the hardware does all of it, which is why this file is guarded
 * and `ldouble.c` is guarded the other way.
 *
 * ---- the rule this file lives under ---------------------------------------
 *
 * It may not use the type it implements. Every routine takes and
 * returns `long double` -- it has to, or the ABI would be wrong, since
 * aarch64 passes a binary128 in a v register and a pair of integers in
 * x registers -- but the value is converted to its bits through a union
 * on the way in and back on the way out, and nothing in between is a
 * floating-point operation. A single `a * b` here would be a call to
 * `__multf3`, which is this file.
 *
 * ---- the representation ---------------------------------------------------
 *
 *   bit 127      sign
 *   bits 126:112 exponent, excess-16383
 *   bits 111:0   significand, with an implicit leading 1 for normals
 *
 * Unpacked, a number is (sign, exp, sig) meaning (-1)^sign * sig *
 * 2^exp, where a normal's `sig` has its leading bit at position 112.
 * Zero has sig == 0; subnormals have a leading bit lower than 112 and
 * are normalised on the way in, so the arithmetic below never has to
 * think about them again -- which is the usual place a soft-float goes
 * wrong.
 *
 * ---- rounding -------------------------------------------------------------
 *
 * Round to nearest, ties to even, which is the only mode this
 * implements: nothing in the toolchain changes the rounding mode, and a
 * soft-float that pretends to support a mode it never sees is a lie
 * that compiles.
 *
 * The arithmetic carries three extra bits below the significand -- a
 * guard, a round, and a sticky that is the OR of everything shifted
 * past it. Those three are exactly enough to round correctly, which is
 * the classical result and the reason the intermediate is 116 bits
 * rather than 113.
 */
#include "rt.h"

#ifdef __aarch64__

union tfbits {
    long double f;
    u128 u;
    struct { u64 lo, hi; } h;
};

#define TF_BIAS      16383
#define TF_SIGBITS   112               /* stored; 113 with the implicit one */
#define TF_EXPMAX    0x7FFF

/* The unpacked form. `cls` keeps the special cases out of the
 * arithmetic, where mixing them into the normal path is how an
 * infinity ends up with a significand. */
enum tfcls { TF_ZERO, TF_NORMAL, TF_INF, TF_NAN };

struct tf {
    int sign;
    int exp;                           /* of sig * 2^exp */
    u128 sig;                          /* leading bit at 112 when normal */
    enum tfcls cls;
};

/* ---- the 128-bit helpers this file needs --------------------------------
 *
 * Written out rather than using `u128` operators for the shifts,
 * because a variable-count 128-bit shift is itself a call (__ashlti3)
 * and going through it here would be slower than the two branches it
 * contains. Addition and comparison DO use the operators: those the
 * machine really has. */
static u128 shl128(u128 a, int n)
{
    u64 lo = lo64(a), hi = hi64(a);
    if (n == 0) return a;
    if (n >= 128) return mk(0, 0);
    if (n >= 64) return mk(lo << (n - 64), 0);
    return mk((hi << n) | (lo >> (64 - n)), lo << n);
}

static u128 shr128(u128 a, int n)
{
    u64 lo = lo64(a), hi = hi64(a);
    if (n == 0) return a;
    if (n >= 128) return mk(0, 0);
    if (n >= 64) return mk(0, hi >> (n - 64));
    return mk(hi >> n, (lo >> n) | (hi << (64 - n)));
}

/* Shift right by n, folding everything shifted out into bit 0. This is
 * the sticky bit, and it is why a >> that merely discards is not enough:
 * the rounding has to know that something was there, not just what the
 * next two bits were. */
static u128 shr_sticky(u128 a, int n)
{
    u128 out;
    if (n == 0) return a;
    if (n >= 128) return mk(0, (lo64(a) | hi64(a)) != 0);
    out = shl128(a, 128 - n);
    return shr128(a, n) | (u128)((lo64(out) | hi64(out)) != 0);
}

static int clz128(u128 a)
{
    u64 hi = hi64(a), lo = lo64(a);
    u64 x = hi ? hi : lo;
    int n = hi ? 0 : 64;
    if (x == 0) return 128;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8;  x <<= 8;  }
    if (!(x >> 60)) { n += 4;  x <<= 4;  }
    if (!(x >> 62)) { n += 2;  x <<= 2;  }
    if (!(x >> 63)) { n += 1; }
    return n;
}

static int is_zero128(u128 a) { return (lo64(a) | hi64(a)) == 0; }

/* ---- unpack and pack ---------------------------------------------------- */

static struct tf unpack(long double x)
{
    union tfbits b;
    struct tf r;
    u128 frac;
    int e;

    b.f = x;
    r.sign = (int)(b.h.hi >> 63);
    e = (int)((b.h.hi >> 48) & TF_EXPMAX);
    frac = mk(b.h.hi & 0x0000FFFFFFFFFFFFULL, b.h.lo);

    if (e == 0) {
        if (is_zero128(frac)) {
            r.cls = TF_ZERO;
            r.exp = 0;
            r.sig = mk(0, 0);
            return r;
        }
        /* Subnormal: normalise it now, once, so that everything after
         * this point sees one shape of number. The exponent it gets is
         * the one a normal of the same value would have had if the
         * format went that low. */
        {
            int sh = clz128(frac) - (127 - TF_SIGBITS);
            r.cls = TF_NORMAL;
            r.sig = shl128(frac, sh);
            r.exp = 1 - TF_BIAS - TF_SIGBITS - sh;
            return r;
        }
    }
    if (e == TF_EXPMAX) {
        r.cls = is_zero128(frac) ? TF_INF : TF_NAN;
        r.exp = 0;
        r.sig = frac;
        return r;
    }
    r.cls = TF_NORMAL;
    r.sig = frac | shl128(mk(0, 1), TF_SIGBITS);   /* the implicit bit */
    r.exp = e - TF_BIAS - TF_SIGBITS;
    return r;
}

static long double from_bits(int sign, int biased_exp, u128 frac)
{
    union tfbits b;
    b.h.lo = lo64(frac);
    b.h.hi = (lo64(shr128(frac, 64)) & 0x0000FFFFFFFFFFFFULL) |
             ((u64)(biased_exp & TF_EXPMAX) << 48) |
             ((u64)(sign & 1) << 63);
    return b.f;
}

static long double tf_zero(int sign)  { return from_bits(sign, 0, mk(0, 0)); }
static long double tf_inf(int sign)   { return from_bits(sign, TF_EXPMAX,
                                                         mk(0, 0)); }
/* The default quiet NaN: the top significand bit set, as every
 * implementation on this machine produces. */
static long double tf_nan(void)
{
    return from_bits(0, TF_EXPMAX, shl128(mk(0, 1), TF_SIGBITS - 1));
}

/* A NaN operand is propagated rather than replaced, quieted if it
 * arrived signalling -- which is what IEEE-754 6.2 asks and what makes
 * a NaN carry information through a computation. */
static long double propagate(struct tf a, struct tf b)
{
    struct tf *n = a.cls == TF_NAN ? &a : &b;
    u128 quiet = n->sig | shl128(mk(0, 1), TF_SIGBITS - 1);
    return from_bits(n->sign, TF_EXPMAX, quiet);
}

/* Round `sig` -- which carries THREE extra low bits -- to a binary128
 * of the given sign, where the value is sig * 2^exp. The significand
 * arrives with its leading bit anywhere; this normalises, rounds, and
 * handles both ends of the exponent range. */
static long double round_pack(int sign, int exp, u128 sig)
{
    int lead, shift, biased;
    u128 lsb, half;

    if (is_zero128(sig))
        return tf_zero(sign);

    /* Normalise so the leading bit sits at TF_SIGBITS + 3, which puts
     * the guard, round and sticky in bits 2, 1 and 0. */
    lead = 127 - clz128(sig);
    shift = (TF_SIGBITS + 3) - lead;
    if (shift > 0) {
        sig = shl128(sig, shift);
        exp -= shift;
    } else if (shift < 0) {
        sig = shr_sticky(sig, -shift);
        exp -= shift;
    }

    /* The biased exponent this value would have. `exp` is of
     * sig * 2^exp with sig's leading bit at TF_SIGBITS+3, so the
     * value is 1.xxx * 2^(exp + TF_SIGBITS + 3). */
    biased = exp + TF_SIGBITS + 3 + TF_BIAS;

    if (biased >= TF_EXPMAX)
        return tf_inf(sign);           /* overflow: nearest gives infinity */

    if (biased <= 0) {
        /* Subnormal, or under it. Shift right by the shortfall, keeping
         * the sticky bit, and let the rounding below decide -- which is
         * how a value just under the smallest normal rounds UP to it,
         * the case a naive implementation flushes to zero. */
        int under = 1 - biased;
        if (under > 127)
            return tf_zero(sign);
        sig = shr_sticky(sig, under);
        biased = 0;
    }

    /* Round to nearest, ties to even. The three extra bits are exactly
     * half when they read 100, and then the tie goes to the even
     * significand. */
    lsb = shr128(sig, 3) & mk(0, 1);
    half = sig & mk(0, 7);
    if (hi64(half) == 0 && lo64(half) > 4)
        sig = sig + mk(0, 8);
    else if (hi64(half) == 0 && lo64(half) == 4 && !is_zero128(lsb))
        sig = sig + mk(0, 8);
    sig = shr128(sig, 3);

    /* The rounding may have carried into a new leading bit. */
    if (!is_zero128(sig & shl128(mk(0, 1), TF_SIGBITS + 1))) {
        sig = shr128(sig, 1);
        biased++;
        if (biased >= TF_EXPMAX)
            return tf_inf(sign);
    } else if (biased == 0 &&
               !is_zero128(sig & shl128(mk(0, 1), TF_SIGBITS))) {
        /* A subnormal that rounded up into the smallest normal. Its
         * stored significand is already right; only the exponent
         * changes, which is what makes gradual underflow continuous. */
        biased = 1;
    }

    if (biased == 0)
        return from_bits(sign, 0, sig);
    return from_bits(sign, biased,
                     sig & (shl128(mk(0, 1), TF_SIGBITS) - mk(0, 1)));
}

/* ---- add and subtract --------------------------------------------------- */

static long double addsub(long double x, long double y, int negate_y)
{
    struct tf a = unpack(x), b = unpack(y);
    int diff;

    if (negate_y && b.cls != TF_NAN)
        b.sign ^= 1;

    if (a.cls == TF_NAN || b.cls == TF_NAN)
        return propagate(a, b);
    if (a.cls == TF_INF) {
        /* inf + (-inf) has no answer; every other sum with an infinity
         * is that infinity. */
        if (b.cls == TF_INF && a.sign != b.sign)
            return tf_nan();
        return tf_inf(a.sign);
    }
    if (b.cls == TF_INF)
        return tf_inf(b.sign);
    if (a.cls == TF_ZERO && b.cls == TF_ZERO)
        /* -0 + -0 is -0; every other pair of zeroes is +0, which is
         * what round-to-nearest requires. */
        return tf_zero(a.sign & b.sign);
    /* One operand is zero: the answer is the other one, repacked --
     * `y` itself will not do, because negate_y may have flipped b's
     * sign and y has not been touched. b is normal here, since the
     * zero + zero case was taken above. */
    if (a.cls == TF_ZERO)
        return round_pack(b.sign, b.exp - 3, shl128(b.sig, 3));
    if (b.cls == TF_ZERO)
        return x;

    /* Make room for the three rounding bits before aligning, so that
     * what the smaller operand loses is caught by the sticky bit rather
     * than dropped. */
    a.sig = shl128(a.sig, 3); a.exp -= 3;
    b.sig = shl128(b.sig, 3); b.exp -= 3;

    diff = a.exp - b.exp;
    if (diff > 0) {
        if (diff > 130) { b.sig = mk(0, !is_zero128(b.sig)); b.exp = a.exp; }
        else { b.sig = shr_sticky(b.sig, diff); b.exp = a.exp; }
    } else if (diff < 0) {
        if (-diff > 130) { a.sig = mk(0, !is_zero128(a.sig)); a.exp = b.exp; }
        else { a.sig = shr_sticky(a.sig, -diff); a.exp = b.exp; }
    }

    if (a.sign == b.sign)
        return round_pack(a.sign, a.exp, a.sig + b.sig);

    /* Opposite signs: subtract the smaller magnitude from the larger,
     * and the result takes the larger's sign. An exact cancellation is
     * +0, which is the one case where the sign is decided by the
     * rounding mode rather than by the operands. */
    if (a.sig == b.sig)
        return tf_zero(0);
    if (a.sig > b.sig)
        return round_pack(a.sign, a.exp, a.sig - b.sig);
    return round_pack(b.sign, b.exp, b.sig - a.sig);
}

long double __addtf3(long double a, long double b) { return addsub(a, b, 0); }
long double __subtf3(long double a, long double b) { return addsub(a, b, 1); }

long double __negtf2(long double a)
{
    union tfbits b;
    b.f = a;
    b.h.hi ^= 0x8000000000000000ULL;
    return b.f;
}

/* ---- multiply ----------------------------------------------------------- */

long double __multf3(long double x, long double y)
{
    struct tf a = unpack(x), b = unpack(y);
    int sign;
    u64 al, ah, bl, bh;
    u64 p0lo, p0hi, p1lo, p1hi, p2lo, p2hi, p3lo, p3hi;
    u64 w0, w1, w2, w3, carry;
    u128 hi, lo;

    if (a.cls == TF_NAN || b.cls == TF_NAN)
        return propagate(a, b);
    sign = a.sign ^ b.sign;
    if (a.cls == TF_INF || b.cls == TF_INF) {
        /* Zero times infinity is the one product with no answer. */
        if (a.cls == TF_ZERO || b.cls == TF_ZERO)
            return tf_nan();
        return tf_inf(sign);
    }
    if (a.cls == TF_ZERO || b.cls == TF_ZERO)
        return tf_zero(sign);

    /* 113 x 113 -> 226 bits, as four 64x64 products assembled with
     * carries. The result is kept as a 256-bit value in w3:w2:w1:w0 and
     * then folded to 128 bits plus a sticky. */
    al = lo64(a.sig); ah = hi64(a.sig);
    bl = lo64(b.sig); bh = hi64(b.sig);
    p0hi = rt_mul64(al, bl, &p0lo);
    p1hi = rt_mul64(al, bh, &p1lo);
    p2hi = rt_mul64(ah, bl, &p2lo);
    p3hi = rt_mul64(ah, bh, &p3lo);

    w0 = p0lo;
    w1 = p0hi + p1lo;      carry  = w1 < p0hi;
    w1 += p2lo;            carry += w1 < p2lo;
    w2 = p1hi + p2hi;      w3 = w2 < p1hi;
    w2 += p3lo;            w3 += w2 < p3lo;
    w2 += carry;           w3 += w2 < carry;
    w3 += p3hi;

    hi = mk(w3, w2);
    lo = mk(w1, w0);

    /* Narrow the 226-bit product to something round_pack can round.
     *
     * The shift is 109 and not a byte more, and that is the whole
     * subtlety: two 113-bit significands make a product whose leading
     * bit is at 224 or 225, and round_pack needs the top 113 bits PLUS
     * three below them to round with. 225 - 115 = 110, so shifting by
     * 109 always leaves at least those 116 bits, and never more than
     * 128 so nothing falls off the top.
     *
     * Shifting further -- 125, which is what this did first -- keeps
     * only 100 bits of significand and throws the next 13 away before
     * anything can round with them. It is not a rounding error: the
     * result is simply missing its low bits, and `1 * (1 + 1ulp)` comes
     * back as 1 + 8192ulp. Everything the product needs is discarded
     * BEFORE the rounding that was supposed to use it, which is why the
     * exponent still looked right. */
    {
        u128 folded = shl128(hi, 19) | shr128(lo, 109);
        int sticky = !is_zero128(lo & (shl128(mk(0, 1), 109) - mk(0, 1)));
        return round_pack(sign, a.exp + b.exp + 109,
                          folded | (u128)(unsigned)sticky);
    }
}

/* ---- divide ------------------------------------------------------------- */

long double __divtf3(long double x, long double y)
{
    struct tf a = unpack(x), b = unpack(y);
    int sign, i;
    u128 rem, quo, den;

    if (a.cls == TF_NAN || b.cls == TF_NAN)
        return propagate(a, b);
    sign = a.sign ^ b.sign;
    if (a.cls == TF_INF) {
        if (b.cls == TF_INF)
            return tf_nan();           /* inf / inf */
        return tf_inf(sign);
    }
    if (b.cls == TF_INF)
        return tf_zero(sign);
    if (b.cls == TF_ZERO) {
        if (a.cls == TF_ZERO)
            return tf_nan();           /* 0 / 0 */
        return tf_inf(sign);           /* finite / 0 */
    }
    if (a.cls == TF_ZERO)
        return tf_zero(sign);

    /* Restoring division, most significant bit first, for 116 bits --
     * the 113 of the significand plus the three rounding bits. Slow and
     * correct: the alternative is a reciprocal iteration whose last bit
     * needs a proof, and this library's rule is correct before fast.
     *
     * The dividend starts at the divisor's magnitude so the first
     * quotient bit is the leading one, and the exponent below accounts
     * for the 116 shifts. */
    rem = a.sig;
    den = b.sig;
    quo = mk(0, 0);
    for (i = 0; i < TF_SIGBITS + 4; i++) {
        quo = shl128(quo, 1);
        if (rem >= den) {
            rem = rem - den;
            quo = quo | mk(0, 1);
        }
        rem = shl128(rem, 1);
    }
    /* Anything left over means the quotient was not exact, which the
     * sticky bit has to say or a tie rounds the wrong way. */
    if (!is_zero128(rem))
        quo = quo | mk(0, 1);

    /* The loop produced 116 bits of quotient with the FIRST one at the
     * top, so quo = (a.sig / b.sig) * 2^115 -- 115 and not 116, because
     * the first iteration tests before shifting and therefore yields
     * the integer part rather than the first fractional bit. One too
     * many here made every quotient exactly half of what it should be,
     * which `1 / 1 == 0.5` says more plainly than any reasoning. */
    return round_pack(sign, a.exp - b.exp - (TF_SIGBITS + 3), quo);
}

/* ---- comparisons --------------------------------------------------------
 *
 * The libgcc contract, which is not the obvious one: each returns an
 * int whose SIGN answers the comparison (negative for less, zero for
 * equal, positive for greater), and an unordered pair returns 1 from
 * __gttf2/__getf2 and -1 from the rest -- chosen so that the caller's
 * test against zero gives false for every ordered comparison involving
 * a NaN. Getting this backwards makes `x < y` true for NaNs, which
 * compiles and passes anything that does not test NaNs. */
static int compare(long double x, long double y, int nan_result)
{
    struct tf a = unpack(x), b = unpack(y);

    if (a.cls == TF_NAN || b.cls == TF_NAN)
        return nan_result;
    if (a.cls == TF_ZERO && b.cls == TF_ZERO)
        return 0;                      /* -0 == +0 */
    if (a.sign != b.sign)
        return a.sign ? -1 : 1;
    /* Same sign: compare magnitudes, then flip for negatives. */
    {
        int m;
        if (a.cls == TF_INF && b.cls == TF_INF) m = 0;
        else if (a.cls == TF_INF)               m = 1;
        else if (b.cls == TF_INF)               m = -1;
        else if (a.cls == TF_ZERO)              m = b.cls == TF_ZERO ? 0 : -1;
        else if (b.cls == TF_ZERO)              m = 1;
        else if (a.exp != b.exp)                m = a.exp > b.exp ? 1 : -1;
        else if (a.sig != b.sig)                m = a.sig > b.sig ? 1 : -1;
        else                                    m = 0;
        return a.sign ? -m : m;
    }
}

int __eqtf2(long double a, long double b) { return compare(a, b, 1); }
int __netf2(long double a, long double b) { return compare(a, b, 1); }
int __lttf2(long double a, long double b) { return compare(a, b, 1); }
int __letf2(long double a, long double b) { return compare(a, b, 1); }
int __gttf2(long double a, long double b) { return compare(a, b, -1); }
int __getf2(long double a, long double b) { return compare(a, b, -1); }

int __unordtf2(long double a, long double b)
{
    struct tf x = unpack(a), y = unpack(b);
    return x.cls == TF_NAN || y.cls == TF_NAN;
}

/* ---- conversions: narrower floats <-> binary128 ------------------------- */

long double __extenddftf2(double x)
{
    union { double d; u64 u; } v;
    int e, sign;
    u64 frac;

    v.d = x;
    sign = (int)(v.u >> 63);
    e = (int)((v.u >> 52) & 0x7FF);
    frac = v.u & 0xFFFFFFFFFFFFFULL;

    if (e == 0x7FF) {
        if (frac == 0)
            return tf_inf(sign);
        /* A NaN keeps its payload, moved up to the wider significand's
         * top bits so that the quiet marker stays the top one. */
        return from_bits(sign, TF_EXPMAX, shl128(mk(0, frac), 60));
    }
    if (e == 0) {
        if (frac == 0)
            return tf_zero(sign);
        /* A double's subnormal is a binary128 NORMAL: the wider
         * exponent range reaches it comfortably, so it is normalised
         * rather than carried across as a subnormal. */
        return round_pack(sign, -1074 - 3, shl128(mk(0, frac), 3));
    }
    return round_pack(sign, e - 1075 - 3,
                      shl128(mk(0, frac | (1ULL << 52)), 3));
}

long double __extendsftf2(float x)
{
    union { float f; u32 u; } v;
    int e, sign;
    u64 frac;

    v.f = x;
    sign = (int)(v.u >> 31);
    e = (int)((v.u >> 23) & 0xFF);
    frac = v.u & 0x7FFFFFU;

    if (e == 0xFF) {
        if (frac == 0)
            return tf_inf(sign);
        return from_bits(sign, TF_EXPMAX, shl128(mk(0, frac), 89));
    }
    if (e == 0) {
        if (frac == 0)
            return tf_zero(sign);
        return round_pack(sign, -149 - 3, shl128(mk(0, frac), 3));
    }
    return round_pack(sign, e - 150 - 3,
                      shl128(mk(0, frac | (1U << 23)), 3));
}

/* Narrowing needs its own rounding, because the target's significand is
 * shorter: the same guard/round/sticky argument, at 53 and 24 bits. */
static double pack_double(int sign, int exp, u128 sig)
{
    union { double d; u64 u; } v;
    int lead, shift, biased;
    u64 s, half, lsb;

    if (is_zero128(sig)) {
        v.u = (u64)sign << 63;
        return v.d;
    }
    lead = 127 - clz128(sig);
    shift = (52 + 3) - lead;
    if (shift > 0) { sig = shl128(sig, shift); exp -= shift; }
    else if (shift < 0) { sig = shr_sticky(sig, -shift); exp -= shift; }
    biased = exp + 52 + 3 + 1023;

    if (biased >= 0x7FF) {
        v.u = ((u64)sign << 63) | (0x7FFULL << 52);
        return v.d;
    }
    if (biased <= 0) {
        int under = 1 - biased;
        if (under > 127) { v.u = (u64)sign << 63; return v.d; }
        sig = shr_sticky(sig, under);
        biased = 0;
    }
    s = lo64(sig);
    lsb = (s >> 3) & 1;
    half = s & 7;
    if (half > 4 || (half == 4 && lsb))
        s += 8;
    s >>= 3;
    if (s & (1ULL << 53)) { s >>= 1; biased++;
                            if (biased >= 0x7FF) {
                                v.u = ((u64)sign << 63) | (0x7FFULL << 52);
                                return v.d; } }
    else if (biased == 0 && (s & (1ULL << 52)))
        biased = 1;
    v.u = ((u64)sign << 63) | ((u64)(biased & 0x7FF) << 52) |
          (s & 0xFFFFFFFFFFFFFULL);
    return v.d;
}

double __trunctfdf2(long double x)
{
    struct tf a = unpack(x);
    union { double d; u64 u; } v;

    if (a.cls == TF_NAN) {
        /* Quiet, and keep what payload fits. */
        v.u = ((u64)a.sign << 63) | (0x7FFULL << 52) |
              (lo64(shr128(a.sig, 60)) & 0xFFFFFFFFFFFFFULL) |
              (1ULL << 51);
        return v.d;
    }
    if (a.cls == TF_INF) {
        v.u = ((u64)a.sign << 63) | (0x7FFULL << 52);
        return v.d;
    }
    if (a.cls == TF_ZERO) {
        v.u = (u64)a.sign << 63;
        return v.d;
    }
    return pack_double(a.sign, a.exp - 3, shl128(a.sig, 3));
}

float __trunctfsf2(long double x)
{
    struct tf a = unpack(x);
    union { float f; u32 u; } v;
    int lead, shift, biased;
    u64 s, half, lsb;
    u128 sig;
    int exp;

    if (a.cls == TF_NAN) {
        v.u = ((u32)a.sign << 31) | (0xFFU << 23) |
              ((u32)lo64(shr128(a.sig, 89)) & 0x7FFFFFU) | (1U << 22);
        return v.f;
    }
    if (a.cls == TF_INF) { v.u = ((u32)a.sign << 31) | (0xFFU << 23);
                           return v.f; }
    if (a.cls == TF_ZERO) { v.u = (u32)a.sign << 31; return v.f; }

    sig = shl128(a.sig, 3);
    exp = a.exp - 3;
    lead = 127 - clz128(sig);
    shift = (23 + 3) - lead;
    if (shift > 0) { sig = shl128(sig, shift); exp -= shift; }
    else if (shift < 0) { sig = shr_sticky(sig, -shift); exp -= shift; }
    biased = exp + 23 + 3 + 127;
    if (biased >= 0xFF) { v.u = ((u32)a.sign << 31) | (0xFFU << 23);
                          return v.f; }
    if (biased <= 0) {
        int under = 1 - biased;
        if (under > 127) { v.u = (u32)a.sign << 31; return v.f; }
        sig = shr_sticky(sig, under);
        biased = 0;
    }
    s = lo64(sig);
    lsb = (s >> 3) & 1;
    half = s & 7;
    if (half > 4 || (half == 4 && lsb))
        s += 8;
    s >>= 3;
    if (s & (1ULL << 24)) { s >>= 1; biased++;
                            if (biased >= 0xFF) {
                                v.u = ((u32)a.sign << 31) | (0xFFU << 23);
                                return v.f; } }
    else if (biased == 0 && (s & (1ULL << 23)))
        biased = 1;
    v.u = ((u32)a.sign << 31) | ((u32)(biased & 0xFF) << 23) |
          ((u32)s & 0x7FFFFFU);
    return v.f;
}

/* ---- conversions: integers <-> binary128 --------------------------------
 *
 * Every integer this machine has fits in a binary128 significand
 * EXACTLY -- 113 bits is wider than 64 and wider than the 128-bit
 * types' useful range minus their low bits -- so the only rounding is
 * for __int128 values above 2^113, and round_pack does it. */
long double __floatunditf(u64 a)
{
    if (a == 0) return tf_zero(0);
    return round_pack(0, -3, shl128(mk(0, a), 3));
}

long double __floatditf(s64 a)
{
    if (a == 0) return tf_zero(0);
    if (a < 0) {
        u64 m = (u64)-(a + 1) + 1;     /* negate without overflowing */
        return round_pack(1, -3, shl128(mk(0, m), 3));
    }
    return round_pack(0, -3, shl128(mk(0, (u64)a), 3));
}

long double __floatsitf(int a)  { return __floatditf((s64)a); }
long double __floatunsitf(u32 a) { return __floatunditf((u64)a); }

long double __floatuntitf(u128 a)
{
    if (is_zero128(a)) return tf_zero(0);
    /* The three rounding bits have to come from somewhere: if the value
     * already uses the top of its 128 bits, shifting left would lose
     * the very bits being rounded, so it is shifted RIGHT with a sticky
     * instead and the exponent moves the other way. */
    if (clz128(a) >= 3)
        return round_pack(0, -3, shl128(a, 3));
    return round_pack(0, 0, a);
}

long double __floattitf(s128 a)
{
    union w128 w;
    w.s = a;
    if ((hi64(w.u) >> 63) == 0)
        return __floatuntitf(w.u);
    {
        u64 lo = lo64(w.u), hi = hi64(w.u);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        u128 m = mk(nhi, nlo);
        if (clz128(m) >= 3)
            return round_pack(1, -3, shl128(m, 3));
        return round_pack(1, 0, m);
    }
}

/* Truncation toward zero, as a cast requires. Out of range saturates,
 * for the reason fp128.c gives: it is undefined in C, and an answer
 * that grows with its input is one a reader can follow. */
static u128 to_u128(long double x, int *neg_out)
{
    struct tf a = unpack(x);
    int shift;

    *neg_out = a.sign;
    if (a.cls == TF_NAN || a.cls == TF_ZERO)
        return mk(0, 0);
    if (a.cls == TF_INF)
        return mk(~0ULL, ~0ULL);
    /* value = sig * 2^exp, and sig's leading bit is at 112. */
    shift = a.exp;
    if (shift <= -128)
        return mk(0, 0);
    if (shift < 0)
        return shr128(a.sig, -shift);
    if (shift >= 128 || clz128(a.sig) < shift)
        return mk(~0ULL, ~0ULL);       /* would not fit */
    return shl128(a.sig, shift);
}

u128 __fixunstfti(long double x)
{
    int neg;
    u128 m = to_u128(x, &neg);
    return neg ? mk(0, 0) : m;         /* negative to unsigned is zero */
}

s128 __fixtfti(long double x)
{
    int neg;
    u128 m = to_u128(x, &neg);
    union w128 w;

    if (!neg) {
        if (hi64(m) >> 63)
            return (s128)mk(0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
        w.u = m;
        return w.s;
    }
    /* -2^127 is representable and is the minimum; anything past it
     * saturates there. */
    if (hi64(m) >> 63) {
        if (hi64(m) == 0x8000000000000000ULL && lo64(m) == 0)
            return (s128)mk(0x8000000000000000ULL, 0);
        return (s128)mk(0x8000000000000000ULL, 0);
    }
    {
        u64 lo = lo64(m), hi = hi64(m);
        u64 nlo = ~lo + 1;
        u64 nhi = ~hi + (nlo == 0);
        w.u = mk(nhi, nlo);
        return w.s;
    }
}

u64 __fixunstfdi(long double x)
{
    int neg;
    u128 m = to_u128(x, &neg);
    if (neg) return 0;
    if (!is_zero128(shr128(m, 64))) return ~0ULL;
    return lo64(m);
}

s64 __fixtfdi(long double x)
{
    int neg;
    u128 m = to_u128(x, &neg);
    if (!is_zero128(shr128(m, 63)))
        return neg ? (s64)0x8000000000000000ULL : (s64)0x7FFFFFFFFFFFFFFFULL;
    return neg ? -(s64)lo64(m) : (s64)lo64(m);
}

int __fixtfsi(long double x)
{
    s64 v = __fixtfdi(x);
    if (v > 2147483647LL) return 2147483647;
    if (v < -2147483647LL - 1) return -2147483647 - 1;
    return (int)v;
}

u32 __fixunstfsi(long double x)
{
    u64 v = __fixunstfdi(x);
    return v > 4294967295ULL ? 4294967295U : (u32)v;
}

#else

/* x86-64: `long double` is x87 and the hardware does all of this. */
typedef int embcc_rt_softtf_is_aarch64_only;

#endif
