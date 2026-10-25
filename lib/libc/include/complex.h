/* <complex.h> — C11 §7.3.
 *
 * EmbCC implements `_Complex` natively (it is laid out, passed and returned
 * as a pair, which is its ABI on both targets), so this header is the
 * library half: the functions over those values. */
#ifndef _COMPLEX_H
#define _COMPLEX_H

#ifdef __cplusplus
extern "C" {
#endif

#define complex _Complex
#define _Complex_I (1.0iF)
#define I _Complex_I

/* C11 §7.3.9.3. A complex from two reals, without the multiply-by-I form,
 * which cannot make a value whose imaginary part is a NaN or an infinity
 * with the right sign. EmbCC makes __real__/__imag__ assignable, so this
 * builds the value directly -- here, in the header, so that a program
 * using CMPLX needs nothing of the library it links. (__cmplx and its
 * kin are in the library too, for objects built when CMPLX called them.) */
static __inline__ double _Complex __embcc_cmplx(double __r, double __i)
{
    double _Complex __z;
    __real__ __z = __r;
    __imag__ __z = __i;
    return __z;
}
static __inline__ float _Complex __embcc_cmplxf(float __r, float __i)
{
    float _Complex __z;
    __real__ __z = __r;
    __imag__ __z = __i;
    return __z;
}
static __inline__ long double _Complex
__embcc_cmplxl(long double __r, long double __i)
{
    long double _Complex __z;
    __real__ __z = __r;
    __imag__ __z = __i;
    return __z;
}
#define CMPLX(r, i)  __embcc_cmplx((double)(r), (double)(i))
#define CMPLXF(r, i) __embcc_cmplxf((float)(r), (float)(i))
#define CMPLXL(r, i) __embcc_cmplxl((long double)(r), (long double)(i))

/* §7.3.5-7.3.9, at each width */
double creal(double _Complex), cimag(double _Complex);
double cabs(double _Complex), carg(double _Complex);
double _Complex conj(double _Complex), cproj(double _Complex);
double _Complex cexp(double _Complex), clog(double _Complex);
double _Complex csqrt(double _Complex), cpow(double _Complex, double _Complex);
double _Complex csin(double _Complex), ccos(double _Complex), ctan(double _Complex);
double _Complex csinh(double _Complex), ccosh(double _Complex), ctanh(double _Complex);
double _Complex casin(double _Complex), cacos(double _Complex), catan(double _Complex);
double _Complex casinh(double _Complex), cacosh(double _Complex);
double _Complex catanh(double _Complex);

float crealf(float _Complex), cimagf(float _Complex);
float cabsf(float _Complex), cargf(float _Complex);
float _Complex conjf(float _Complex), cprojf(float _Complex);
float _Complex cexpf(float _Complex), clogf(float _Complex);
float _Complex csqrtf(float _Complex), cpowf(float _Complex, float _Complex);
float _Complex csinf(float _Complex), ccosf(float _Complex), ctanf(float _Complex);
float _Complex csinhf(float _Complex), ccoshf(float _Complex), ctanhf(float _Complex);
float _Complex casinf(float _Complex), cacosf(float _Complex), catanf(float _Complex);
float _Complex casinhf(float _Complex), cacoshf(float _Complex);
float _Complex catanhf(float _Complex);

long double creall(long double _Complex), cimagl(long double _Complex);
long double cabsl(long double _Complex), cargl(long double _Complex);
long double _Complex conjl(long double _Complex), cprojl(long double _Complex);
long double _Complex cexpl(long double _Complex), clogl(long double _Complex);
long double _Complex csqrtl(long double _Complex);
long double _Complex cpowl(long double _Complex, long double _Complex);
long double _Complex csinl(long double _Complex), ccosl(long double _Complex);
long double _Complex ctanl(long double _Complex);
long double _Complex csinhl(long double _Complex), ccoshl(long double _Complex);
long double _Complex ctanhl(long double _Complex);
long double _Complex casinl(long double _Complex), cacosl(long double _Complex);
long double _Complex catanl(long double _Complex);
long double _Complex casinhl(long double _Complex), cacoshl(long double _Complex);
long double _Complex catanhl(long double _Complex);

#ifdef __cplusplus
}
#endif

#endif
