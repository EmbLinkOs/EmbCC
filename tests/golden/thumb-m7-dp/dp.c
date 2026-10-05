/* `double` on the Cortex-M7's FPU (FPv5-D16), RUN, and compared with the
 * host bit for bit.
 *
 * Every result is printed as its bit pattern: an answer one ulp off is a
 * wrong answer, and "%f" would hide it. The one exception is a NaN produced
 * by arithmetic, whose payload is the machine's choice when two NaNs meet
 * (or when one is made from nothing), so those print as a fixed word -- but
 * negation, fabs and copysign are bit operations on the sign alone, and
 * they print a NaN's bits exactly.
 *
 * The shapes are the ones the register class and the calling convention
 * have to get right, not only the arithmetic: more doubles live at once
 * than d8-d15 hold, doubles live across calls to a function that uses d8
 * itself, recursion, a double read narrower than itself, arguments that
 * back-fill a hole a double left, nine doubles where eight fit in d0-d7,
 * a homogeneous struct of doubles in and out, and a variadic call, which
 * passes doubles in core registers even under the hard-float convention.
 */
#include <stdarg.h>

void writec(int c);
void puts_(const char *s);

typedef unsigned long long u64;
typedef unsigned u32;

static void hx(u64 v, int n)
{
    for (int i = (n - 1) * 4; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfULL)]);
    writec(' ');
}
static u64 db(double d) { union { double d; u64 u; } x; x.d = d; return x.u; }
static double bd(u64 u) { union { double d; u64 u; } x; x.u = u; return x.d; }
static u32 fb(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }
static float bf(u32 u) { union { float f; u32 u; } x; x.u = u; return x.f; }
/* a NaN out of arithmetic: its payload is not the point */
static u64 canon(double d)
{
    u64 u = db(d);
    return (u & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
           (u & 0xfffffffffffffULL) ? 0x7ff8dead00000000ULL : u;
}
static void hd(double d) { hx(canon(d), 16); }
static void hdx(double d) { hx(db(d), 16); }
static void hf(float f)
{
    u32 u = fb(f);
    hx((u & 0x7f800000u) == 0x7f800000u && (u & 0x7fffffu) ? 0x7fc0deadu : u,
       8);
}
static void nl(void) { writec('\n'); }

volatile u64 vals[] = {
    0x3ff0000000000000ULL, /* 1 */           0xbff0000000000000ULL, /* -1 */
    0x0000000000000000ULL, /* +0 */          0x8000000000000000ULL, /* -0 */
    0x7ff0000000000000ULL, /* inf */         0xfff0000000000000ULL, /* -inf */
    0x7ff8000000000000ULL, /* NaN */         0x400921fb54442d18ULL, /* pi */
    0x0000000000000001ULL, /* least denormal */
    0x000fffffffffffffULL, /* greatest denormal */
    0x0010000000000000ULL, /* least normal */
    0x7fefffffffffffffULL, /* greatest */
    0x3fb999999999999aULL, /* 0.1 */         0x3fd5555555555555ULL, /* 1/3 */
    0x4340000000000001ULL, /* 2^53 + 2 */    0xc1e0000000000000ULL, /* -2^31 */
};
#define NV 16

/* ---- arithmetic, every pair ---------------------------------------- */
__attribute__((noinline)) static void arith(void)
{
    u64 h[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV; j++) {
            double a = bd(vals[i]), b = bd(vals[j]);
            h[0] = h[0] * 1000003u + canon(a + b);
            h[1] = h[1] * 1000003u + canon(a - b);
            h[2] = h[2] * 1000003u + canon(a * b);
            h[3] = h[3] * 1000003u + canon(a / b);
        }
    for (int k = 0; k < 4; k++)
        hx(h[k], 16);
    nl();
    /* and a few by value, so a failure names its operation */
    {
        double p = bd(vals[7]), t = bd(vals[12]), th = bd(vals[13]);
        hd(p + t); hd(p - th); hd(p * t); hd(p / th); hd(t * t * t - th);
        hd(bd(vals[9]) + bd(vals[8]));        /* denormal + denormal */
        hd(bd(vals[10]) / 4.0);               /* normal -> denormal */
        hd(bd(vals[11]) * 2.0);               /* overflow */
        nl();
    }
}

/* ---- comparisons, as values and as branches ------------------------ */
static u32 as_values(double a, double b)
{
    return (u32)(a < b) | (u32)(a <= b) << 1 | (u32)(a > b) << 2 |
           (u32)(a >= b) << 3 | (u32)(a == b) << 4 | (u32)(a != b) << 5;
}
static u32 as_branches(double a, double b)
{
    u32 r = 0;
    if (a < b)  r |= 1;
    if (a <= b) r |= 2;
    if (a > b)  r |= 4;
    if (a >= b) r |= 8;
    if (a == b) r |= 16;
    if (a != b) r |= 32;
    if (!(a < b))  r |= 64;          /* the inverse, which NaN makes true */
    if (!(a >= b)) r |= 128;
    return r;
}
__attribute__((noinline)) static void compares(void)
{
    u32 h = 0, g = 0;
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV; j++) {
            double a = bd(vals[i]), b = bd(vals[j]);
            h = h * 31 + as_values(a, b);
            g = g * 31 + as_branches(a, b);
        }
    hx(h, 8); hx(g, 8);
    {
        double x = bd(vals[7]);       /* against constants */
        hx((u32)(x > 3.0) | (u32)(x < 4.0) << 1 | (u32)(x == 0.0) << 2 |
           (u32)(x != 0.5) << 3 | (u32)(-x <= -3.25) << 4, 2);
    }
    nl();
}

/* ---- conversions ---------------------------------------------------- */
volatile int iv[] = { 0, 1, -1, 7, -7, 2147483647, -2147483647 - 1,
                      16777217, 123456789, -987654321 };
volatile u32 uv[] = { 0u, 1u, 2147483648u, 4294967295u, 3000000000u };
volatile u64 cvd[] = { 0x3ffccccccccccccdULL, /* 1.8 */
                       0xc005999999999999ULL, /* -2.7 */
                       0x3fdfffffffffffffULL, /* just under 0.5 */
                       0x41dfffffffc00000ULL, /* 2^31 - 1 */
                       0xc1dfffffffc00000ULL, /* -(2^31 - 1) */
                       0x41e65a0bc0000000ULL, /* 3e9 */
                       0x3ff0000010000000ULL, /* 1 + 2^-24: a tie as float */
                       0x3ff0000030000000ULL, /* 1 + 3*2^-24: ties up */
                       0x380fffffe0000000ULL, /* a float denormal, rounded */
                       0x47efffffefffffffULL, /* just under float's max */
                       0x47effffff0000000ULL, /* rounds to float inf */
                       0x7ff8000000000000ULL, 0xfff0000000000000ULL,
                       0x8000000000000000ULL };
volatile u32 cvf[] = { 0x3f800000u, 0x00000001u /* float denormal */,
                       0x7f7fffffu, 0xff800000u, 0x7fc00000u, 0x80000000u,
                       0x3eaaaaabu };
__attribute__((noinline)) static void conversions(void)
{
    for (int i = 0; i < 10; i++) hdx((double)iv[i]);
    for (int i = 0; i < 5; i++) hdx((double)uv[i]);
    nl();
    for (int i = 0; i < 14; i++) {
        double d = bd(cvd[i]);
        hf((float)d);
        /* only in range: an out-of-range conversion is undefined, and
         * x86 and ARM answer it differently */
        if (d > -2147483648.0 && d < 2147483648.0 && d == d)
            hx((u32)(int)d, 8);
        if (d >= 0 && d < 4294967296.0)
            hx((unsigned)d, 8);
    }
    nl();
    for (int i = 0; i < 7; i++) hd((double)bf(cvf[i]));
    nl();
    /* the 64-bit conversions are calls, and live doubles must survive
     * them: a, b and c are in d registers across each */
    {
        double a = bd(vals[7]), b = bd(vals[13]), c = 0;
        for (int i = 0; i < 4; i++) {
            long long q = (long long)(a * 1e15) + i;
            unsigned long long uq = (unsigned long long)(b * 1e18);
            c += (double)q - (double)(long long)uq + (double)(uq >> 3);
            hx((u64)q, 16);
        }
        hd(a); hd(b); hd(c);
    }
    nl();
}

/* ---- the sign, square root and selection ---------------------------- */
__attribute__((noinline)) static void signs(void)
{
    for (int i = 0; i < NV; i++) {
        double x = bd(vals[i]);
        hdx(-x);
        hdx(__builtin_fabs(x));
        hdx(__builtin_copysign(1.5, x));
        hd(__builtin_sqrt(x));
    }
    nl();
    for (int i = 0; i + 1 < NV; i++) {
        double a = bd(vals[i]), b = bd(vals[i + 1]);
        double lo = a < b ? a : b, hi = a > b ? a : b;
        hdx(lo); hdx(hi);
        hdx(iv[i % 10] & 1 ? a : -b);
    }
    nl();
}

/* ---- more doubles than registers ------------------------------------ */
/* Twelve partial results live at once, so some must be spilled: d8-d15
 * is eight registers. Each is used twice after all are made, which a
 * register allocator cannot satisfy by recomputing. */
__attribute__((noinline)) static double pressure(const double *x)
{
    double a0 = x[0] * x[1], a1 = x[1] * x[2], a2 = x[2] * x[3],
           a3 = x[3] * x[4], a4 = x[4] * x[5], a5 = x[5] * x[6],
           a6 = x[6] * x[7], a7 = x[7] * x[8], a8 = x[8] * x[9],
           a9 = x[9] * x[10], a10 = x[10] * x[11], a11 = x[11] * x[0];
    double s = a0 - a1 + a2 - a3 + a4 - a5 + a6 - a7 + a8 - a9 + a10 - a11;
    double t = a0 * a11 + a1 * a10 + a2 * a9 + a3 * a8 + a4 * a7 + a5 * a6;
    return s / t + (a0 + a1 + a2 + a3 + a4 + a5) * (a6 + a7 + a8 + a9 + a10 + a11);
}

/* ---- across calls ---------------------------------------------------- */
/* A callee that keeps doubles in d8 and up itself: if it did not save
 * them, or the caller kept its own in d0-d7 across the call, the caller's
 * values change under it. */
__attribute__((noinline)) double clobber(double a, double b)
{
    double x = a * b, y = a + b, z = a - b, w = a / b;
    for (int i = 0; i < 3; i++) {
        x = x * y - z;
        y = y * w + x;
    }
    return x + y + z + w;
}
__attribute__((noinline)) static double across(double p, double q)
{
    double u = p * 3.0, v = q + 0.25, w = p - q;
    double r = clobber(u, v);
    r += clobber(v, w) * u;
    return r + u + v + w;               /* u, v and w survived two calls */
}
__attribute__((noinline)) static double fact(int n, double acc)
{
    return n <= 1 ? acc : fact(n - 1, acc * n) + 0.5;
}

/* ---- the calling convention ----------------------------------------- */
struct v3 { double x, y, z; };         /* homogeneous: d0-d2 under hard */
__attribute__((noinline)) static struct v3 cross(struct v3 a, struct v3 b)
{
    struct v3 r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}
/* float, double, float: the second float back-fills s1, the hole the
 * double's alignment left; and nine doubles where d0-d7 hold eight. */
__attribute__((noinline)) static double backfill(float a, double b, float c,
                                                 double d, int k, double e)
{
    return (double)a * b + (double)c * d + k * e;
}
__attribute__((noinline)) static double nine(double a, double b, double c,
                                             double d, double e, double f,
                                             double g, double h, double i)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}
__attribute__((noinline)) static double vsum(int n, ...)
{
    va_list ap;
    double s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        s = s * 1.25 + va_arg(ap, double);
    va_end(ap);
    return s;
}

/* ---- memory ----------------------------------------------------------- */
struct pt { int tag; double w; double xy[2]; };
double gd = 2.75;
__attribute__((noinline)) static double memory(struct pt *p, double *y,
                                               const double *x, int n)
{
    double dot = 0;
    for (int i = 0; i < n; i++) {
        y[i] = y[i] + p->w * x[i];             /* axpy, in place */
        dot += x[i] * y[i];
    }
    p->xy[0] = dot;
    p->xy[1] = dot * gd;
    gd = gd + 1.0;
    return p->xy[0] - p->xy[1] + p->w;
}

/* A double read a word at a time, as fdlibm's GET_HIGH_WORD does: the
 * value is made in a d register and read through a union. */
__attribute__((noinline)) static u32 high_word(double a, double b)
{
    union { double d; u32 w[2]; } u;
    u.d = a * b;
    return u.w[1] ^ (u.w[0] >> 7);
}

int main(void)
{
    arith();
    compares();
    conversions();
    signs();
    {
        double x[12];
        for (int i = 0; i < 12; i++)
            x[i] = bd(vals[(i * 5 + 7) % NV]) * 0.0 + (i + 1) * 0.375;
        hd(pressure(x));
        x[3] = bd(vals[13]);
        hd(pressure(x));
        nl();
    }
    hd(across(bd(vals[7]), bd(vals[12])));
    hd(across(-2.5, 1e-3));
    hd(fact(12, 1.0));
    nl();
    {
        struct v3 a = { 1.5, -2.0, 0.25 }, b = { 3.0, 0.5, -4.0 }, c;
        c = cross(a, b);
        hd(c.x); hd(c.y); hd(c.z);
        c = cross(c, a);
        hd(c.x); hd(c.y); hd(c.z);
        hd(backfill(1.5f, bd(vals[7]), -0.75f, bd(vals[13]), 3, 2.5));
        hd(nine(1, 2, 3, 4, 5, 6, 7, 8, bd(vals[12])));
        hd(vsum(5, 1.0, bd(vals[7]), -3.5, bd(vals[13]), 1e10));
        nl();
    }
    {
        struct pt p = { 7, 1.5, { 0, 0 } };
        double y[5] = { 1, 2, 3, 4, 5 }, x[5] = { 0.5, -1, 2.25, 8, -0.125 };
        hd(memory(&p, y, x, 5));
        for (int i = 0; i < 5; i++) hd(y[i]);
        hd(p.xy[0]); hd(p.xy[1]); hd(gd);
        hx(high_word(bd(vals[7]), bd(vals[13])), 8);
        hx(high_word(-3.0, bd(vals[9])), 8);
        nl();
    }
    puts_("==END==\n");
    return 0;
}
