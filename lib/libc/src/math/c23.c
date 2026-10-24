/* C23's additions to <math.h> (§7.12), at all three widths: roundeven,
 * nextup and nextdown, the fmaximum/fminimum family, and exp10.
 *
 *   roundeven     to the nearest integer, a tie to the even one, whatever
 *                 the rounding mode -- round() with its ties moved
 *   nextup/down   the next value toward +inf / -inf: nextafter's, toward
 *                 an infinity, at the width's own step (nextafterl's)
 *   fmaximum      the larger, a NaN if either is one, and +0 above -0 --
 *                 what fmax cannot say about zeros or NaNs
 *   fminimum      the smaller, likewise, -0 below +0
 *   ..._num       a number over a NaN, as fmax does; NaN only from two
 *   ..._mag       the one of larger (smaller) magnitude, the fmaximum
 *                 (fminimum) of the two when their magnitudes are equal
 *   exp10         10^x, as pow computes it
 *
 * A NaN result is x + y, which is quiet whichever operand was the NaN.
 */
#include <math.h>

#include "fdlibm/fdlibm.h"

#define C23_MATH(T, S)                                                      \
static int neg##S(T x) { return copysign##S((T)1, x) < 0; }                 \
                                                                            \
T roundeven##S(T x)                                                         \
{                                                                           \
    T r = round##S(x);                                                      \
    if (fabs##S(x - trunc##S(x)) == (T)0.5) {                               \
        T h = r * (T)0.5;                                                   \
        if (h != trunc##S(h))          /* a tie that went to odd */         \
            r = copysign##S(r - copysign##S((T)1, x), x);                   \
    }                                                                       \
    return r;                                                               \
}                                                                           \
                                                                            \
T nextup##S(T x)   { return nextafter##S(x, (T)INFINITY); }                 \
T nextdown##S(T x) { return nextafter##S(x, -(T)INFINITY); }                \
                                                                            \
T fmaximum##S(T x, T y)                                                     \
{                                                                           \
    if (x != x || y != y) return x + y;                                     \
    if (x == y) return neg##S(x) ? y : x;                                   \
    return x > y ? x : y;                                                   \
}                                                                           \
T fminimum##S(T x, T y)                                                     \
{                                                                           \
    if (x != x || y != y) return x + y;                                     \
    if (x == y) return neg##S(x) ? x : y;                                   \
    return x < y ? x : y;                                                   \
}                                                                           \
T fmaximum_mag##S(T x, T y)                                                 \
{                                                                           \
    if (x != x || y != y) return x + y;                                     \
    T ax = fabs##S(x), ay = fabs##S(y);                                     \
    return ax > ay ? x : ay > ax ? y : fmaximum##S(x, y);                   \
}                                                                           \
T fminimum_mag##S(T x, T y)                                                 \
{                                                                           \
    if (x != x || y != y) return x + y;                                     \
    T ax = fabs##S(x), ay = fabs##S(y);                                     \
    return ax < ay ? x : ay < ax ? y : fminimum##S(x, y);                   \
}                                                                           \
T fmaximum_num##S(T x, T y)                                                 \
{                                                                           \
    if (x != x) return y != y ? x + y : y;                                  \
    return y != y ? x : fmaximum##S(x, y);                                  \
}                                                                           \
T fminimum_num##S(T x, T y)                                                 \
{                                                                           \
    if (x != x) return y != y ? x + y : y;                                  \
    return y != y ? x : fminimum##S(x, y);                                  \
}                                                                           \
T fmaximum_mag_num##S(T x, T y)                                             \
{                                                                           \
    if (x != x) return y != y ? x + y : y;                                  \
    return y != y ? x : fmaximum_mag##S(x, y);                              \
}                                                                           \
T fminimum_mag_num##S(T x, T y)                                             \
{                                                                           \
    if (x != x) return y != y ? x + y : y;                                  \
    return y != y ? x : fminimum_mag##S(x, y);                              \
}                                                                           \
                                                                            \
T exp10##S(T x) { return pow##S((T)10, x); }

C23_MATH(double, )
C23_MATH(float, f)
C23_MATH(long double, l)

/* atan2pi, which FreeBSD has not got: atan2 over pi, an ulp or two from
 * the answer where the others (msun/) are within one. */
double atan2pi(double y, double x) { return atan2(y, x) / M_PI; }
float atan2pif(float y, float x) { return (float)(atan2(y, x) / M_PI); }

/* the long double forms in double, as this library's long double
 * transcendentals are (docs/manual/libraries.md) */
long double sinpil(long double x)  { return sinpi((double)x); }
long double cospil(long double x)  { return cospi((double)x); }
long double tanpil(long double x)  { return tanpi((double)x); }
long double asinpil(long double x) { return asinpi((double)x); }
long double acospil(long double x) { return acospi((double)x); }
long double atanpil(long double x) { return atanpi((double)x); }
long double atan2pil(long double y, long double x)
{
    return atan2pi((double)y, (double)x);
}

/* where double is binary32 (RX) the double ones are the float ones */
#ifdef _DOUBLE_IS_32BITS
double sinpi(double x)  { return sinpif(x); }
double cospi(double x)  { return cospif(x); }
double tanpi(double x)  { return tanpif(x); }
double asinpi(double x) { return asinpif(x); }
double atanpi(double x) { return atanpif(x); }
double acospi(double x) { return acospif(x); }
#endif
