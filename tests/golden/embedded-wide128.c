/* 128-bit values on a 64-bit machine: __int128 and long double (IEEE
 * binary128) at RV64, each two doublewords. The backend does add, sub,
 * the bitwise operations, multiply, compares and shifts by a known
 * count inline; division, remainder, a variable shift and every
 * binary128 operation are calls into lib/rt.
 *
 * The expected answers are not written down here. The same source is
 * compiled by clang for the same triple and linked against the same
 * runtime, so a disagreement is the compiler's -- in its own lowering or
 * in how it calls the runtime. Every result is printed as bits.
 */
extern void writec(int c);
extern void puts_(const char *s);

typedef __int128 s128;
typedef unsigned __int128 u128;

static void hx(unsigned long v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfUL)]);
}

static void p128(const char *tag, u128 v)
{
    puts_(tag);
    writec(' ');
    hx((unsigned long)(v >> 64));
    writec('_');
    hx((unsigned long)v);
    writec('\n');
}

static void pld(const char *tag, long double v)
{
    union { long double d; u128 u; } b;
    b.d = v;
    p128(tag, b.u);
}

static void pn(const char *tag, long v)
{
    puts_(tag);
    writec(' ');
    hx((unsigned long)v);
    writec('\n');
}

/* Values the optimizer cannot see through. */
static volatile long vlo = -3, vhi = 0x7123456789abcdefL;
static volatile unsigned long vu = 0xfedcba9876543210UL;
static volatile int vi = -7, vsh = 70, vsh2 = 3;
static volatile double vd = 1234.5678;
static volatile float vf = -0.15625f;

static s128 mk(long hi, unsigned long lo)
{
    return (s128)((u128)(unsigned long)hi << 64 | lo);
}

/* Arguments in every place the psABI puts them: a pair in a0:a1 and in
 * an ODD pair (a1:a2, no even rounding for a named one), split across
 * a7 and the stack, and wholly on the stack, 16-aligned. */
__attribute__((noinline)) static s128 args(s128 a, long b, s128 c, s128 d,
                                           int e, s128 f, s128 g)
{
    return a + (s128)b * 3 - c + (d ^ (s128)e) + f * 5 - g;
}

__attribute__((noinline)) static long double largs(long double a, int b,
                                                   long double c, double d,
                                                   long double e,
                                                   long double f)
{
    return a * (long double)b + c / (long double)d - e + f;
}

struct holder { char tag; s128 v; long double d; };

__attribute__((noinline)) static void fill(struct holder *h, s128 v,
                                           long double d)
{
    h->tag = 'h';
    h->v = v * 3;
    h->d = d * d;
}

static int cmps(s128 a, s128 b)
{
    return (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 |
           (a == b) << 4 | (a != b) << 5;
}

static int cmpu(u128 a, u128 b)
{
    return (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 |
           (a == b) << 4 | (a != b) << 5;
}

static int cmpl(long double a, long double b)
{
    return (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 |
           (a == b) << 4 | (a != b) << 5;
}

static s128 sel(int k, s128 a, s128 b) { return k ? a : b; }

/* (float)(long)x: once copies are propagated the narrowing is no
 * instruction, and the conversion reads the 128-bit value as its 64-bit
 * source -- which RV64 took for a widened unsigned int. */
__attribute__((noinline)) static float lf(s128 x) { return (float)(long)x; }
__attribute__((noinline)) static double lg(s128 x) { return (double)(long)x; }

int main(void)
{
    s128 a = mk(vhi, (unsigned long)vlo);         /* both halves busy */
    s128 b = mk(-2, vu);
    u128 ua = (u128)a, ub = (u128)b;
    int n = vsh, m = vsh2;

    p128("add", a + b);
    p128("sub", a - b);
    p128("subb", b - a);
    p128("and", a & b);
    p128("or", a | b);
    p128("xor", a ^ b);
    p128("not", ~a);
    p128("neg", -a);
    p128("neg0", -(s128)0);
    p128("mul", (s128)vlo * b);                   /* no signed overflow */
    p128("mulu", ua * ub);
    p128("mul64", (u128)vu * (u128)vu);           /* the full 64x64 */
    p128("muls64", (s128)vlo * (s128)vhi);
    p128("div", a / b);
    p128("mod", a % b);
    p128("divn", b / (s128)vi);
    p128("modn", b % (s128)vi);
    p128("divu", ua / (ub >> 70));
    p128("modu", ua % (ub >> 70));

    /* shifts: every case of a known count, then the variable ones (left
     * on the unsigned value: a signed overflow would be undefined) */
    p128("shl0", ua << 0);
    p128("shl1", ua << 1);
    p128("shl63", ua << 63);
    p128("shl64", ua << 64);
    p128("shl65", ua << 65);
    p128("shl127", ua << 127);
    p128("sar1", b >> 1);
    p128("sar63", b >> 63);
    p128("sar64", b >> 64);
    p128("sar100", b >> 100);
    p128("sar127", b >> 127);
    p128("shr1", ub >> 1);
    p128("shr64", ub >> 64);
    p128("shr127", ub >> 127);
    p128("shlv", ua << n);
    p128("sarv", b >> n);
    p128("shrv", ub >> n);
    p128("shlv3", ua << m);
    p128("sarv3", b >> m);

    /* widening and narrowing */
    p128("exti", (s128)vi);
    p128("extu", (u128)(unsigned)vi);
    p128("extl", (s128)vlo);
    p128("extul", (u128)vu);
    pn("trl", (long)a);
    pn("tri", (int)b);
    pn("tru", (unsigned)b);
    pn("trs", (short)a);
    pn("trb", (unsigned char)b);
    {
        union { float f; unsigned u; } uf;
        union { double d; unsigned long u; } ud;
        uf.f = lf(a);                       /* low word -3 */
        ud.d = lg(mk(9, 0x1fffffffeUL));   /* bits above 31 */
        pn("lf", (long)uf.u);
        pn("lg", (long)ud.u);
    }

    /* comparisons, against each other and across the halves */
    pn("cmp", cmps(a, b) | cmps(b, a) << 6 | cmps(a, a) << 12 |
              cmps(mk(0, 5), mk(0, 7)) << 18 |
              cmps(mk(1, 0), mk(0, ~0UL)) << 24 |
              cmps(mk(-1, 0), mk(0, 0)) << 30);
    pn("cmpu", cmpu(ua, ub) | cmpu(ub, ua) << 6 |
               cmpu((u128)mk(-1, 0), (u128)mk(0, 0)) << 12 |
               cmpu((u128)mk(0, ~0UL), (u128)mk(1, 0)) << 18);
    /* equal high words: the low words decide, UNSIGNED, whatever the
     * sign of the whole -- bit 63 of a low word is just a digit */
    pn("cmplo", cmps(mk(0, 1), mk(0, 1UL << 63)) |
                cmps(mk(-1, 5), mk(-1, ~0UL)) << 6 |
                cmpu((u128)mk(3, ~0UL), (u128)mk(3, 7)) << 12 |
                cmpu((u128)mk(-1, 1UL << 63), (u128)mk(-1, 0)) << 18);
    pn("bool", (a ? 1 : 0) | (mk(1, 0) ? 2 : 0) | ((s128)0 ? 4 : 0) |
               (!mk(0, 1) ? 8 : 0));
    p128("sel", sel(vi < 0, a, b));
    p128("sel2", sel(vi > 0, a, b));

    /* memory and calls */
    {
        static u128 arr[4];
        struct holder h;
        for (int k = 0; k < 4; k++)
            arr[k] = ua * (u128)(k + 1);
        p128("arr", arr[1] + arr[3]);
        fill(&h, b, (long double)vd);
        p128("fillv", h.v);
        pld("filld", h.d);
        pn("tag", h.tag);
    }
    p128("args", args(a, vlo, b, a, vi, b, a));

    /* long double */
    {
        long double x = (long double)vd, y = (long double)vf;
        long double z = x * 3.25L - y / 7.0L;
        pld("ldc", 3.14159265358979323846264338327950288L);
        pld("ldadd", x + y);
        pld("ldsub", x - y);
        pld("ldmul", x * y);
        pld("lddiv", x / y);
        pld("ldneg", -x);
        pld("ldnegz", -(long double)0);
        pld("ldz", z);
        pld("ldargs", largs(x, vi, y, vd, z, 1.0L / 3.0L));
        pn("ldcmp", cmpl(x, y) | cmpl(y, x) << 6 | cmpl(x, x) << 12);
        {
            long double nan = (long double)vd - (long double)vd;
            nan = nan / nan;
            pn("ldnan", cmpl(nan, x) | cmpl(nan, nan) << 6);
        }
        pld("fromi", (long double)vi);
        pld("fromu", (long double)(unsigned)vi);
        pld("froml", (long double)vlo);
        pld("fromul", (long double)vu);
        pld("fromd", (long double)vd);
        pld("fromf", (long double)vf);
        pld("from128", (long double)a);
        pld("fromu128", (long double)ub);
        pn("toi", (int)(z / 100));
        pn("tou", (unsigned)(x * 1000));
        pn("tol", (long)(-x * 1e12L));
        pn("toul", (unsigned long)(x * 1e15L));
        p128("to128", (s128)(-x * 1e30L));
        p128("tou128", (u128)(x * 1e35L));
        {
            union { double d; unsigned long u; } dd;
            union { float f; unsigned u; } ff;
            dd.d = (double)z;
            ff.f = (float)z;
            pn("tod", (long)dd.u);
            pn("tof", (long)ff.u);
        }
        p128("i2d", (s128)(double)a);
        pn("128tod", (long)(double)(a >> 70));
    }
    puts_("==END==\n");
    return 0;
}
