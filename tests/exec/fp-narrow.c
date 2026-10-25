// expect-exit: 42
/* Arithmetic in a wider format than it needs is done in the narrow one
 * (src/opt/fold.c, fold_narrow): `x * 0.5` on a float x multiplies in
 * float, as rounding the double product to float would answer anyway;
 * `x > 0.5` compares in float. Each narrowed form here is checked BIT FOR
 * BIT against the same arithmetic forced into the wide format, through a
 * volatile the optimizer cannot see past -- over a run of pseudo-random
 * floats and the special values. A constant the float format does not
 * hold (0.1) must not be narrowed: x > 0.1 and x > 0.1f differ at
 * x = 0.1f. */

#include <float.h>

#define NOINLINE __attribute__((noinline))

static unsigned fbits(float f) { union { float f; unsigned u; } x = { f }; return x.u; }
static float ffrom(unsigned u) { union { unsigned u; float f; } x = { u }; return x.f; }

NOINLINE static float add_d(float a, float b) { return (double)a + (double)b; }
NOINLINE static float sub_d(float a, float b) { return (double)a - b; }
NOINLINE static float mul_d(float a, float b) { return a * (double)b; }
NOINLINE static float div_d(float a, float b) { return (double)a / (double)b; }
NOINLINE static float mul_half(float a) { return a * 0.5; }
NOINLINE static float add_tenth(float a) { return a + 0.1; }     /* not narrowed */
NOINLINE static int gt_half(float a) { return a > 0.5; }
NOINLINE static int gt_tenth(float a) { return a > 0.1; }         /* not narrowed */
NOINLINE static int lt_ff(float a, float b) { return (double)a < (double)b; }
NOINLINE static float add_ld(float a, float b) { return (long double)a + b; }
NOINLINE static double mul_ld(double a, double b) { return (long double)a * b; }

/* the references, in the wide format for certain */
static volatile double vd;
static volatile long double vl;
static float r_add(float a, float b) { vd = a; return (float)(vd + (double)b); }
static float r_sub(float a, float b) { vd = a; return (float)(vd - (double)b); }
static float r_mul(float a, float b) { vd = a; return (float)(vd * (double)b); }
static float r_div(float a, float b) { vd = a; return (float)(vd / (double)b); }
static float r_half(float a) { vd = a; return (float)(vd * 0.5); }
static float r_tenth(float a) { vd = a; return (float)(vd + 0.1); }
static int r_gt(float a, double k) { vd = a; return vd > k; }
static float r_add_ld(float a, float b) { vl = a; return (float)(vl + b); }
static double r_mul_ld(double a, double b) { vl = a; return (double)(vl * b); }

static int same(float x, float y)
{
    return fbits(x) == fbits(y) || (x != x && y != y);    /* any NaN */
}

int main(void)
{
    /* nothing to narrow where a double is a float (AVR's, RX's binary32) */
    if (DBL_MANT_DIG == FLT_MANT_DIG)
        return 42;
    static const float special[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 0.1f,
        3.0f, FLT_MAX, -FLT_MAX, FLT_MIN, FLT_MIN / 4, FLT_EPSILON,
        1.0f / 3.0f, 16777217.0f, 1e-30f, 7e30f };
    float v[64];
    unsigned seed = 12345;
    unsigned n = 0;
    for (; n < sizeof special / sizeof special[0]; n++)
        v[n] = special[n];
    while (n < 64) {
        seed = seed * 1664525u + 1013904223u;
        float f = ffrom(seed & 0xbfffffffu);    /* below 2^65 in magnitude */
        if (f == f)
            v[n++] = f;
    }
    volatile float zero = 0.0f;
    float inf = 1.0f / zero, nan = zero / zero;
    v[62] = inf;
    v[63] = nan;
    for (unsigned i = 0; i < 64; i++) {
        float a = v[i];
        if (!same(mul_half(a), r_half(a))) return 1;
        if (!same(add_tenth(a), r_tenth(a))) return 2;
        if (gt_half(a) != r_gt(a, 0.5)) return 3;
        if (gt_tenth(a) != r_gt(a, 0.1)) return 4;
        for (unsigned j = 0; j < 64; j += 3) {
            float b = v[j];
            if (!same(add_d(a, b), r_add(a, b))) return 5;
            if (!same(sub_d(a, b), r_sub(a, b))) return 6;
            if (!same(mul_d(a, b), r_mul(a, b))) return 7;
            if (!same(div_d(a, b), r_div(a, b))) return 8;
            if (lt_ff(a, b) != (a < b)) return 9;
            if (!same(add_ld(a, b), r_add_ld(a, b))) return 10;
        }
    }
    if (gt_tenth(0.1f) != 1) return 11;       /* 0.1f > 0.1 */
    double ds[] = { 1.0, 1.0 / 3.0, DBL_MAX, DBL_MIN, 1e-300, 3.0, -7.5e200 };
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = 0; j < 7; j++) {
            double x = mul_ld(ds[i], ds[j]), y = r_mul_ld(ds[i], ds[j]);
            if (x != y && !(x != x && y != y)) return 12;
        }
    return 42;
}
