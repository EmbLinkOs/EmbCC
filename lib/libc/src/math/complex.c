/* <complex.h>, C11 §7.3.
 *
 * The compiler already knows what a `_Complex` IS -- its layout, how it is
 * passed and returned -- so this file is only the arithmetic over it, and
 * not most of that: the exponentials, square roots, powers and the
 * trigonometric and hyperbolic functions with their inverses are FreeBSD's
 * (src/math/msun), in double and float, which handle every special value
 * C11's Annex G lists and stay accurate across the plane -- catrig.c is
 * Hull, Fairgrieve and Tang's algorithm, where `-i * clog(i*z + csqrt(1 -
 * z*z))` loses every digit near the branch points. What is here: the
 * parts, the constructors, the absolute value and argument, the
 * conjugate, the logarithm, and every long double function.
 *
 * cabs is hypot, not sqrt(x*x + y*y): the naive form overflows for values
 * whose square overflows even though their magnitude does not, and
 * underflows to zero for small ones.
 *
 * The long double functions compute in double, as the rest of this
 * library's long double transcendentals do (docs/manual/libraries.md);
 * where long double is double -- the 32-bit targets -- that is exact. The
 * ones that only take a value apart or put it back -- the parts, conj,
 * cproj -- work at full width. */
#include <complex.h>
#include <math.h>

/* The constructors CMPLX called before it was the header's own: objects
 * built then still call them. EmbCC has no __builtin_complex, but it
 * does make __real__ and __imag__ assignable, which is all that is
 * needed and is exact for every value -- including the NaNs and signed
 * zeros the `x + y*I` form cannot express. */
double _Complex __cmplx(double, double);
float _Complex __cmplxf(float, float);
long double _Complex __cmplxl(long double, long double);
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

double (creal)(double _Complex z) { return __real__ z; }
double (cimag)(double _Complex z) { return __imag__ z; }
double cabs(double _Complex z)  { return hypot(__real__ z, __imag__ z); }
double carg(double _Complex z)  { return atan2(__imag__ z, __real__ z); }
double _Complex conj(double _Complex z)
{
    return __cmplx(__real__ z, -__imag__ z);
}

float (crealf)(float _Complex z) { return __real__ z; }
float (cimagf)(float _Complex z) { return __imag__ z; }
float cabsf(float _Complex z)  { return hypotf(__real__ z, __imag__ z); }
float cargf(float _Complex z)  { return atan2f(__imag__ z, __real__ z); }
float _Complex conjf(float _Complex z)
{
    return __cmplxf(__real__ z, -__imag__ z);
}

/* log|z| + i arg z. Near |z| = 1, log of the hypotenuse is log of a
 * number rounding has already moved by an ulp, and the result -- near
 * 0 -- keeps none of its digits; |z|^2 - 1 has to be had exactly, which
 * x*x + y*y - 1 is not: each product is off by up to half an ulp of 0.5,
 * as much as the whole answer at |0.6 + 0.8i|. So each square is split
 * into a high and a low part that sum to it exactly (Dekker's product:
 * a split of a into 26 and 27 bits, whose products are exact), the high
 * part of the larger less 1 is exact there (Sterbenz), and the rest are
 * added smallest first, the one rounding of a sum kept (Knuth's
 * two-sum). log1p of that, halved, is log|z| to the last bit. Everything
 * else -- zeros, infinities, NaNs -- is what hypot, log and atan2 give:
 * -inf + i(0 or pi) for a zero, +inf for an infinity, NaN for a NaN. */
static void sq(double a, double *hi, double *lo)
{
    double p = a * 134217729.0;                 /* 2^27 + 1 */
    double ah = p - (p - a), al = a - ah;
    *hi = a * a;
    *lo = ((ah * ah - *hi) + 2 * ah * al) + al * al;
}

double _Complex clog(double _Complex z)
{
    double x = fabs(__real__ z), y = fabs(__imag__ z), h = hypot(x, y), r;
    if (h > 0.7 && h < 1.4) {
        double big = x > y ? x : y, small = x > y ? y : x;
        double bh, bl, sh, sl;
        sq(big, &bh, &bl);
        sq(small, &sh, &sl);
        double t = bh - 1, s = t + sh;          /* t exact; s rounded */
        double e = (t - (s - (s - t))) + (sh - (s - t));   /* that rounding */
        r = 0.5 * log1p(s + ((e + bl) + sl));
    } else {
        r = log(h);
    }
    return __cmplx(r, carg(z));
}

static void sqf(float a, float *hi, float *lo)
{
    float p = a * 4097.0f;                      /* 2^12 + 1 */
    float ah = p - (p - a), al = a - ah;
    *hi = a * a;
    *lo = ((ah * ah - *hi) + 2 * ah * al) + al * al;
}

float _Complex clogf(float _Complex z)
{
    float x = fabsf(__real__ z), y = fabsf(__imag__ z), h = hypotf(x, y), r;
    if (h > 0.7f && h < 1.4f) {
        float big = x > y ? x : y, small = x > y ? y : x;
        float bh, bl, sh, sl;
        sqf(big, &bh, &bl);
        sqf(small, &sh, &sl);
        float t = bh - 1, s = t + sh;
        float e = (t - (s - (s - t))) + (sh - (s - t));
        r = 0.5f * log1pf(s + ((e + bl) + sl));
    } else {
        r = logf(h);
    }
    return __cmplxf(r, cargf(z));
}

/* Where double is binary32 (RX), the double functions are the float ones
 * (msun/math_private.h): the same numbers, the same layout. */
#if __SIZEOF_DOUBLE__ == 4
#define AS_FLOAT(name)                                                  \
    double _Complex name(double _Complex z) { return name##f(z); }
AS_FLOAT(cexp)  AS_FLOAT(csqrt) AS_FLOAT(cproj)
AS_FLOAT(csin)  AS_FLOAT(ccos)  AS_FLOAT(ctan)
AS_FLOAT(csinh) AS_FLOAT(ccosh) AS_FLOAT(ctanh)
AS_FLOAT(casin) AS_FLOAT(cacos) AS_FLOAT(catan)
AS_FLOAT(casinh) AS_FLOAT(cacosh) AS_FLOAT(catanh)
double _Complex cpow(double _Complex x, double _Complex y)
{
    return cpowf(x, y);
}
#endif

/* ---- long double ---- */

long double (creall)(long double _Complex z) { return __real__ z; }
long double (cimagl)(long double _Complex z) { return __imag__ z; }
long double _Complex conjl(long double _Complex z)
{
    return __cmplxl(__real__ z, -__imag__ z);
}
/* Every infinity is the one point at infinity, the imaginary part's
 * sign kept; a finite value, however large, is itself. */
long double _Complex cprojl(long double _Complex z)
{
    if (isinf(__real__ z) || isinf(__imag__ z))
        return __cmplxl(INFINITY, copysignl(0.0L, __imag__ z));
    return z;
}

long double cabsl(long double _Complex z)
{
    return hypotl(__real__ z, __imag__ z);
}
long double cargl(long double _Complex z)
{
    return atan2l(__imag__ z, __real__ z);
}

static double _Complex to_d(long double _Complex z)
{
    return __cmplx((double)__real__ z, (double)__imag__ z);
}
static long double _Complex to_ld(double _Complex z)
{
    return __cmplxl(__real__ z, __imag__ z);
}
#define VIA_DOUBLE(name)                                                \
    long double _Complex name##l(long double _Complex z)                \
    {                                                                   \
        return to_ld(name(to_d(z)));                                    \
    }
VIA_DOUBLE(cexp)  VIA_DOUBLE(clog)  VIA_DOUBLE(csqrt)
VIA_DOUBLE(csin)  VIA_DOUBLE(ccos)  VIA_DOUBLE(ctan)
VIA_DOUBLE(csinh) VIA_DOUBLE(ccosh) VIA_DOUBLE(ctanh)
VIA_DOUBLE(casin) VIA_DOUBLE(cacos) VIA_DOUBLE(catan)
VIA_DOUBLE(casinh) VIA_DOUBLE(cacosh) VIA_DOUBLE(catanh)

long double _Complex cpowl(long double _Complex x, long double _Complex y)
{
    return to_ld(cpow(to_d(x), to_d(y)));
}
