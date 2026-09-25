/* IEEE-754 binary64 and binary32, in software.
 *
 * ARMv7-M's base profile has no FPU, so `a + b` on two doubles is a call
 * to __adddf3 and this file is what it calls. x86-64 and aarch64 do all
 * of it in hardware, which is why the whole file is guarded: compiling
 * it there would put a second __adddf3 in the archive beside libgcc's
 * for no reason.
 *
 * ---- the rule this file lives under ---------------------------------
 *
 * It may not use the operations it implements. Every routine takes and
 * returns `double` or `float` — it has to, or the ABI would be wrong —
 * but the value is converted to its BITS through a union on the way in
 * and back on the way out, and nothing in between is a floating-point
 * operation. A single `a * b` here would be a call to __muldf3, which
 * is this file.
 *
 * ---- why there is only one core -------------------------------------
 *
 * Only binary64 is implemented. A binary32 operation is done by
 * widening both operands to binary64, doing it there, and rounding the
 * result back — which gives the SAME answer as computing in binary32
 * directly, because binary64 carries 53 significand bits and 53 is at
 * least 2p+2 for p = 24. That is Figueroa's theorem, and it is the
 * reason double rounding cannot go wrong here; it would not hold the
 * other way round, or for a format only a few bits wider.
 */

#if __SIZEOF_POINTER__ < 8

typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

union dbits { double d; u64 u; };
union fbits { float f; u32 u; };

static u64 d2u(double d) { union dbits x; x.d = d; return x.u; }
static double u2d(u64 u) { union dbits x; x.u = u; return x.d; }
static u32 f2u(float f) { union fbits x; x.f = f; return x.u; }
static float u2f(u32 u) { union fbits x; x.u = u; return x.f; }

/* A value taken apart. `sig` carries the significand with its implicit
 * bit at position 55, leaving three low bits for guard, round and
 * sticky — the sum of two such fits in 64 bits with room for a carry,
 * which is what makes the addition below a plain one. */
#define SIGBIT 55
#define EXPMAX 0x7ff
#define BIAS   1023

enum { CLS_NORMAL, CLS_ZERO, CLS_INF, CLS_NAN };

/* Taken apart through a POINTER, and not returned or passed by value.
 * A 24-byte struct travels by memory in AAPCS32 with a hidden pointer,
 * which is an ABI corner the backend that calls this file does not
 * implement yet — and a runtime routine that could not be compiled
 * until the compiler grew a feature would be a circular dependency. */
struct fp { int sign, exp, cls; u64 sig; };

static void unpack(u64 u, struct fp *r_out)
{
    struct fp r;
    int e = (int)((u >> 52) & EXPMAX);
    u64 frac = u & 0xfffffffffffffULL;

    r.sign = (int)(u >> 63);
    r.sig = 0;
    r.exp = 0;
    if (e == 0) {
        if (frac == 0) { r.cls = CLS_ZERO; *r_out = r; return; }
        /* A denormal, normalised on the way in: shift until the
         * implicit bit is where a normal's would be, and charge the
         * exponent for it. Everything downstream then has one case. */
        r.cls = CLS_NORMAL;
        r.exp = 1 - BIAS;
        r.sig = frac << 3;
        while (!(r.sig & ((u64)1 << SIGBIT))) {
            r.sig <<= 1;
            r.exp--;
        }
        *r_out = r;
        return;
    }
    if (e == EXPMAX) {
        r.cls = frac ? CLS_NAN : CLS_INF;
        r.sig = frac;
        *r_out = r;
        return;
    }
    r.cls = CLS_NORMAL;
    r.exp = e - BIAS;
    r.sig = (frac | 0x10000000000000ULL) << 3;
    *r_out = r;
}

static double pack_raw(int sign, u64 e, u64 frac)
{
    return u2d(((u64)(sign & 1) << 63) | (e << 52) | frac);
}

static double d_zero(int s) { return pack_raw(s, 0, 0); }
static double d_inf(int s)  { return pack_raw(s, EXPMAX, 0); }
static double d_nan(void)   { return pack_raw(0, EXPMAX, 0x8000000000000ULL); }

/* Round to nearest, ties to even, and assemble. `sig` has the leading
 * bit at SIGBIT or one above it (a carry out of an addition); `exp` is
 * unbiased and names the weight of SIGBIT. */
static double round_pack(int sign, int exp, u64 sig)
{
    int shift;

    if (sig == 0)
        return d_zero(sign);
    /* Normalise: one bit down for a carry, or up for a borrow. */
    while (sig >= ((u64)1 << (SIGBIT + 1))) {
        /* The bit shifted out must not be lost — it becomes sticky. */
        sig = (sig >> 1) | (sig & 1);
        exp++;
    }
    while (!(sig & ((u64)1 << SIGBIT))) {
        sig <<= 1;
        exp--;
    }

    /* Underflow into the denormal range: shift right instead of letting
     * the exponent go below the format's minimum, keeping a sticky bit
     * so the rounding below still sees what was dropped. */
    if (exp < 1 - BIAS) {
        shift = (1 - BIAS) - exp;
        if (shift > 63)
            return d_zero(sign);        /* rounds to zero either way */
        {
            u64 lost = sig & (((u64)1 << shift) - 1);
            sig = (sig >> shift) | (lost != 0);
        }
        exp = 1 - BIAS;
        /* A denormal result: the biased exponent is 0 and there is no
         * implicit bit, which is what the assembly below produces once
         * the significand has been shifted into place. */
        {
            u64 round = sig & 7;
            sig >>= 3;
            if (round > 4 || (round == 4 && (sig & 1)))
                sig++;
            if (sig & 0x10000000000000ULL)   /* rounded up INTO a normal */
                return pack_raw(sign, 1, sig & 0xfffffffffffffULL);
            return pack_raw(sign, 0, sig);
        }
    }

    {
        u64 round = sig & 7;
        u64 frac = sig >> 3;
        if (round > 4 || (round == 4 && (frac & 1))) {
            frac++;
            if (frac & 0x20000000000000ULL) {   /* carried past 53 bits */
                frac >>= 1;
                exp++;
            }
        }
        if (exp + BIAS >= EXPMAX)
            return d_inf(sign);
        return pack_raw(sign, (u64)(exp + BIAS), frac & 0xfffffffffffffULL);
    }
}

/* A NaN out of an operation, keeping one operand's payload where there
 * is one — quieted, as IEEE asks. */
static double propagate(const struct fp *a, const struct fp *b)
{
    if (a->cls == CLS_NAN)
        return pack_raw(a->sign, EXPMAX, a->sig | 0x8000000000000ULL);
    if (b->cls == CLS_NAN)
        return pack_raw(b->sign, EXPMAX, b->sig | 0x8000000000000ULL);
    return d_nan();
}

/* ---- add and subtract ------------------------------------------- */

static double addsub(double x, double y, int negate_y)
{
    struct fp a, b;

    unpack(d2u(x), &a);
    unpack(d2u(y), &b);
    int shift;

    /* The NaN check comes BEFORE the negation: `a - NaN` propagates
     * that NaN as it was, and flipping its sign first would hand back
     * one the subtraction invented. */
    if (a.cls == CLS_NAN || b.cls == CLS_NAN)
        return propagate(&a, &b);
    if (negate_y)
        b.sign ^= 1;
    if (a.cls == CLS_INF || b.cls == CLS_INF) {
        if (a.cls == CLS_INF && b.cls == CLS_INF && a.sign != b.sign)
            return d_nan();             /* inf - inf */
        return d_inf(a.cls == CLS_INF ? a.sign : b.sign);
    }
    if (a.cls == CLS_ZERO && b.cls == CLS_ZERO)
        /* -0 + -0 is -0; every other pairing is +0, in the default
         * rounding direction. */
        return d_zero(a.sign & b.sign);
    if (a.cls == CLS_ZERO) return u2d(d2u(y) ^ ((u64)negate_y << 63));
    if (b.cls == CLS_ZERO) return x;

    /* Align on the larger exponent, keeping what falls off as sticky. */
    if (a.exp < b.exp) { struct fp t = a; a = b; b = t; }
    shift = a.exp - b.exp;
    if (shift > 63) {
        b.sig = b.sig != 0;             /* everything became sticky */
    } else if (shift > 0) {
        u64 lost = b.sig & (((u64)1 << shift) - 1);
        b.sig = (b.sig >> shift) | (lost != 0);
    }

    if (a.sign == b.sign)
        return round_pack(a.sign, a.exp, a.sig + b.sig);
    /* Exact cancellation is +0, not the larger operand's sign: IEEE
     * says a sum that vanishes is positive in the default rounding
     * direction, so `-1.0 + 1.0` is +0 and not -0. round_pack would
     * take the sign it was handed. */
    if (a.sig == b.sig)
        return d_zero(0);
    if (a.sig > b.sig)
        return round_pack(a.sign, a.exp, a.sig - b.sig);
    return round_pack(b.sign, a.exp, b.sig - a.sig);
}

double __adddf3(double a, double b) { return addsub(a, b, 0); }
double __subdf3(double a, double b) { return addsub(a, b, 1); }

double __negdf2(double a) { return u2d(d2u(a) ^ ((u64)1 << 63)); }

/* ---- multiply ----------------------------------------------------- */

/* 64x64 -> 128, in 32-bit pieces, because this machine has no wider
 * type and `unsigned __int128` does not exist on it. */
static u64 mul64(u64 a, u64 b, u64 *lo_out)
{
    u32 al = (u32)a, ah = (u32)(a >> 32);
    u32 bl = (u32)b, bh = (u32)(b >> 32);
    u64 ll = (u64)al * bl;
    u64 lh = (u64)al * bh;
    u64 hl = (u64)ah * bl;
    u64 hh = (u64)ah * bh;
    u64 mid = (ll >> 32) + (lh & 0xffffffffULL) + (hl & 0xffffffffULL);

    *lo_out = (ll & 0xffffffffULL) | (mid << 32);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

double __muldf3(double x, double y)
{
    struct fp a, b;

    unpack(d2u(x), &a);
    unpack(d2u(y), &b);
    u64 hi, lo;
    int sign;

    if (a.cls == CLS_NAN || b.cls == CLS_NAN)
        return propagate(&a, &b);
    sign = a.sign ^ b.sign;
    if (a.cls == CLS_INF || b.cls == CLS_INF) {
        if (a.cls == CLS_ZERO || b.cls == CLS_ZERO)
            return d_nan();             /* inf * 0 */
        return d_inf(sign);
    }
    if (a.cls == CLS_ZERO || b.cls == CLS_ZERO)
        return d_zero(sign);

    /* Each significand has its leading bit at 55, so the product's is
     * at 111 or 110. Bring it back to 55 and fold everything below into
     * a sticky bit. */
    hi = mul64(a.sig, b.sig, &lo);
    {
        u64 sig = (hi << (64 - 56)) | (lo >> 56);
        u64 rest = lo & ((((u64)1) << 56) - 1);
        sig |= (rest != 0);
        return round_pack(sign, a.exp + b.exp + 1, sig);
    }
}

/* ---- divide -------------------------------------------------------- */

double __divdf3(double x, double y)
{
    struct fp a, b;

    unpack(d2u(x), &a);
    unpack(d2u(y), &b);
    int sign;

    if (a.cls == CLS_NAN || b.cls == CLS_NAN)
        return propagate(&a, &b);
    sign = a.sign ^ b.sign;
    if (a.cls == CLS_INF) {
        if (b.cls == CLS_INF) return d_nan();
        return d_inf(sign);
    }
    if (b.cls == CLS_INF) return d_zero(sign);
    if (a.cls == CLS_ZERO) {
        if (b.cls == CLS_ZERO) return d_nan();
        return d_zero(sign);
    }
    if (b.cls == CLS_ZERO) return d_inf(sign);

    /* Restoring division, one quotient bit at a time, most significant
     * first: 57 of them, so the result has its leading bit at 55 or 56
     * and round_pack sees a guard, a round and a sticky below it. */
    {
        u64 rem = a.sig, q = 0;
        int i;
        for (i = 56; i >= 0; i--) {
            if (rem >= b.sig) {
                rem -= b.sig;
                q |= (u64)1 << i;
            }
            /* rem < b.sig < 2^56, so this cannot overflow. */
            rem <<= 1;
        }
        q |= (rem != 0);                /* the sticky bit */
        return round_pack(sign, a.exp - b.exp - 1, q);
    }
}

/* ---- comparison ----------------------------------------------------
 *
 * libgcc's shape: the sign of the answer decides. Each returns the
 * value that makes ITS predicate false when either operand is a NaN,
 * which is why there are six of them and not one.
 */
static int compare(double x, double y, int nan_result)
{
    struct fp a, b;

    unpack(d2u(x), &a);
    unpack(d2u(y), &b);
    u64 ua, ub;

    if (a.cls == CLS_NAN || b.cls == CLS_NAN)
        return nan_result;
    if (a.cls == CLS_ZERO && b.cls == CLS_ZERO)
        return 0;                       /* -0 == +0 */
    ua = d2u(x);
    ub = d2u(y);
    if (a.sign != b.sign)
        return a.sign ? -1 : 1;
    /* Same sign: the bit patterns order the same way as the values for
     * positives, and the other way round for negatives. */
    if (ua == ub) return 0;
    if (a.sign)
        return ua > ub ? -1 : 1;
    return ua > ub ? 1 : -1;
}

int __eqdf2(double a, double b) { return compare(a, b, 1); }
int __nedf2(double a, double b) { return compare(a, b, 1); }
int __ltdf2(double a, double b) { return compare(a, b, 1); }
int __ledf2(double a, double b) { return compare(a, b, 1); }
int __gtdf2(double a, double b) { return compare(a, b, -1); }
int __gedf2(double a, double b) { return compare(a, b, -1); }
int __unorddf2(double a, double b)
{
    struct fp x, y;
    unpack(d2u(a), &x);
    unpack(d2u(b), &y);
    return x.cls == CLS_NAN || y.cls == CLS_NAN;
}

/* ---- conversions: integer to floating ------------------------------ */

static double from_u64(u64 v, int sign)
{
    int exp = SIGBIT;
    u64 sig = v;

    if (v == 0)
        return d_zero(0);
    /* Bring the value's top bit to SIGBIT, folding anything shifted
     * out into a sticky bit so the rounding is right for a 64-bit
     * integer that does not fit 53 bits. */
    while (sig >= ((u64)1 << (SIGBIT + 1))) {
        sig = (sig >> 1) | (sig & 1);
        exp++;
    }
    while (!(sig & ((u64)1 << SIGBIT))) {
        sig <<= 1;
        exp--;
    }
    return round_pack(sign, exp, sig);
}

double __floatsidf(int v)
{
    return v < 0 ? from_u64((u64)-(s64)v, 1) : from_u64((u64)v, 0);
}
double __floatunsidf(unsigned v) { return from_u64((u64)v, 0); }
double __floatdidf(s64 v)
{
    return v < 0 ? from_u64((u64)-v, 1) : from_u64((u64)v, 0);
}
double __floatundidf(u64 v) { return from_u64(v, 0); }

/* ---- conversions: floating to integer ------------------------------
 *
 * Truncating toward zero, as C requires. Out of range is undefined in
 * C; the saturating answer is given rather than a trap, because a trap
 * is not available here.
 */
static u64 to_u64(double x, int *sign_out)
{
    struct fp a;
    u64 sig;
    int shift;

    unpack(d2u(x), &a);
    *sign_out = a.sign;
    if (a.cls == CLS_NAN || a.cls == CLS_ZERO)
        return 0;
    if (a.cls == CLS_INF)
        return ~(u64)0;
    if (a.exp < 0)
        return 0;                       /* |x| < 1 truncates to zero */
    if (a.exp > 63)
        return ~(u64)0;
    sig = a.sig;
    shift = SIGBIT - a.exp;
    if (shift > 0)
        return shift > 63 ? 0 : (sig >> shift);
    return sig << (-shift);
}

s64 __fixdfdi(double x)
{
    int sign;
    u64 v = to_u64(x, &sign);
    if (sign)
        return v > (u64)1 << 63 ? -(s64)((u64)1 << 63) : -(s64)v;
    return v > (((u64)1 << 63) - 1) ? (s64)(((u64)1 << 63) - 1) : (s64)v;
}
u64 __fixunsdfdi(double x)
{
    int sign;
    u64 v = to_u64(x, &sign);
    return sign ? 0 : v;
}
int __fixdfsi(double x)
{
    s64 v = __fixdfdi(x);
    if (v > 2147483647LL) return 2147483647;
    if (v < -2147483647LL - 1) return -2147483647 - 1;
    return (int)v;
}
unsigned __fixunsdfsi(double x)
{
    u64 v = __fixunsdfdi(x);
    return v > 4294967295ULL ? 4294967295u : (unsigned)v;
}

/* ---- binary32 ------------------------------------------------------
 *
 * Widening is exact, so the only rounding is the one on the way back.
 */

float __truncdfsf2(double x)
{
    struct fp a;
    int exp;
    u64 sig;
    u32 frac;

    unpack(d2u(x), &a);
    if (a.cls == CLS_NAN)
        /* The sign of a NaN means nothing to the arithmetic, and every
         * other implementation still carries it across a narrowing —
         * so this does too, rather than leaving a difference that looks
         * like a bug to anyone comparing bit patterns. */
        return u2f(((u32)a.sign << 31) | 0x7fc00000u |
                   ((u32)(d2u(x) >> 29) & 0x3fffffu));
    if (a.cls == CLS_INF)  return u2f(((u32)a.sign << 31) | 0x7f800000u);
    if (a.cls == CLS_ZERO) return u2f((u32)a.sign << 31);

    /* Round the 53-bit significand to 24, by the same rule, with the
     * bits below folded into a sticky. */
    exp = a.exp;
    sig = a.sig >> 3;                   /* the significand, bit 52 leading */
    if (exp < -126) {                   /* a binary32 denormal, or zero */
        int shift = -126 - exp;
        if (shift > 60)
            return u2f((u32)a.sign << 31);
        {
            u64 lost = sig & (((u64)1 << shift) - 1);
            sig = (sig >> shift) | (lost != 0);
        }
        exp = -126;
        {
            u64 keep = sig >> 29, rest = sig & 0x1fffffffULL;
            if (rest > 0x10000000ULL ||
                (rest == 0x10000000ULL && (keep & 1)))
                keep++;
            if (keep & 0x800000ULL)
                return u2f(((u32)a.sign << 31) | (1u << 23) |
                           ((u32)keep & 0x7fffffu));
            return u2f(((u32)a.sign << 31) | (u32)keep);
        }
    }
    {
        u64 keep = sig >> 29, rest = sig & 0x1fffffffULL;
        if (rest > 0x10000000ULL || (rest == 0x10000000ULL && (keep & 1))) {
            keep++;
            if (keep & 0x1000000ULL) { keep >>= 1; exp++; }
        }
        if (exp > 127)
            return u2f(((u32)a.sign << 31) | 0x7f800000u);
        frac = (u32)keep & 0x7fffffu;
        return u2f(((u32)a.sign << 31) | ((u32)(exp + 127) << 23) | frac);
    }
}

double __extendsfdf2(float f)
{
    u32 u = f2u(f);
    int sign = (int)(u >> 31);
    int e = (int)((u >> 23) & 0xff);
    u32 frac = u & 0x7fffffu;

    if (e == 0xff) {
        if (frac == 0) return d_inf(sign);
        return pack_raw(sign, EXPMAX, ((u64)frac << 29) |
                                      0x8000000000000ULL);
    }
    if (e == 0) {
        if (frac == 0) return d_zero(sign);
        /* A binary32 denormal is a perfectly ordinary binary64 number;
         * round_pack normalises it.
         *
         * The exponent is -126 and nothing else. Shifting frac left by
         * SIGBIT - 23 puts its implicit-bit POSITION (bit 23, which is
         * zero in a denormal) at SIGBIT, and that position's weight in
         * a binary32 denormal is 2^-126 — the same as the smallest
         * normal's leading bit. Deriving it from SIGBIT instead gave
         * -94 and turned every denormal into a number 2^32 too large. */
        return round_pack(sign, -126, (u64)frac << (SIGBIT - 23));
    }
    return pack_raw(sign, (u64)(e - 127 + BIAS), (u64)frac << 29);
}

float __addsf3(float a, float b)
{ return __truncdfsf2(__adddf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __subsf3(float a, float b)
{ return __truncdfsf2(__subdf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __mulsf3(float a, float b)
{ return __truncdfsf2(__muldf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __divsf3(float a, float b)
{ return __truncdfsf2(__divdf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __negsf2(float a) { return u2f(f2u(a) ^ 0x80000000u); }

int __eqsf2(float a, float b) { return __eqdf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __nesf2(float a, float b) { return __nedf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __ltsf2(float a, float b) { return __ltdf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __lesf2(float a, float b) { return __ledf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __gtsf2(float a, float b) { return __gtdf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __gesf2(float a, float b) { return __gedf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __unordsf2(float a, float b)
{ return __unorddf2(__extendsfdf2(a), __extendsfdf2(b)); }

float __floatsisf(int v)        { return __truncdfsf2(__floatsidf(v)); }
float __floatunsisf(unsigned v) { return __truncdfsf2(__floatunsidf(v)); }
float __floatdisf(s64 v)        { return __truncdfsf2(__floatdidf(v)); }
float __floatundisf(u64 v)      { return __truncdfsf2(__floatundidf(v)); }

int __fixsfsi(float f)        { return __fixdfsi(__extendsfdf2(f)); }
unsigned __fixunssfsi(float f) { return __fixunsdfsi(__extendsfdf2(f)); }
s64 __fixsfdi(float f)        { return __fixdfdi(__extendsfdf2(f)); }
u64 __fixunssfdi(float f)     { return __fixunsdfdi(__extendsfdf2(f)); }

#else
/* A translation unit needs a declaration, and this one has none to make
 * on a machine whose hardware does all of it. */
typedef int rt_softfp_not_needed_here;
#endif
