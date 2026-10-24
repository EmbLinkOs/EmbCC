/* What the FreeBSD msun sources in this directory take from FreeBSD's
 * math_private.h, written for this library rather than copied: the word
 * access fdlibm's header already has, and the few FreeBSD macros these
 * files use -- and, under Sun's notice, the few of its macros copied below.
 * The files are kept as FreeBSD has them, each under its own notice
 * (NOTICE lists them), so a fix there can be carried here. */
#ifndef _MSUN_MATH_PRIVATE_H
#define _MSUN_MATH_PRIVATE_H

#include <complex.h>
#include <float.h>
#include <stdint.h>
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
#define erf     __msun_unused_erf
#define erfc    __msun_unused_erfc
#define lgamma  __msun_unused_lgamma
#define lgamma_r __msun_unused_lgamma_r
#define tgamma  __msun_unused_tgamma
#define sinpi   __msun_unused_sinpi
#define cospi   __msun_unused_cospi
#define tanpi   __msun_unused_tanpi
#define asinpi  __msun_unused_asinpi
#define atanpi  __msun_unused_atanpi
#define acospi  __msun_unused_acospi
double __msun_unused_lgamma_r(double, int *);
double __msun_unused_sinpi(double), __msun_unused_cospi(double);
double __msun_unused_tgamma(double), __msun_unused_lgamma(double);
#endif

/* FreeBSD makes csqrtl and the others aliases of the double functions
 * where long double is double. Here every long double function is
 * lib/libc/src/math/complex.c's, on every target, so the alias is only a
 * declaration. */
#define __weak_reference(sym, alias) extern void __msun_no_alias_##alias(void)

/* BSD's spellings of the exact-width types */
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

/* The float kernels on a double argument, msun/k_sinf.c and k_cosf.c --
 * included and made inline by the files that say so. */
#ifndef INLINE_KERNEL_SINDF
float __kernel_sindf(double);
#endif
#ifndef INLINE_KERNEL_COSDF
float __kernel_cosdf(double);
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef STRICT_ASSIGN
#define STRICT_ASSIGN(type, lval, rval) ((lval) = (rval))
#endif

/* The double-double arithmetic and the fast floors the half-cycle
 * trigonometric files (sinpi and the others) use, as FreeBSD's
 * math_private.h has them:
 *
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
#define _SPLIT(x, xh, xl)		\
do {					\
	typeof(xl) __t1;		\
	__t1 = (x) * _CC;		\
	xh = __t1 + ((x) - __t1);	\
	xl = (x) - xh;			\
} while(0)
#define _FAST2SUM(x, y, hi, lo)	\
do {				\
	hi = (x) + (y);		\
	lo = (y) - (hi - (x));	\
} while(0)
#define _SLOW2SUM(x, y, hi, lo)			\
do {						\
	volatile typeof(lo) __t1;		\
	typeof(lo) __t2;				\
	hi = (x) + (y);				\
	__t1 = hi - (y);			\
	__t2 = hi - __t1;			\
	lo = ((x) - __t1) + ((y) - __t2);	\
} while(0)
#define _XADD(xh, xl, yh, yl, zh, zl)			\
do {							\
	typeof(zl) __s1, __s2, __s3, __s4, __s5, __s6;	\
	_SLOW2SUM(xh, yh, __s1, __s2);			\
	_SLOW2SUM(xl, yl, __s3, __s4);			\
	_FAST2SUM(__s1, __s2 + __s3, __s5, __s6);	\
	_FAST2SUM(__s5, __s6 + __s4, zh, zl);		\
} while(0)
#define _MUL(x, y, r1 ,r2)				\
do  {							\
	typeof(r2) __xh, __xl, __yh, __yl, __t1;	\
	_SPLIT(x, __xh, __xl);				\
	_SPLIT(y, __yh, __yl);				\
	r1 = (x) * (y);					\
	__t1 = __xh * __yl + (__xh * __yh - r1);	\
	r2 = __xl * __yl + (__xl * __yh + __t1);	\
} while(0)
#define _XMUL(xh, xl, yh, yl, ph, pl)		\
do {						\
	_MUL(xh, yh, ph, pl);			\
	pl += xl * yl + xl * yh + xh * yl;	\
} while(0)
#define _SQRT(x, zh, zl)			\
do {						\
	typeof(zl) s, ph, pl, xh, xl, yh, yl;	\
	_SPLIT(x, xh, xl);			\
	s = 1 / _ROOT(x);			\
	yh = s * xh;				\
	yl = s * xl;				\
	_XMUL(yh, yl, yh, yl, ph, pl);		\
	_XADD(xh, xl, -ph, -pl, zh, zl);	\
	s /= 2;					\
	_MUL(s, zh, ph, pl);			\
	pl += s * zl;				\
	_XADD(yh, yl, ph, pl, zh, zl);		\
} while(0)
#define	_2sumF(a, b) do {	\
	__typeof(a) __w;	\
				\
	__w = (a) + (b);	\
	(b) = ((a) - __w) + (b); \
	(a) = __w;		\
} while (0)
#define	FFLOORF(x, j0, ix) do {			\
	(j0) = (((ix) >> 23) & 0xff) - 0x7f;	\
	(ix) &= ~(0x007fffff >> (j0));		\
	SET_FLOAT_WORD((x), (ix));		\
} while (0)
#define	FFLOOR(x, j0, ix, lx) do {				\
	(j0) = (((ix) >> 20) & 0x7ff) - 0x3ff;			\
	if ((j0) < 20) {					\
		(ix) &= ~(0x000fffff >> (j0));			\
		(lx) = 0;					\
	} else {						\
		(lx) &= ~((uint32_t)0xffffffff >> ((j0) - 20));	\
	}							\
	INSERT_WORDS((x), (ix), (lx));				\
} while (0)

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
