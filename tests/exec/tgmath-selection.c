/* <tgmath.h> picks the width from every generic argument (C11 7.25p3):
 * long double if any is one, else double if any is double or an integer,
 * else float. So pow(2.0f, 3.0L) is powl and atan2(y, 1) on a float y is
 * atan2 -- not the first argument's width, which narrowed the second. An
 * argument that is not generic -- scalbn's exponent, frexp's pointer,
 * nexttoward's long double -- does not take part. Asked of the type of
 * each call, which is its function's return type; and the values, which
 * the narrowed second argument made wrong. */
// expect-exit: 42
#include <tgmath.h>
#include <float.h>

#define IS(e, T) _Generic((e), T: 1, default: 0)

_Static_assert(IS(pow(2.0f, 3.0f), float), "float, float");
_Static_assert(IS(pow(2.0f, 3.0), double), "float, double");
_Static_assert(IS(pow(2.0f, 3), double), "float, int");
_Static_assert(IS(pow(2.0f, 3.0L), long double), "float, long double");
_Static_assert(IS(pow(2, 3.0f), double), "int, float");
_Static_assert(IS(pow(2.0L, 3.0f), long double), "long double, float");
_Static_assert(IS(atan2(1.0f, 1), double), "atan2 float, int");
_Static_assert(IS(fmax(1.0f, 2.0f), float), "fmax float, float");
_Static_assert(IS(fma(1.0f, 2.0f, 3.0L), long double), "fma, z wide");
_Static_assert(IS(fma(1.0f, 2.0f, 3.0f), float), "fma floats");
_Static_assert(IS(fma(1.0f, 2.0, 3.0f), double), "fma, y double");
_Static_assert(IS(scalbn(1.0f, 3), float), "scalbn's exponent is not generic");
_Static_assert(IS(ldexp(1.0L, 3), long double), "ldexp long double");
_Static_assert(IS(nexttoward(1.0f, 2.0L), float), "nexttoward's direction");
_Static_assert(IS(nexttoward(1.0, 2.0L), double), "nexttoward double");
_Static_assert(IS(sqrt(2), double), "an integer is a double");
_Static_assert(IS(sqrt(2.0f), float), "sqrt float");

int main(void)
{
    float f = 1.0f;
    int one = 1;
    if (fmax(f, 1.0 + DBL_EPSILON) != 1.0 + DBL_EPSILON)
        return 2;                       /* narrowed, it was 1.0f */
    if (atan2(f, one) != atan2(1.0, 1.0))
        return 3;
    if (frexp(8.0f, &one) != 0.5f || one != 4)
        return 4;
    return 42;
}
