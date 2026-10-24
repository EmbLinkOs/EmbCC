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

#define acos(x)        __tg1(acos, x)
#define asin(x)        __tg1(asin, x)
#define atan(x)        __tg1(atan, x)
#define acosh(x)       __tg1(acosh, x)
#define asinh(x)       __tg1(asinh, x)
#define atanh(x)       __tg1(atanh, x)
#define cos(x)         __tg1(cos, x)
#define sin(x)         __tg1(sin, x)
#define tan(x)         __tg1(tan, x)
#define cosh(x)        __tg1(cosh, x)
#define sinh(x)        __tg1(sinh, x)
#define tanh(x)        __tg1(tanh, x)
#define exp(x)         __tg1(exp, x)
#define exp2(x)        __tg1(exp2, x)
#define expm1(x)       __tg1(expm1, x)
#define log(x)         __tg1(log, x)
#define log10(x)       __tg1(log10, x)
#define log1p(x)       __tg1(log1p, x)
#define log2(x)        __tg1(log2, x)
#define logb(x)        __tg1(logb, x)
#define sqrt(x)        __tg1(sqrt, x)
#define cbrt(x)        __tg1(cbrt, x)
#define fabs(x)        __tg1(fabs, x)
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
#define pow(x, y)      __tg2(pow, x, y)
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

/* fma is the one that genuinely needs the right width: its whole
 * purpose is a single rounding, and computing it at a wider type and
 * narrowing would round twice. */
#define fma(x, y, z)   __tg3(fma, x, y, z)
#define remquo(x, y, q) __tg_pick(remquo, __tg_rep(x) + __tg_rep(y))(x, y, q)

#endif /* __cplusplus */

#endif
