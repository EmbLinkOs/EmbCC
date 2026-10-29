/* GCC's __builtin_complex(re, im) and C11's CMPLX, CMPLXF and CMPLXL: a
 * complex from its two parts, whatever they are -- an infinite or NaN
 * imaginary part, a negative zero, which `re + im * I` cannot make (the
 * multiply turns 0 * inf into a NaN real part, and -0.0 + 0.0 into +0.0)
 * -- in a function and, of constants, in a static initializer, which C11
 * asks of CMPLX. */
// expect-exit: 42
#include <complex.h>
#include <math.h>

/* (a C library whose <complex.h> has no CMPLX for this compiler --
 * macOS's offers it to clang alone -- gets GCC's definition) */
#ifndef CMPLX
#define CMPLX(r, i)  __builtin_complex((double)(r), (double)(i))
#define CMPLXF(r, i) __builtin_complex((float)(r), (float)(i))
#define CMPLXL(r, i) __builtin_complex((long double)(r), (long double)(i))
#endif

static double _Complex sd = __builtin_complex(1.5, -2.0);
static float _Complex sf = __builtin_complex(3.0f, __builtin_inff());
static long double _Complex sl = __builtin_complex(-0.0L, 4.0L);
static double _Complex cm = CMPLX(-0.0, INFINITY);
static float _Complex cmf = CMPLXF(2, -1);
static long double _Complex cml = CMPLXL(0.5, -0.0);

static volatile double vr = 0.25, vi = -0.0, vinf = 1.0 / 0.0;
static volatile float fr = -1.0f, fi = 8.0f;
static volatile long double lr = 7.0L;
static int order;
static double first(void) { return order = order * 10 + 1, 1.0; }
static double second(void) { return order = order * 10 + 2, 2.0; }

int main(void)
{
    int bad = 0;
    bad |= (creal(sd) != 1.5 || cimag(sd) != -2.0) << 0;
    bad |= (crealf(sf) != 3.0f || !isinf(cimagf(sf)) || cimagf(sf) < 0) << 1;
    bad |= (creall(sl) != 0 || !signbit(creall(sl)) || cimagl(sl) != 4.0L) << 2;
    bad |= (creal(cm) != 0 || !signbit(creal(cm)) || !isinf(cimag(cm))) << 3;
    bad |= (crealf(cmf) != 2.0f || cimagf(cmf) != -1.0f) << 3;
    bad |= (creall(cml) != 0.5L || !signbit(cimagl(cml))) << 3;
    {
        double _Complex z = __builtin_complex(vr, vi);
        double _Complex w = __builtin_complex(0.0, (double)vinf);
        float _Complex f = __builtin_complex(fr, fi);
        long double _Complex l = __builtin_complex(lr, (long double)vi);
        bad |= (creal(z) != 0.25 || cimag(z) != 0 || !signbit(cimag(z))) << 4;
        /* the multiply form makes this real part 0 * inf, a NaN */
        bad |= (creal(w) != 0 || isnan(creal(w)) || !isinf(cimag(w))) << 4;
        bad |= (crealf(f) != -1.0f || cimagf(f) != 8.0f) << 5;
        bad |= (creall(l) != 7.0L || !signbit(cimagl(l))) << 5;
    }
    {
        /* each part evaluated once, the real part first */
        double _Complex z = __builtin_complex(first(), second());
        bad |= (creal(z) != 1.0 || cimag(z) != 2.0 || order != 12) << 6;
        double _Complex c = CMPLX(vr * 2, vr * 4);
        bad |= (creal(c) != 0.5 || cimag(c) != 1.0) << 6;
    }
    bad |= (sizeof __builtin_complex(1.0f, 2.0f) != sizeof(float _Complex) ||
            sizeof __builtin_complex(1.0L, 2.0L) != sizeof(long double _Complex)) << 7;
    return bad ? bad : 42;
}
