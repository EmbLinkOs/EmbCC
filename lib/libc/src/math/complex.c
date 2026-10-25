/* <complex.h>, C11 §7.3.
 *
 * The compiler already knows what a `_Complex` IS -- its layout, how it is
 * passed and returned -- so this file is only the arithmetic over it.
 *
 * cabs is hypot, not sqrt(x*x + y*y): the naive form overflows for values
 * whose square overflows even though their magnitude does not, and
 * underflows to zero for small ones. That is the single most common bug in
 * a hand-rolled complex library, and fdlibm's hypot already avoids it.
 */
#include <complex.h>
#include <math.h>

/* The constructors. EmbCC has no __builtin_complex, but it does make
 * __real__ and __imag__ assignable, which is all that is needed and is
 * exact for every value -- including the NaNs and signed zeros the
 * `x + y*I` form cannot express. */
double _Complex __cmplx(double r, double i)
{
    double _Complex z;
    __real__ z = r;
    __imag__ z = i;
    return z;
}
float _Complex __cmplxf(float r, float i)
{
    float _Complex z;
    __real__ z = r;
    __imag__ z = i;
    return z;
}
long double _Complex __cmplxl(long double r, long double i)
{
    long double _Complex z;
    __real__ z = r;
    __imag__ z = i;
    return z;
}

double creal(double _Complex z) { return __real__ z; }
double cimag(double _Complex z) { return __imag__ z; }
double cabs(double _Complex z)  { return hypot(__real__ z, __imag__ z); }
double carg(double _Complex z)  { return atan2(__imag__ z, __real__ z); }

double _Complex conj(double _Complex z)
{
    return __cmplx(__real__ z, -__imag__ z);
}

double _Complex cproj(double _Complex z)
{
    /* Every infinity projects to the one point at infinity on the Riemann
     * sphere, with the sign of the imaginary part preserved. */
    if (isinf(__real__ z) || isinf(__imag__ z))
        return __cmplx(__builtin_inf(),
                                 copysign(0.0, __imag__ z));
    return z;
}

double _Complex cexp(double _Complex z)
{
    double e = exp(__real__ z);
    return __cmplx(e * cos(__imag__ z), e * sin(__imag__ z));
}

double _Complex clog(double _Complex z)
{
    return __cmplx(log(cabs(z)), carg(z));
}

double _Complex csqrt(double _Complex z)
{
    double r = cabs(z);
    /* The half-angle form, which stays accurate near the negative real
     * axis where the polar form loses every significant digit. */
    double a = sqrt((r + __real__ z) / 2.0);
    double b = sqrt((r - __real__ z) / 2.0);
    if (__imag__ z < 0) b = -b;
    return __cmplx(a, b);
}

double _Complex cpow(double _Complex x, double _Complex y)
{
    if (__real__ x == 0 && __imag__ x == 0)
        return __cmplx(0.0, 0.0);
    return cexp(clog(x) * y);
}

double _Complex csin(double _Complex z)
{
    return __cmplx(sin(__real__ z) * cosh(__imag__ z),
                             cos(__real__ z) * sinh(__imag__ z));
}
double _Complex ccos(double _Complex z)
{
    return __cmplx(cos(__real__ z) * cosh(__imag__ z),
                             -sin(__real__ z) * sinh(__imag__ z));
}
double _Complex ctan(double _Complex z) { return csin(z) / ccos(z); }

double _Complex csinh(double _Complex z)
{
    return __cmplx(sinh(__real__ z) * cos(__imag__ z),
                             cosh(__real__ z) * sin(__imag__ z));
}
double _Complex ccosh(double _Complex z)
{
    return __cmplx(cosh(__real__ z) * cos(__imag__ z),
                             sinh(__real__ z) * sin(__imag__ z));
}
double _Complex ctanh(double _Complex z) { return csinh(z) / ccosh(z); }

double _Complex casin(double _Complex z)
{
    double _Complex i = __cmplx(0.0, 1.0);
    return -i * clog(i * z + csqrt(__cmplx(1.0, 0.0) - z * z));
}
double _Complex cacos(double _Complex z)
{
    return __cmplx(1.5707963267948966192, 0.0) - casin(z);
}
double _Complex catan(double _Complex z)
{
    double _Complex i = __cmplx(0.0, 1.0);
    double _Complex one = __cmplx(1.0, 0.0);
    return (i / __cmplx(2.0, 0.0)) * clog((i + z) / (i - z));
}

float crealf(float _Complex z) { return __real__ z; }
float cimagf(float _Complex z) { return __imag__ z; }
float cabsf(float _Complex z)  { return (float)hypot(__real__ z, __imag__ z); }
float cargf(float _Complex z)  { return (float)atan2(__imag__ z, __real__ z); }
float _Complex conjf(float _Complex z)
{
    return __cmplxf((float)__real__ z, (float)-__imag__ z);
}
float _Complex cprojf(float _Complex z) { return z; }
float _Complex cexpf(float _Complex z)
{
    double _Complex r = cexp(__cmplx((double)__real__ z,
                                               (double)__imag__ z));
    return __cmplxf((float)__real__ r, (float)__imag__ r);
}
float _Complex clogf(float _Complex z)
{
    double _Complex r = clog(__cmplx((double)__real__ z,
                                               (double)__imag__ z));
    return __cmplxf((float)__real__ r, (float)__imag__ r);
}
float _Complex csqrtf(float _Complex z)
{
    double _Complex r = csqrt(__cmplx((double)__real__ z,
                                                (double)__imag__ z));
    return __cmplxf((float)__real__ r, (float)__imag__ r);
}
float _Complex cpowf(float _Complex x, float _Complex y)
{
    double _Complex r = cpow(__cmplx((double)__real__ x,
                                               (double)__imag__ x),
                             __cmplx((double)__real__ y,
                                               (double)__imag__ y));
    return __cmplxf((float)__real__ r, (float)__imag__ r);
}

long double creall(long double _Complex z) { return __real__ z; }
long double cimagl(long double _Complex z) { return __imag__ z; }
long double cabsl(long double _Complex z)
{
    return (long double)hypot((double)__real__ z, (double)__imag__ z);
}
long double cargl(long double _Complex z)
{
    return (long double)atan2((double)__imag__ z, (double)__real__ z);
}
long double _Complex conjl(long double _Complex z)
{
    return __cmplxl(__real__ z, -__imag__ z);
}
