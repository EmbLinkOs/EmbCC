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
 * builds the value directly. */
double      _Complex __cmplx(double, double);
float       _Complex __cmplxf(float, float);
long double _Complex __cmplxl(long double, long double);
#define CMPLX(r, i)  __cmplx((double)(r), (double)(i))
#define CMPLXF(r, i) __cmplxf((float)(r), (float)(i))
#define CMPLXL(r, i) __cmplxl((long double)(r), (long double)(i))

double creal(double _Complex), cimag(double _Complex);
double cabs(double _Complex), carg(double _Complex);
double _Complex conj(double _Complex), cproj(double _Complex);
double _Complex cexp(double _Complex), clog(double _Complex);
double _Complex csqrt(double _Complex), cpow(double _Complex, double _Complex);
double _Complex csin(double _Complex), ccos(double _Complex), ctan(double _Complex);
double _Complex csinh(double _Complex), ccosh(double _Complex), ctanh(double _Complex);
double _Complex casin(double _Complex), cacos(double _Complex), catan(double _Complex);

float crealf(float _Complex), cimagf(float _Complex);
float cabsf(float _Complex), cargf(float _Complex);
float _Complex conjf(float _Complex), cprojf(float _Complex);
float _Complex cexpf(float _Complex), clogf(float _Complex);
float _Complex csqrtf(float _Complex), cpowf(float _Complex, float _Complex);

long double creall(long double _Complex), cimagl(long double _Complex);
long double cabsl(long double _Complex), cargl(long double _Complex);
long double _Complex conjl(long double _Complex);

#ifdef __cplusplus
}
#endif

#endif
