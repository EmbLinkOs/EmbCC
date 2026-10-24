/* What the FreeBSD msun sources in this directory take from FreeBSD's
 * math_private.h, written for this library rather than copied: the word
 * access fdlibm's header already has, and the few FreeBSD macros these
 * files use. They are kept as FreeBSD has them, each file under its own
 * notice (NOTICE lists them), so a fix there can be carried here. */
#ifndef _MSUN_MATH_PRIVATE_H
#define _MSUN_MATH_PRIVATE_H

#include <complex.h>
#include <float.h>
#include "../fdlibm/fdlibm.h"

/* Where double is binary32 (RX: fdlibm.h's _DOUBLE_IS_32BITS), the double
 * files here read a double as two words, which it is not. They compile to
 * functions of other names, which nothing calls, and the double functions
 * are the float ones (lib/libc/src/math/complex.c) -- as fdlibm's double
 * cores give way to its float ones there. */
#ifdef _DOUBLE_IS_32BITS
#define casinh  __msun_unused_casinh
#define casin   __msun_unused_casin
#define cacosh  __msun_unused_cacosh
#define cacos   __msun_unused_cacos
#define catanh  __msun_unused_catanh
#define catan   __msun_unused_catan
#define csqrt   __msun_unused_csqrt
#define cexp    __msun_unused_cexp
#define ccosh   __msun_unused_ccosh
#define ccos    __msun_unused_ccos
#define csinh   __msun_unused_csinh
#define csin    __msun_unused_csin
#define ctanh   __msun_unused_ctanh
#define ctan    __msun_unused_ctan
#define cproj   __msun_unused_cproj
#define cpow    __msun_unused_cpow
#endif

/* FreeBSD makes csqrtl and the others aliases of the double functions
 * where long double is double. Here every long double function is
 * lib/libc/src/math/complex.c's, on every target, so the alias is only a
 * declaration. */
#define __weak_reference(sym, alias) extern void __msun_no_alias_##alias(void)

#ifndef __unused
#define __unused __attribute__((unused))
#endif

/* A NaN from either argument, raising what an operation on them raises:
 * the sum does both (FreeBSD sums in long double; double carries any
 * NaN of these files' types). */
#define nan_mix(x, y) (((x) + 0.0) + ((y) + 0.0))

/* GNU's sincos, which this library does not have: these files' own. */
static inline void __msun_sincos(double x, double *s, double *c)
{
    *s = sin(x);
    *c = cos(x);
}
static inline void __msun_sincosf(float x, float *s, float *c)
{
    *s = sinf(x);
    *c = cosf(x);
}
#define sincos  __msun_sincos
#define sincosf __msun_sincosf

/* k_exp.c and k_expf.c */
double _Complex __ldexp_cexp(double _Complex, int);
float _Complex __ldexp_cexpf(float _Complex, int);
double __ldexp_exp(double, int);
float __ldexp_expf(float, int);

#endif
