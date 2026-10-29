/* <tgmath.h> — C11 §7.25.
 *
 * Type-generic math: `sqrt(x)` calls `sqrtf`, `sqrt` or `sqrtl`
 * depending on what `x` is. Without it every call has to name the
 * width, and a `float` computation written with the double functions
 * rounds through a double at every step -- which costs speed on a
 * target with no double unit and precision nowhere.
 *
 * ---- how it works -------------------------------------------------------
 *
 * `_Generic`, which is exactly what C11 added it for. Each macro below
 * is a selection on the argument's type, and it is a MACRO because the
 * choice is made from a type and C has no overloading.
 *
 * Two consequences worth knowing. The macro shadows the function name,
 * so `(sqrt)(x)` -- with parentheses -- still calls the double one, and
 * `&sqrt` does not compile. And an argument is evaluated once by the
 * selected call but named several times in the _Generic, which is fine
 * because only the SELECTED branch is evaluated: `_Generic` discards
 * the others without evaluating them, and that is a language guarantee
 * rather than an optimization.
 *
 * The complex half of C11's tgmath is not here. This library's
 * <complex.h> has the complex functions under their own names; the
 * type-generic dispatch to them would double the size of every macro
 * below for a case that is easier to write out.
 */
#ifndef _TGMATH_H
#define _TGMATH_H

#include <math.h>
#include <complex.h>

#ifndef __cplusplus

/* One argument. The default branch catches every integer type too, as
 * C requires: an integer argument is converted to double. */
/* The parameter is `fn` and not `f`, because `f##f` pastes the
 * ARGUMENT with itself -- both operands are the same parameter -- and
 * `sqrt` becomes `sqrtsqrt`. The suffix letters must not be parameter
 * names. */
#define __tg1(fn, x) _Generic((x),                    \
    float: fn##f,                                     \
    long double: fn##l,                               \
    default: fn)(x)

/* Two and three arguments. C11 7.25p3 decides from all the generic
 * arguments together: long double if any is one, else double if any is
 * double or an integer, else float -- so pow(2.0f, 3.0L) is powl, and
 * atan2(y, 1) on a float y is atan2, not atan2f. That is the usual
 * arithmetic conversions over a stand-in for each argument -- a float, a
 * long double, or for anything else a double -- which the outer
 * _Generic then reads the type of. No stand-in is evaluated: a
 * _Generic's controlling expression never is. */
#define __tg_rep(x) _Generic((x),                     \
    float: 0.0f,                                      \
    long double: 0.0L,                                \
    default: 0.0)

#define __tg_pick(fn, t) _Generic((t),                \
    float: fn##f,                                     \
    long double: fn##l,                               \
    default: fn)

#define __tg2(fn, x, y) __tg_pick(fn, __tg_rep(x) + __tg_rep(y))(x, y)

#define __tg3(fn, x, y, z)                                              \
    __tg_pick(fn, __tg_rep(x) + __tg_rep(y) + __tg_rep(z))(x, y, z)

/* The ones with one generic argument and another that is not: the
 * exponent of scalbn and ldexp, the pointer of frexp and modf, the long
 * double direction of nexttoward. The first decides alone. */
#define __tgx(fn, x, y) __tg_pick(fn, __tg_rep(x))(x, y)

/* A complex argument (C11 7.25p4-5): the functions with a complex form
 * call it -- sqrt(z) is csqrt, fabs(z) cabs, and the width is the
 * argument's, csqrtf for a float complex -- and pow is complex when
 * either argument is, of the width both decide. */
#define __tgc1(fn, cfn, x) _Generic((x),              \
    float: fn##f,                                     \
    long double: fn##l,                               \
    float _Complex: cfn##f,                           \
    double _Complex: cfn,                             \
    long double _Complex: cfn##l,                     \
    default: fn)(x)

#define __tgc_rep(x) _Generic((x),                    \
    float: 0.0f,                                      \
    long double: 0.0L,                                \
    float _Complex: (float _Complex)0,                \
    double _Complex: (double _Complex)0,              \
    long double _Complex: (long double _Complex)0,    \
    default: 0.0)

#define __tgc_pick(fn, cfn, t) _Generic((t),          \
    float: fn##f,                                     \
    long double: fn##l,                               \
    float _Complex: cfn##f,                           \
    double _Complex: cfn,                             \
    long double _Complex: cfn##l,                     \
    default: fn)

#define __tgc2(fn, cfn, x, y)                                           \
    __tgc_pick(fn, cfn, __tgc_rep(x) + __tgc_rep(y))(x, y)

/* The complex-only ones take a real argument too, as a complex one with
 * no imaginary part, at its width: carg(1.0f) is cargf. */
#define __tgz1(cfn, x) _Generic((x),                  \
    float: cfn##f,                                    \
    float _Complex: cfn##f,                           \
    long double: cfn##l,                              \
    long double _Complex: cfn##l,                     \
    default: cfn)(x)

#define acos(x)        __tgc1(acos, cacos, x)
#define asin(x)        __tgc1(asin, casin, x)
#define atan(x)        __tgc1(atan, catan, x)
#define acosh(x)       __tgc1(acosh, cacosh, x)
#define asinh(x)       __tgc1(asinh, casinh, x)
#define atanh(x)       __tgc1(atanh, catanh, x)
#define cos(x)         __tgc1(cos, ccos, x)
#define sin(x)         __tgc1(sin, csin, x)
#define tan(x)         __tgc1(tan, ctan, x)
#define cosh(x)        __tgc1(cosh, ccosh, x)
#define sinh(x)        __tgc1(sinh, csinh, x)
#define tanh(x)        __tgc1(tanh, ctanh, x)
#define exp(x)         __tgc1(exp, cexp, x)
#define exp2(x)        __tg1(exp2, x)
#define expm1(x)       __tg1(expm1, x)
#define log(x)         __tgc1(log, clog, x)
#define log10(x)       __tg1(log10, x)
#define log1p(x)       __tg1(log1p, x)
#define log2(x)        __tg1(log2, x)
#define logb(x)        __tg1(logb, x)
#define sqrt(x)        __tgc1(sqrt, csqrt, x)
#define cbrt(x)        __tg1(cbrt, x)
#define fabs(x)        __tgc1(fabs, cabs, x)
#define ceil(x)        __tg1(ceil, x)
#define floor(x)       __tg1(floor, x)
#define nearbyint(x)   __tg1(nearbyint, x)
#define rint(x)        __tg1(rint, x)
#define round(x)       __tg1(round, x)
#define trunc(x)       __tg1(trunc, x)
#define erf(x)         __tg1(erf, x)
#define erfc(x)        __tg1(erfc, x)
#define lgamma(x)      __tg1(lgamma, x)
#define tgamma(x)      __tg1(tgamma, x)
#define ilogb(x)       __tg1(ilogb, x)
#define llrint(x)      __tg1(llrint, x)
#define llround(x)     __tg1(llround, x)
#define lrint(x)       __tg1(lrint, x)
#define lround(x)      __tg1(lround, x)

#define atan2(x, y)    __tg2(atan2, x, y)
#define copysign(x, y) __tg2(copysign, x, y)
#define fdim(x, y)     __tg2(fdim, x, y)
#define fmax(x, y)     __tg2(fmax, x, y)
#define fmin(x, y)     __tg2(fmin, x, y)
#define fmod(x, y)     __tg2(fmod, x, y)
#define hypot(x, y)    __tg2(hypot, x, y)
#define nextafter(x, y) __tg2(nextafter, x, y)
#define pow(x, y)      __tgc2(pow, cpow, x, y)
#define remainder(x, y) __tg2(remainder, x, y)
#define scalbn(x, n)   __tgx(scalbn, x, n)
#define scalbln(x, n)  __tgx(scalbln, x, n)
#define ldexp(x, n)    __tgx(ldexp, x, n)
#define frexp(x, p)    __tgx(frexp, x, p)
#define modf(x, p)     __tgx(modf, x, p)
#define nexttoward(x, y) __tgx(nexttoward, x, y)
/* C23's */
#define roundeven(x)   __tg1(roundeven, x)
#define nextup(x)      __tg1(nextup, x)
#define nextdown(x)    __tg1(nextdown, x)
#define exp10(x)       __tg1(exp10, x)
#define fmaximum(x, y) __tg2(fmaximum, x, y)
#define fminimum(x, y) __tg2(fminimum, x, y)
#define fmaximum_num(x, y) __tg2(fmaximum_num, x, y)
#define fminimum_num(x, y) __tg2(fminimum_num, x, y)
#define fmaximum_mag(x, y) __tg2(fmaximum_mag, x, y)
#define fminimum_mag(x, y) __tg2(fminimum_mag, x, y)
#define fmaximum_mag_num(x, y) __tg2(fmaximum_mag_num, x, y)
#define fminimum_mag_num(x, y) __tg2(fminimum_mag_num, x, y)
#define sinpi(x)       __tg1(sinpi, x)
#define cospi(x)       __tg1(cospi, x)
#define tanpi(x)       __tg1(tanpi, x)
#define asinpi(x)      __tg1(asinpi, x)
#define acospi(x)      __tg1(acospi, x)
#define atanpi(x)      __tg1(atanpi, x)
#define atan2pi(y, x)  __tg2(atan2pi, y, x)

#define carg(z)        __tgz1(carg, z)
#define conj(z)        __tgz1(conj, z)
#define cproj(z)       __tgz1(cproj, z)
/* the parts, at the argument's width, without a call (as <complex.h>) */
#undef creal
#undef cimag
#define creal(z) _Generic((z),                                          \
    float: (float)__real__ (z), float _Complex: (float)__real__ (z),    \
    long double: (long double)__real__ (z),                             \
    long double _Complex: (long double)__real__ (z),                    \
    default: (double)__real__ (z))
#define cimag(z) _Generic((z),                                          \
    float: (float)__imag__ (z), float _Complex: (float)__imag__ (z),    \
    long double: (long double)__imag__ (z),                             \
    long double _Complex: (long double)__imag__ (z),                    \
    default: (double)__imag__ (z))

/* fma is the one that genuinely needs the right width: its whole
 * purpose is a single rounding, and computing it at a wider type and
 * narrowing would round twice. */
#define fma(x, y, z)   __tg3(fma, x, y, z)
#define remquo(x, y, q) __tg_pick(remquo, __tg_rep(x) + __tg_rep(y))(x, y, q)

#endif /* __cplusplus */

#endif
