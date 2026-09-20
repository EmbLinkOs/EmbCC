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

/* manipulation */
double fabs(double), copysign(double, double), nan(const char *);
double fdim(double, double), fmax(double, double), fmin(double, double);
double fma(double, double, double);
double nextafter(double, double);
double erf(double), erfc(double), tgamma(double), lgamma(double);

/* the float forms C requires */
float sinf(float), cosf(float), tanf(float);
float asinf(float), acosf(float), atanf(float), atan2f(float, float);
float sinhf(float), coshf(float), tanhf(float);
float expf(float), exp2f(float), logf(float), log2f(float), log10f(float);
float powf(float, float), sqrtf(float), cbrtf(float), hypotf(float, float);
float ceilf(float), floorf(float), truncf(float), roundf(float);
float fabsf(float), copysignf(float, float), fmodf(float, float);
float ldexpf(float, int), frexpf(float, int *), modff(float, float *);
float fmaxf(float, float), fminf(float, float), fdimf(float, float);

/* long double: this library computes in double and widens, which is exact
 * for every value a double can hold and honest about the rest. */
long double sinl(long double), cosl(long double), tanl(long double);
long double expl(long double), logl(long double), sqrtl(long double);
long double powl(long double, long double), fabsl(long double);
long double ceill(long double), floorl(long double), fmodl(long double, long double);

#ifdef __cplusplus
}
#endif

#endif
