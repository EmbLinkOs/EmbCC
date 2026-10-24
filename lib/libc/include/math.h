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

#if defined(__APPLE__)
/* Apple's numbering, which its __fpclassifyd returns: with this
 * library's, a Darwin program's isnan() asked whether the answer was 0
 * and Apple's NaN is 1. isfinite's `> FP_INFINITE` holds for both. */
#define FP_NAN       1
#define FP_INFINITE  2
#define FP_ZERO      3
#define FP_NORMAL    4
#define FP_SUBNORMAL 5
#else
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#endif

#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

int __fpclassifyd(double x);
int __fpclassifyf(float x);
int __fpclassifyl(long double x);
int __signbitd(double x);
int __signbitf(float x);
int __signbitl(long double x);

/* By the argument's size, and a long double by its own function where
 * it is wider than a double: converted to double, 1e400L was an
 * infinity and a long double subnormal was zero. */
#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf((float)(x)) \
                       : sizeof(x) == sizeof(double) ? __fpclassifyd((double)(x)) \
                       : __fpclassifyl((long double)(x)))
#define isfinite(x)   (fpclassify(x) > FP_INFINITE)
#define isnan(x)      (fpclassify(x) == FP_NAN)
#define isinf(x)      (fpclassify(x) == FP_INFINITE)
#define isnormal(x)   (fpclassify(x) == FP_NORMAL)
#define signbit(x)    (sizeof(x) == sizeof(float) ? __signbitf((float)(x)) \
                       : sizeof(x) == sizeof(double) ? __signbitd((double)(x)) \
                       : __signbitl((long double)(x)))
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
/* The types the evaluation happens in (7.12p2): FLT_EVAL_METHOD is 0
 * everywhere, so each is its own type. */
typedef float float_t;
typedef double double_t;

/* POSIX's constants, outside strict ISO C (--std=c11 and the rest, which
 * define __STRICT_ANSI__) as glibc and newlib have them: M_PI is in more
 * programs than not, and was missing. */
#if !defined(__STRICT_ANSI__) || defined(_XOPEN_SOURCE) || \
    defined(_POSIX_C_SOURCE) || defined(_GNU_SOURCE) || defined(_DEFAULT_SOURCE)
#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440
#endif

/* ilogb/logb: the exponent as an int and as a value of the same type.
 * Neither is in fdlibm's 1993 set; C99 §7.12.6.5/.11 requires both. */
int    ilogb(double), ilogbf(float), ilogbl(long double);
/* what ilogb answers for a zero and a NaN (7.12p8): the int's least, as
 * glibc's are -- an int being 16 bits on AVR */
#define FP_ILOGB0   (-__INT_MAX__ - 1)
#define FP_ILOGBNAN (-__INT_MAX__ - 1)
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
double nextafter(double, double), nexttoward(double, long double);
double erf(double), erfc(double), tgamma(double), lgamma(double);
/* lgamma's sign, which lgamma leaves in signgam (POSIX), and the forms
 * that return it through a pointer instead -- for a program with threads */
extern int signgam;
double lgamma_r(double, int *);
float lgammaf_r(float, int *);

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
float nexttowardf(float, long double), nanf(const char *);
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
long double nexttowardl(long double, long double), nanl(const char *);
long double remainderl(long double, long double);
long double fmal(long double, long double, long double);
long double remquol(long double, long double, int *);
long double erfl(long double), erfcl(long double);
long double tgammal(long double), lgammal(long double);
long lrintl(long double), lroundl(long double);
long long llrintl(long double), llroundl(long double);

/* C23 (7.12): roundeven, ties to even in any rounding mode; nextup and
 * nextdown, one step toward an infinity; fmaximum and fminimum, which say
 * what fmax and fmin do not -- a NaN either side is a NaN, +0 is above
 * -0 -- with their _num (a number over a NaN), _mag (by magnitude) and
 * _mag_num forms; and exp10. lib/libc/src/math/c23.c. And sinpi, cospi,
 * tanpi, asinpi, acospi, atanpi, atan2pi: the trigonometry of pi x, exact
 * where the argument is (sinpi(1) is 0, where sin(M_PI) is not), FreeBSD's
 * in msun/ where it has them. */
#define __C23_MATH_DECL(T, S)                                               \
    T roundeven##S(T), nextup##S(T), nextdown##S(T), exp10##S(T);           \
    T fmaximum##S(T, T), fminimum##S(T, T);                                 \
    T fmaximum_num##S(T, T), fminimum_num##S(T, T);                         \
    T fmaximum_mag##S(T, T), fminimum_mag##S(T, T);                         \
    T fmaximum_mag_num##S(T, T), fminimum_mag_num##S(T, T);
#define __C23_MATH_PI(T, S)                                                 \
    T sinpi##S(T), cospi##S(T), tanpi##S(T);                                \
    T asinpi##S(T), acospi##S(T), atanpi##S(T), atan2pi##S(T, T);
__C23_MATH_PI(double, )
__C23_MATH_PI(float, f)
__C23_MATH_PI(long double, l)
#undef __C23_MATH_PI
__C23_MATH_DECL(double, )
__C23_MATH_DECL(float, f)
__C23_MATH_DECL(long double, l)
#undef __C23_MATH_DECL

#if __STDC_VERSION__ >= 202311L
#define iszero(x)      ((x) == 0)
#define issubnormal(x) (fpclassify(x) == FP_SUBNORMAL)
#endif

#ifdef __cplusplus
}
#endif

#endif
