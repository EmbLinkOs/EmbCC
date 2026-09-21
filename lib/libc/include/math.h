/* <math.h> — C11 §7.12.
 *
 * The transcendentals are fdlibm (Sun's, ~1 ulp, the same source newlib's
 * libm is built from); src/math/math.c is the thin layer that names its
 * cores and adds what its classic set does not carry. */
#ifndef _MATH_H
#define _MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

int __fpclassifyd(double x);
int __fpclassifyf(float x);
int __signbitd(double x);
int __signbitf(float x);

#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf((float)(x)) \
                                                  : __fpclassifyd((double)(x)))
#define isfinite(x)   (fpclassify(x) > FP_INFINITE)
#define isnan(x)      (fpclassify(x) == FP_NAN)
#define isinf(x)      (fpclassify(x) == FP_INFINITE)
#define isnormal(x)   (fpclassify(x) == FP_NORMAL)
#define signbit(x)    (sizeof(x) == sizeof(float) ? __signbitf((float)(x)) \
                                                  : __signbitd((double)(x)))
#define isgreater(a, b)      ((a) > (b))
#define isgreaterequal(a, b) ((a) >= (b))
#define isless(a, b)         ((a) < (b))
#define islessequal(a, b)    ((a) <= (b))
#define islessgreater(a, b)  ((a) < (b) || (a) > (b))
#define isunordered(a, b)    (isnan(a) || isnan(b))

/* trigonometry */
double sin(double), cos(double), tan(double);
double asin(double), acos(double), atan(double), atan2(double, double);
double sinh(double), cosh(double), tanh(double);
double asinh(double), acosh(double), atanh(double);

/* exponential and logarithmic */
double exp(double), exp2(double), expm1(double);
double log(double), log2(double), log10(double), log1p(double);
double pow(double, double), sqrt(double), cbrt(double), hypot(double, double);
double frexp(double, int *), ldexp(double, int), scalbn(double, int);
/* ilogb/logb: the exponent as an int and as a value of the same type.
 * Neither is in fdlibm's 1993 set; C99 §7.12.6.5/.11 requires both. */
int    ilogb(double), ilogbf(float), ilogbl(long double);
double logb(double);
float  logbf(float);
long double logbl(long double);

/* rounding and remainder */
double ceil(double), floor(double), trunc(double), round(double);
double nearbyint(double), rint(double);
double fmod(double, double), remainder(double, double), modf(double, double *);
long   lround(double);
long long llround(double);
long   lrint(double);
long long llrint(double);
double scalbln(double, long);
/* remquo gives the remainder AND the low bits of the quotient, which is
 * what an argument reduction needs: knowing how many multiples of pi/2
 * were removed decides which of sin/cos/-sin/-cos the answer is, and
 * recovering it from the remainder alone is impossible. */
double remquo(double, double, int *);

/* manipulation */
double fabs(double), copysign(double, double), nan(const char *);
double fdim(double, double), fmax(double, double), fmin(double, double);
double fma(double, double, double);
double nextafter(double, double);
double erf(double), erfc(double), tgamma(double), lgamma(double);

/* The float forms. C11 requires all three widths for every function
 * here, and <tgmath.h> makes the requirement visible: a type-generic
 * `cbrt(x)` on a long double selects `cbrtl`, so a missing variant is a
 * compile error rather than a quiet fallback. */
float sinf(float), cosf(float), tanf(float);
float asinf(float), acosf(float), atanf(float), atan2f(float, float);
float sinhf(float), coshf(float), tanhf(float);
float asinhf(float), acoshf(float), atanhf(float);
float expf(float), exp2f(float), expm1f(float);
float logf(float), log2f(float), log10f(float), log1pf(float);
float powf(float, float), sqrtf(float), cbrtf(float), hypotf(float, float);
float ceilf(float), floorf(float), truncf(float), roundf(float);
float nearbyintf(float), rintf(float);
float fabsf(float), copysignf(float, float), fmodf(float, float);
float ldexpf(float, int), frexpf(float, int *), modff(float, float *);
float fmaxf(float, float), fminf(float, float), fdimf(float, float);
float scalbnf(float, int), scalblnf(float, long);
float nextafterf(float, float), remainderf(float, float);
float fmaf(float, float, float), remquof(float, float, int *);
float erff(float), erfcf(float), tgammaf(float), lgammaf(float);
long lrintf(float), lroundf(float);
long long llrintf(float), llroundf(float);

/* long double. Most of these compute in DOUBLE and widen, which is
 * exact for every value a double can hold and loses the extra mantissa
 * bits an 80-bit x87 long double has -- so `sinl` is a double's answer
 * in a wider type. That is stated rather than hidden, and it is what
 * most C libraries do for the transcendental functions.
 *
 * The ones that are EXACT in long double are the ones that can be:
 * fabsl, copysignl, ceill, floorl, truncl, roundl, fmaxl, fminl, fdiml,
 * ldexpl, frexpl, modfl and scalbnl move or inspect the value without
 * computing a new one, so they are written in long double throughout. */
long double sinl(long double), cosl(long double), tanl(long double);
long double asinl(long double), acosl(long double), atanl(long double);
long double atan2l(long double, long double);
long double sinhl(long double), coshl(long double), tanhl(long double);
long double asinhl(long double), acoshl(long double), atanhl(long double);
long double expl(long double), exp2l(long double), expm1l(long double);
long double logl(long double), log2l(long double), log10l(long double);
long double log1pl(long double);
long double powl(long double, long double), sqrtl(long double);
long double cbrtl(long double), hypotl(long double, long double);
long double fabsl(long double), copysignl(long double, long double);
long double ceill(long double), floorl(long double), truncl(long double);
long double roundl(long double), nearbyintl(long double), rintl(long double);
long double fmodl(long double, long double);
long double ldexpl(long double, int), frexpl(long double, int *);
long double modfl(long double, long double *);
long double fmaxl(long double, long double), fminl(long double, long double);
long double fdiml(long double, long double);
long double scalbnl(long double, int), scalblnl(long double, long);
long double nextafterl(long double, long double);
long double remainderl(long double, long double);
long double fmal(long double, long double, long double);
long double remquol(long double, long double, int *);
long double erfl(long double), erfcl(long double);
long double tgammal(long double), lgammal(long double);
long lrintl(long double), lroundl(long double);
long long llrintl(long double), llroundl(long double);

#ifdef __cplusplus
}
#endif

#endif
