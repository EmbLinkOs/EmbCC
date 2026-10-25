// expect-exit: 42
/* Conversions back to exactly what an exact widening started from are
 * the value itself (src/opt/fold.c): (float)(double)x, (int)(double)i,
 * (short)(float)s. And the near misses, which round on the way and must
 * keep doing so: an int above 2^24 through a float, a long long above
 * 2^53 through a double, a double through a float and back. Each at the
 * values that tell a fold from a conversion. */

#include <float.h>
#include <limits.h>

#define NOINLINE __attribute__((noinline))

NOINLINE static float f_d_f(float x) { return (float)(double)x; }
NOINLINE static float f_ld_f(float x) { return (float)(long double)x; }
NOINLINE static double d_ld_d(double x) { return (double)(long double)x; }
NOINLINE static int i_d_i(int i) { return (int)(double)i; }
NOINLINE static unsigned u_d_u(unsigned u) { return (unsigned)(double)u; }
NOINLINE static short s_f_s(short s) { return (short)(float)s; }
/* the near misses */
NOINLINE static int i_f_i(int i) { return (int)(float)i; }
NOINLINE static long long ll_d_ll(long long v) { return (long long)(double)v; }
NOINLINE static double d_f_d(double x) { return (double)(float)x; }
NOINLINE static unsigned i_d_u(int i) { return (unsigned)(double)(i & 0x7fffffff); }

static int same_bits(float a, float b)
{
    union { float f; unsigned u; } x = { a }, y = { b };
    return x.u == y.u;
}

int main(void)
{
    static const float fs[] = { 0.0f, -0.0f, 1.5f, -3.25f, FLT_MAX, -FLT_MAX,
                                FLT_MIN, FLT_MIN / 8, FLT_EPSILON, 1e30f };
    for (unsigned k = 0; k < sizeof fs / sizeof fs[0]; k++) {
        if (!same_bits(f_d_f(fs[k]), fs[k])) return 1;
        if (!same_bits(f_ld_f(fs[k]), fs[k])) return 2;
    }
    volatile float zero = 0.0f;
    float inf = 1.0f / zero, nan = zero / zero;
    if (f_d_f(inf) != inf || f_d_f(nan) == f_d_f(nan)) return 3;
    if (d_ld_d(DBL_MAX) != DBL_MAX || d_ld_d(-DBL_MIN) != -DBL_MIN) return 4;
    static const int is[] = { 0, 1, -1, 123456789, INT_MAX, INT_MIN, 16777217 };
    for (unsigned k = 0; k < sizeof is / sizeof is[0]; k++)
        if (i_d_i(is[k]) != is[k]) return 5;
    if (u_d_u(UINT_MAX) != UINT_MAX || u_d_u(0x80000001u) != 0x80000001u) return 6;
    if (s_f_s(SHRT_MIN) != SHRT_MIN || s_f_s(SHRT_MAX) != SHRT_MAX) return 7;
    /* rounded, as written */
    if (i_f_i(16777217) != 16777216) return 8;
    if (ll_d_ll(9007199254740993LL) != 9007199254740992LL) return 9;
    if (d_f_d(0.1) == 0.1 || d_f_d(0.1) != (double)0.1f) return 10;
    if (i_d_u(-1) != 0x7fffffffu) return 11;
    return 42;
}
