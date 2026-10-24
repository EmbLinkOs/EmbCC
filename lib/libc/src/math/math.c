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
#include <fenv.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include "fdlibm/fdlibm.h"

/* Where double is binary32 (RX), fdlibm's double cores are compiled out
 * (fdlibm.h's _DOUBLE_IS_32BITS) and the float cores serve both widths:
 * the double names below, and every use in this file, are theirs. The
 * sf_ files define the public double functions they carry (floor, sin,
 * ...) themselves. */
#ifdef _DOUBLE_IS_32BITS
#define __ieee754_acos(x)     __ieee754_acosf(x)
#define __ieee754_asin(x)     __ieee754_asinf(x)
#define __ieee754_atan2(y, x) __ieee754_atan2f(y, x)
#define __ieee754_cosh(x)     __ieee754_coshf(x)
#define __ieee754_sinh(x)     __ieee754_sinhf(x)
#define __ieee754_exp(x)      __ieee754_expf(x)
#define __ieee754_log(x)      __ieee754_logf(x)
#define __ieee754_log10(x)    __ieee754_log10f(x)
#define __ieee754_pow(x, y)   __ieee754_powf(x, y)
#define __ieee754_fmod(x, y)  __ieee754_fmodf(x, y)
#define __ieee754_hypot(x, y) __ieee754_hypotf(x, y)
#endif

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

/* sqrt is sqrt.c's: an instruction where the machine has one, and
 * integer arithmetic where it does not. */

/* fdlibm's own cores call these two names: its asin/acos/hypot reach the
 * square root through __ieee754_sqrt, and several files test finiteness
 * with the old BSD `finite`. Naming them here rather than editing fdlibm
 * keeps that tree verbatim, which is what makes it auditable against
 * Sun's original. */
double __ieee754_sqrt(double x) { return sqrt(x); }
float __ieee754_sqrtf(float x) { return sqrtf(x); }
int finite(double x) { return __fpclassifyd(x) > FP_INFINITE; }
int finitef(float x) { return __fpclassifyf(x) > FP_INFINITE; }

/* ---- what fdlibm's classic set does not carry ---- */

double log2(double x)  { return __ieee754_log(x) * 1.4426950408889634074; }
double exp2(double x)  { return __ieee754_pow(2.0, x); }
double log1p(double x)
{
    /* log(1+x) with the cancellation removed for small x, which is the
     * entire reason the function exists: log(u) scaled by the rounding of
     * 1 + x (Goldberg). Not for an infinity or a NaN, where that ratio is
     * inf/inf: log1p(+inf) is +inf. */
    double u = 1.0 + x;
    if (u == 1.0) return x;
    if (!isfinite(u)) return __ieee754_log(u);
    return __ieee754_log(u) * (x / (u - 1.0));
}

/* asinh and acosh as fdlibm has them: past 2^28 the 1 under the root is
 * gone and the answer is log(2x) -- where x*x would overflow and leave
 * inf/inf, which was asinh(1e200) a NaN and acosh(1e200) an infinity. */
#define LN2 6.93147180559945286227e-01
double asinh(double x)
{
    double a = fabs(x), t;
    if (!isfinite(a))
        return x + x;                   /* +-inf, or the NaN */
    if (a > 268435456.0)                /* 2^28 */
        t = __ieee754_log(a) + LN2;
    else if (a > 2.0)
        t = __ieee754_log(2.0 * a + 1.0 / (sqrt(a * a + 1.0) + a));
    else
        t = log1p(a + a * a / (1.0 + sqrt(a * a + 1.0)));
    return copysign(t, x);              /* -0 for -0 */
}
double acosh(double x)
{
    if (x < 1.0) { errno = EDOM; return __builtin_nan(""); }
    if (!isfinite(x))
        return x + x;
    if (x > 268435456.0)
        return __ieee754_log(x) + LN2;
    if (x > 2.0)
        return __ieee754_log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
    double t = x - 1.0;
    return log1p(t + sqrt(2.0 * t + t * t));
}
/* On |x|, the sign put back after, as fdlibm has it: for x near -1,
 * 2x/(1 - x) is near -1 and log1p of it all cancellation -- atanh(-0.999999)
 * kept 5 of its digits. Below 1/2, 2x + 2x^2/(1 - x) keeps the small
 * argument's own digits. */
double atanh(double x)
{
    double a = fabs(x), t;
    if (a > 1.0) { errno = EDOM; return __builtin_nan(""); }
    if (a < 0.5) {
        t = a + a;
        t = 0.5 * log1p(t + t * a / (1.0 - a));
    } else {
        t = 0.5 * log1p((a + a) / (1.0 - a));
    }
    return copysign(t, x);
}

double round(double x)
{
    /* Half away from zero — which is round(), not rint(). From the
     * truncation, whose difference from x is exact: floor(x + 0.5) took
     * the largest double below one half to 1, as x + 0.5 rounds up. */
    double t = trunc(x);
    if (fabs(x - t) >= 0.5)
        t += x < 0 ? -1.0 : 1.0;
    return t;
}
/* nearbyint and rint round according to the CURRENT rounding
 * direction, and that is the whole difference from round(): round()
 * always takes a tie AWAY from zero, and the default direction takes it
 * to EVEN. So nearbyint(2.5) is 2 and round(2.5) is 3, and an
 * implementation written as floor(x + 0.5) -- which this was -- is
 * round() under another name. It is the same mistake printf's
 * conversion used to make, and it drifts a column of half-integers
 * upward in exactly the same way.
 *
 * rint differs from nearbyint only in that it may raise the inexact
 * flag; neither raises it here, which the standard permits for
 * nearbyint and tolerates for rint. */
double nearbyint(double x)
{
    double t = trunc(x);
    double d = x - t;
    if (d == 0)
        return x;
    double a = d < 0 ? -d : d;
    switch (fegetround()) {
    case FE_DOWNWARD:   return x < 0 ? t - 1.0 : t;
    case FE_UPWARD:     return x > 0 ? t + 1.0 : t;
    case FE_TOWARDZERO: return t;
    default: break;
    }
    if (a > 0.5)
        return d < 0 ? t - 1.0 : t + 1.0;
    if (a < 0.5)
        return t;
    /* A tie: the answer must be even. */
    double half = t / 2.0;
    if (half == trunc(half))
        return t;
    return d < 0 ? t - 1.0 : t + 1.0;
}

double rint(double x)      { return nearbyint(x); }
long   lround(double x)    { return (long)round(x); }
long long llround(double x){ return (long long)round(x); }
long   lrint(double x)     { return (long)rint(x); }

/* The fraction keeps x's sign, -0.0 for a negative integer; an infinity
 * is all integer, its fraction a zero (x - x would be a NaN). */
double modf(double x, double *ip)
{
    double i = trunc(x);
    *ip = i;
    if (x - x != 0 && x == x)
        return copysign(0.0, x);
    return copysign(x - i, x);
}

/* IEEE's remainder, which is not fmod's. fmod TRUNCATES the quotient;
 * remainder rounds it to the NEAREST integer, ties to EVEN. So
 * remainder(7, 2) is -1 and not 1 -- the nearest multiple of 2 to 7 is
 * 8, not 6 -- and the two disagree for exactly the halfway cases, which
 * is why both exist.
 *
 * The tie rule was missing here, and it is the case argument reduction
 * depends on: reducing an angle by pi/2 lands on a tie whenever the
 * angle is an odd multiple of pi/4. */
double remainder(double x, double y)
{
    double r = fmod(x, y);
    double ay = fabs(y);
    double h = ay / 2;
    double a = fabs(r);
    if (a > h) {
        r = r > 0 ? r - ay : r + ay;
    } else if (a == h) {
        /* Halfway between two multiples: take the EVEN one. */
        double q = (x - r) / y;
        double half = q / 2;
        if (half != trunc(half))
            r = r > 0 ? r - ay : r + ay;
    }
    return r;
}

double fdim(double x, double y) { return x > y ? x - y : 0.0; }
double fmax(double x, double y) { return (x != x) ? y : (y != y) ? x
                                        : (x > y ? x : y); }
double fmin(double x, double y) { return (x != x) ? y : (y != y) ? x
                                        : (x < y ? x : y); }
double fma(double x, double y, double z) { return x * y + z; }

double nan(const char *tag) { (void)tag; return __builtin_nan(""); }

float nextafterf(float x, float y)
{
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    union { float f; uint32_t u; } v = { x };
    if (x == 0) { v.u = 1; return y > 0 ? v.f : -v.f; }
    if ((x < y) == (x > 0)) v.u++; else v.u--;
    return v.f;
}

#ifdef _DOUBLE_IS_32BITS
double nextafter(double x, double y) { return nextafterf(x, y); }
#else
double nextafter(double x, double y)
{
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    union { double d; uint64_t u; } v = { x };
    if (x == 0) { v.u = 1; return y > 0 ? v.d : -v.d; }
    if ((x < y) == (x > 0)) v.u++; else v.u--;
    return v.d;
}
#endif

/* erf, erfc, lgamma and tgamma, and their float forms, are FreeBSD's
 * (msun/s_erf.c, e_lgamma_r.c, bsdsrc/b_tgamma.c and the f files). Where
 * double is binary32 (RX) those double files read a double as two words,
 * which it is not; they are compiled under other names (msun/
 * math_private.h) and the double functions are the float ones -- tgamma
 * from lgamma there, as s_tgammaf.c's tgammaf calls tgamma. */
#ifdef _DOUBLE_IS_32BITS
double erf(double x) { return erff(x); }
double erfc(double x) { return erfcf(x); }
double lgamma_r(double x, int *sign) { return lgammaf_r(x, sign); }
double lgamma(double x) { return lgammaf_r(x, &signgam); }
double tgamma(double x)
{
    int sign;
    if (x == 0 || (x < 0 && x == floor(x)) || x != x)
        return x == 0 ? 1 / x : (x - x) / (x - x);
    double l = lgammaf_r(x, &sign);
    return sign * exp(l);
}
#endif

/* ---- classification ---- */

#ifdef _DOUBLE_IS_32BITS
int __fpclassifyd(double x) { return __fpclassifyf(x); }
#else
int __fpclassifyd(double x)
{
    union { double d; uint64_t u; } v = { x };
    uint32_t e = (uint32_t)((v.u >> 52) & 0x7ff);
    uint64_t m = v.u & 0xfffffffffffffULL;
    if (e == 0)     return m ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0x7ff) return m ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}
#endif
int __fpclassifyf(float x)
{
    union { float f; uint32_t u; } v = { x };
    uint32_t e = (v.u >> 23) & 0xff, m = v.u & 0x7fffff;
    if (e == 0)    return m ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0xff) return m ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}
/* A long double through the compiler's bit builtins, which read the
 * format's own sign and exponent: x87's 80 bits on x86-64, binary128 on
 * AArch64, a double where it is one. */
int __fpclassifyl(long double x)
{
    if (__builtin_isnan(x))    return FP_NAN;
    if (__builtin_isinf(x))    return FP_INFINITE;
    if (x == 0)                return FP_ZERO;
    if (__builtin_isnormal(x)) return FP_NORMAL;
    return FP_SUBNORMAL;
}
int __signbitl(long double x) { return __builtin_signbit(x); }

#ifdef _DOUBLE_IS_32BITS
int __signbitd(double x) { return __signbitf(x); }
#else
int __signbitd(double x)
{
    union { double d; uint64_t u; } v = { x };
    return (int)(v.u >> 63);
}
#endif
int __signbitf(float x)
{
    union { float f; uint32_t u; } v = { x };
    return (int)(v.u >> 31);
}

/* ---- float ----
 *
 * In float, by the float cores (fdlibm/ef_*, kf_*, sf_*: newlib's, Cygnus's
 * conversion of Sun's). sinf, cosf, tanf, atanf, tanhf, cbrtf, ceilf,
 * floorf, truncf, fabsf, copysignf, frexpf, ldexpf, scalbnf and expm1f
 * are the sf_ files' own. Going through double, as this did, was exact
 * but costs a software double on a single-precision FPU -- a Cortex-M4F's
 * sinf as a soft-double series -- and was wrong where double IS float
 * (RX), since fdlibm read a binary32 double as two words.
 */
float acosf(float x)             { return __ieee754_acosf(x); }
float asinf(float x)             { return __ieee754_asinf(x); }
float atan2f(float y, float x)   { return __ieee754_atan2f(y, x); }
float coshf(float x)             { return __ieee754_coshf(x); }
float sinhf(float x)             { return __ieee754_sinhf(x); }
float expf(float x)              { return __ieee754_expf(x); }
float logf(float x)              { return __ieee754_logf(x); }
float log10f(float x)            { return __ieee754_log10f(x); }
float powf(float x, float y)     { return __ieee754_powf(x, y); }
float fmodf(float x, float y)    { return __ieee754_fmodf(x, y); }
float hypotf(float x, float y)   { return __ieee754_hypotf(x, y); }
float log2f(float x)             { return __ieee754_logf(x) * 1.44269504088896340736f; }
float exp2f(float x)             { return __ieee754_powf(2.0f, x); }

/* The machine's instruction where it has a single-precision square root
 * (a Cortex-M4F's vsqrt.f32, RISC-V F's fsqrt.s, SSE, AArch64) -- 25 times
 * a software double's speed on the M4F. Elsewhere exact through double: a
 * double's 53 bits round a float's square root once more without error
 * (53 >= 2 * 24 + 2), and where double is binary32 it IS this function.
 * (__builtin_sqrtf without the instruction is a call to sqrtf: itself.) */
float sqrtf(float x)
{
#if (defined(__ARM_FP) && (__ARM_FP & 4)) || \
    (defined(__riscv_flen) && __riscv_flen >= 32) || \
    defined(__x86_64__) || defined(__aarch64__)
    return __builtin_sqrtf(x);
#else
    return (float)sqrt((double)x);
#endif
}

float roundf(float x)
{
    float t = truncf(x);
    if (fabsf(x - t) >= 0.5f)
        t += x < 0 ? -1.0f : 1.0f;
    return t;
}
float fmaxf(float x, float y) { return (x != x) ? y : (y != y) ? x
                                      : (x > y ? x : y); }
float fminf(float x, float y) { return (x != x) ? y : (y != y) ? x
                                      : (x < y ? x : y); }
float fdimf(float x, float y) { return x > y ? x - y : 0.0f; }

float modff(float x, float *ip)
{
    float i = truncf(x);
    *ip = i;
    if (x - x != 0 && x == x)
        return copysignf(0.0f, x);
    return copysignf(x - i, x);
}

/* ---- long double ----
 *
 * Computed in double and narrowed: this library does not carry an 80-bit
 * or 128-bit core, and pretending otherwise by naming the functions
 * without widening the precision would be worse than saying so here.
 */

#define LD1(n) long double n##l(long double x) { return (long double)n((double)x); }
#define LD2(n) long double n##l(long double x, long double y) \
    { return (long double)n((double)x, (double)y); }
/* fabsl, ceill, floorl and fmodl are NOT here: they move or inspect a
 * value rather than computing a new one, so they can be exact in long
 * double and are written that way in widths.c. Narrowing them would be
 * a visible defect -- truncl of a value with more than 53 significant
 * bits must not lose them. */
LD1(sin) LD1(cos) LD1(tan) LD1(exp) LD1(log) LD1(sqrt)
LD2(pow)

/* ---- ilogb / logb -------------------------------------------------------
 * The unbiased exponent of x: the e in x = m * 2^e with 1 <= |m| < 2.
 * Not in fdlibm's set, so they are built on frexp, whose m is in
 * [0.5, 1) -- hence the -1. The special cases are the whole difficulty
 * and C99 spells each one out: ilogb(0) is INT_MIN, ilogb(inf) is
 * INT_MAX, ilogb(nan) is INT_MIN, while logb returns -inf, +inf and nan
 * respectively. Returning something plausible for zero is the usual bug;
 * a caller that scales by 2^ilogb(x) needs to be able to tell.
 */
int ilogb(double x)
{
    if (x == 0.0)
        return FP_ILOGB0;
    if (isnan(x))
        return FP_ILOGBNAN;
    if (isinf(x))
        return INT_MAX;
    int e;
    frexp(x, &e);
    return e - 1;
}

int ilogbf(float x) { return ilogb((double)x); }
/* at long double's own range: through double, 1e4000L was INT_MAX and a
 * value below double's least was FP_ILOGB0 */
int ilogbl(long double x)
{
    if (x == 0)
        return FP_ILOGB0;
    if (x != x)
        return FP_ILOGBNAN;
    if (x - x != 0)
        return INT_MAX;
    int e;
    frexpl(x, &e);
    return e - 1;
}

double logb(double x)
{
    if (x == 0.0)
        return -HUGE_VAL;
    if (isnan(x) || isinf(x))
        return fabs(x);
    int e;
    frexp(x, &e);
    return (double)(e - 1);
}

float logbf(float x) { return (float)logb((double)x); }
long double logbl(long double x)
{
    if (x == 0)
        return -HUGE_VALL;
    if (x != x || x - x != 0)
        return fabsl(x);
    return ilogbl(x);
}
