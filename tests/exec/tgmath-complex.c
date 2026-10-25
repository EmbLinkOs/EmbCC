/* <tgmath.h> with complex arguments (C11 7.25p4-5): a function with a
 * complex form calls it, at the argument's width -- sqrt(z) is csqrtf for
 * a float complex, fabs(z) is cabs -- pow is complex when either argument
 * is, and creal, cimag, carg, conj and cproj take real arguments too, as
 * complex ones with no imaginary part. Asked of each call's type, and of
 * values that tell the complex function from the real one. */
// expect-exit: 42
// needs-libc: newlib's <tgmath.h> is not the one under test
#include <tgmath.h>

#define IS(e, T) _Generic((e), T: 1, default: 0)

static float _Complex zf = 3.0f + 4.0fi;
static double _Complex zd = -4.0 + 0.0i;
static long double _Complex zl = 0.0L + 2.0Li;

_Static_assert(IS(sqrt(zf), float _Complex), "csqrtf");
_Static_assert(IS(sqrt(zd), double _Complex), "csqrt");
_Static_assert(IS(sqrt(zl), long double _Complex), "csqrtl");
_Static_assert(IS(sqrt(2.0f), float), "sqrtf still");
_Static_assert(IS(exp(zf), float _Complex), "cexpf");
_Static_assert(IS(sin(zd), double _Complex), "csin");
_Static_assert(IS(atanh(zl), long double _Complex), "catanhl");
_Static_assert(IS(fabs(zf), float), "cabsf is real");
_Static_assert(IS(fabs(zl), long double), "cabsl");
_Static_assert(IS(pow(zf, 2.0f), float _Complex), "cpowf");
_Static_assert(IS(pow(2.0f, zf), float _Complex), "cpowf, complex second");
_Static_assert(IS(pow(zf, 2.0), double _Complex), "cpow: a double widens");
_Static_assert(IS(pow(zf, 2.0L), long double _Complex), "cpowl");
_Static_assert(IS(pow(2.0f, 3.0f), float), "powf still");
_Static_assert(IS(creal(zf), float), "crealf");
_Static_assert(IS(cimag(zl), long double), "cimagl");
_Static_assert(IS(creal(2), double), "creal of an int");
_Static_assert(IS(creal(2.0f), float), "creal of a float");
_Static_assert(IS(carg(zf), float), "cargf");
_Static_assert(IS(carg(1.0), double), "carg of a real");
_Static_assert(IS(conj(zd), double _Complex), "conj");
_Static_assert(IS(conj(1.0f), float _Complex), "conjf of a real");
_Static_assert(IS(cproj(zl), long double _Complex), "cprojl");

int main(void)
{
    float _Complex r = sqrt(zf);              /* 2 + 1i, which sqrtf has not */
    if (crealf(r) != 2.0f || cimagf(r) != 1.0f)
        return 1;
    double _Complex q = sqrt(zd);             /* sqrt(-4) is 2i, not a NaN */
    if (creal(q) != 0.0 || cimag(q) != 2.0)
        return 2;
    if (fabs(zf) != 5.0f)                     /* cabsf: |3 + 4i| */
        return 3;
    float _Complex p = pow(zf, 2.0f);         /* (3 + 4i)^2 = -7 + 24i */
    if (fabs(creal(p) + 7.0f) > 1e-4f || fabs(cimag(p) - 24.0f) > 1e-4f)
        return 4;
    if (creal(zl) != 0.0L || cimag(zl) != 2.0L)
        return 5;
    if (cimag(conj(zf)) != -4.0f || carg(-1.0) != carg(-1.0 + 0.0i))
        return 6;
    if (creal(5) != 5.0 || cimag(5.0f) != 0.0f)
        return 7;
    long double _Complex e = exp(zl * 0.0L);  /* cexpl(0) = 1 */
    if (creall(e) != 1.0L || cimagl(e) != 0.0L)
        return 8;
    return 42;
}
