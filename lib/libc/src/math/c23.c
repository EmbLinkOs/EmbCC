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
