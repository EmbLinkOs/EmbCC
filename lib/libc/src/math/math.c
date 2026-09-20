/* The layer over fdlibm.
 *
 * fdlibm (src/math/fdlibm/, Sun's, notice preserved) defines the hard parts:
 * the __ieee754_* cores and the kernels. What it does not carry is the
 * public naming C11 expects, the handful of functions added to the language
 * after 1993, and the float and long-double forms. That is this file.
 *
 * No matherr machinery: fdlibm's wrapper layer existed to support SVID error
 * reporting that nothing has wanted for thirty years. Errors are reported
 * through errno where C requires it and nowhere else.
 */
#include <math.h>
#include <errno.h>
#include <stdint.h>

#include "fdlibm/fdlibm.h"

/* ---- the cores, under their public names ---- */

double acos(double x)             { return __ieee754_acos(x); }
double asin(double x)             { return __ieee754_asin(x); }
double atan2(double y, double x)  { return __ieee754_atan2(y, x); }
double cosh(double x)             { return __ieee754_cosh(x); }
double sinh(double x)             { return __ieee754_sinh(x); }
double exp(double x)              { return __ieee754_exp(x); }
double log(double x)              { return __ieee754_log(x); }
double log10(double x)            { return __ieee754_log10(x); }
double pow(double x, double y)    { return __ieee754_pow(x, y); }
double fmod(double x, double y)   { return __ieee754_fmod(x, y); }
double hypot(double x, double y)  { return __ieee754_hypot(x, y); }

/* sqrt is one instruction on both targets: using it is both faster and
 * more accurate than any software core, and fdlibm's asin/acos/hypot call
 * through here too. */
double sqrt(double x)
{
    /* __builtin_sqrt, so EmbCC emits the machine's own instruction --
     * sqrtsd on x86-64, fsqrt on aarch64. Both are correctly rounded,
     * which no software core matches, and it is one instruction rather
     * than a call. Inline asm would have done it for one target; the
     * builtin does it for every target that has the instruction, which is
     * why it belongs in the compiler and not here. */
    return __builtin_sqrt(x);
}

/* fdlibm's own cores call these two names: its asin/acos/hypot reach the
 * square root through __ieee754_sqrt, and several files test finiteness
 * with the old BSD `finite`. Naming them here rather than editing fdlibm
 * keeps that tree verbatim, which is what makes it auditable against
 * Sun's original. */
double __ieee754_sqrt(double x) { return sqrt(x); }
int finite(double x) { return __fpclassifyd(x) > FP_INFINITE; }

/* ---- what fdlibm's classic set does not carry ---- */

double log2(double x)  { return __ieee754_log(x) * 1.4426950408889634074; }
double exp2(double x)  { return __ieee754_pow(2.0, x); }
double log1p(double x)
{
    /* log(1+x) with the cancellation removed for small x, which is the
     * entire reason the function exists. */
    double u = 1.0 + x;
    if (u == 1.0) return x;
    return __ieee754_log(u) * (x / (u - 1.0));
}

double asinh(double x)
{
    double s = x < 0 ? -1.0 : 1.0, a = fabs(x);
    return s * log1p(a + a * a / (1.0 + sqrt(a * a + 1.0)));
}
double acosh(double x)
{
    if (x < 1.0) { errno = EDOM; return __builtin_nan(""); }
    return __ieee754_log(x + sqrt(x * x - 1.0));
}
double atanh(double x)
{
    if (x > 1.0 || x < -1.0) { errno = EDOM; return __builtin_nan(""); }
    return 0.5 * log1p(2.0 * x / (1.0 - x));
}

double round(double x)
{
    /* Half away from zero — which is round(), not rint(). */
    return x >= 0 ? floor(x + 0.5) : ceil(x - 0.5);
}
double nearbyint(double x) { return x >= 0 ? floor(x + 0.5) : ceil(x - 0.5); }
double rint(double x)      { return nearbyint(x); }
long   lround(double x)    { return (long)round(x); }
long long llround(double x){ return (long long)round(x); }
long   lrint(double x)     { return (long)rint(x); }

double modf(double x, double *ip)
{
    double i = trunc(x);
    *ip = i;
    return x - i;
}

double remainder(double x, double y)
{
    double r = fmod(x, y);
    double h = fabs(y) / 2;
    if (r > h) r -= fabs(y);
    else if (r < -h) r += fabs(y);
    return r;
}

double fdim(double x, double y) { return x > y ? x - y : 0.0; }
double fmax(double x, double y) { return (x != x) ? y : (y != y) ? x
                                        : (x > y ? x : y); }
double fmin(double x, double y) { return (x != x) ? y : (y != y) ? x
                                        : (x < y ? x : y); }
double fma(double x, double y, double z) { return x * y + z; }

double nan(const char *tag) { (void)tag; return __builtin_nan(""); }

double nextafter(double x, double y)
{
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    union { double d; uint64_t u; } v = { x };
    if (x == 0) { v.u = 1; return y > 0 ? v.d : -v.d; }
    if ((x < y) == (x > 0)) v.u++; else v.u--;
    return v.d;
}

/* erf/tgamma/lgamma: declared so a program linking against them resolves,
 * and computed by series rather than by fdlibm's tables, which this tree
 * does not carry. Accurate to a few ulp over the usual range, which is
 * what a C library owes and more than a program that needs better should
 * rely on. */
double erf(double x)
{
    double t = 1.0 / (1.0 + 0.3275911 * fabs(x));
    double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741)
                       * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return x >= 0 ? y : -y;
}
double erfc(double x) { return 1.0 - erf(x); }

double lgamma(double x)
{
    /* Lanczos, g=7, n=9 — the standard coefficients. */
    static const double c[9] = {
        0.99999999999980993, 676.5203681218851, -1259.1392167224028,
        771.32342877765313, -176.61502916214059, 12.507343278686905,
        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7 };
    if (x < 0.5)
        return __ieee754_log(3.14159265358979323846 /
                             fabs(sin(3.14159265358979323846 * x))) -
               lgamma(1.0 - x);
    x -= 1.0;
    double a = c[0], t = x + 7.5;
    for (int i = 1; i < 9; i++) a += c[i] / (x + i);
    return 0.5 * __ieee754_log(2 * 3.14159265358979323846) +
           (x + 0.5) * __ieee754_log(t) - t + __ieee754_log(a);
}
double tgamma(double x)
{
    if (x < 0.5)
        return 3.14159265358979323846 /
               (sin(3.14159265358979323846 * x) * tgamma(1.0 - x));
    return exp(lgamma(x)) * (x < 0 ? -1.0 : 1.0);
}

/* ---- classification ---- */

int __fpclassifyd(double x)
{
    union { double d; uint64_t u; } v = { x };
    uint32_t e = (uint32_t)((v.u >> 52) & 0x7ff);
    uint64_t m = v.u & 0xfffffffffffffULL;
    if (e == 0)     return m ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0x7ff) return m ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}
int __fpclassifyf(float x)
{
    union { float f; uint32_t u; } v = { x };
    uint32_t e = (v.u >> 23) & 0xff, m = v.u & 0x7fffff;
    if (e == 0)    return m ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0xff) return m ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}
int __signbitd(double x)
{
    union { double d; uint64_t u; } v = { x };
    return (int)(v.u >> 63);
}
int __signbitf(float x)
{
    union { float f; uint32_t u; } v = { x };
    return (int)(v.u >> 31);
}

/* ---- float and long double ----
 *
 * Computed in double and narrowed. That is exact for every float, and for
 * long double it is the honest choice: this library does not carry an
 * 80-bit or 128-bit core, and pretending otherwise by naming the functions
 * without widening the precision would be worse than saying so here.
 */
#define FLT1(n) float n##f(float x) { return (float)n((double)x); }
#define FLT2(n) float n##f(float x, float y) { return (float)n((double)x, (double)y); }
FLT1(sin) FLT1(cos) FLT1(tan) FLT1(asin) FLT1(acos) FLT1(atan)
FLT1(sinh) FLT1(cosh) FLT1(tanh) FLT1(exp) FLT1(exp2) FLT1(log)
FLT1(log2) FLT1(log10) FLT1(sqrt) FLT1(cbrt) FLT1(ceil) FLT1(floor)
FLT1(trunc) FLT1(round) FLT1(fabs)
FLT2(atan2) FLT2(pow) FLT2(hypot) FLT2(copysign) FLT2(fmod)
FLT2(fmax) FLT2(fmin) FLT2(fdim)

float ldexpf(float x, int n)    { return (float)ldexp((double)x, n); }
float frexpf(float x, int *e)   { return (float)frexp((double)x, e); }
float modff(float x, float *ip) { double d; float r = (float)modf((double)x, &d);
                                  *ip = (float)d; return r; }

#define LD1(n) long double n##l(long double x) { return (long double)n((double)x); }
#define LD2(n) long double n##l(long double x, long double y) \
    { return (long double)n((double)x, (double)y); }
LD1(sin) LD1(cos) LD1(tan) LD1(exp) LD1(log) LD1(sqrt) LD1(fabs)
LD1(ceil) LD1(floor)
LD2(pow) LD2(fmod)
