/* log1p, asinh, acosh and atanh at the ends of their ranges: values from
 * mpmath at 50 digits, each within 8 ulps -- where double is binary64: on
 * RX's binary32 most of these arguments are other numbers or out of
 * range -- and the special values C11's Annex F gives, everywhere. log1p(+inf) was a
 * NaN (inf * inf/inf), and with it atanh(1), the real part of
 * catanh(1 + 0i); asinh(1e200) was a NaN and acosh(1e200) an infinity,
 * x*x overflowing under the root. */
// expect-exit: 42
#include <float.h>
#include <math.h>

/* Classified by hand, not with <math.h>'s macros: the x86-64 QEMU harness
 * compiles with lib/libc's headers and links newlib, which has none of the
 * functions those call. */
static int is_nan(double v) { return v != v; }
static int sign_of(double v) { return __builtin_signbit(v) != 0; }

enum { LOG1P, ASINH, ACOSH, ATANH };
struct ref { int fn; double x, want; };
static const struct ref refs[] = {
    { LOG1P, 1e-300, 1e-300 },
    { LOG1P, -0.5, -0.6931471805599453 },
    { LOG1P, 1e-05, 9.999950000333332e-06 },
    { LOG1P, 3.0, 1.3862943611198906 },
    { LOG1P, 1e+100, 230.25850929940458 },
    { LOG1P, 2.5e-17, 2.5e-17 },
    { LOG1P, -0.999999, -13.815510557935518 },
    { ASINH, 1e-300, 1e-300 },
    { ASINH, 0.5, 0.48121182505960347 },
    { ASINH, -3.0, -1.8184464592320668 },
    { ASINH, 10000000000.0, 23.7189981105004 },
    { ASINH, 1e+200, 461.2101657793691 },
    { ASINH, -1e+300, -691.4686750787737 },
    { ASINH, 2.0, 1.4436354751788103 },
    { ASINH, 2.5, 1.6472311463710958 },
    { ACOSH, 1.0000001, 0.0004472135919037347 },
    { ACOSH, 1.5, 0.9624236501192069 },
    { ACOSH, 2.0, 1.3169578969248168 },
    { ACOSH, 3.0, 1.762747174039086 },
    { ACOSH, 10000000000.0, 23.7189981105004 },
    { ACOSH, 1e+200, 461.2101657793691 },
    { ATANH, 1e-300, 1e-300 },
    { ATANH, 0.5, 0.5493061443340549 },
    { ATANH, -0.999999, -7.254328619247669 },
    { ATANH, 0.9999999999, 11.859499013855018 },
    { ATANH, 1e-05, 1.0000000000333334e-05 },
};

static double of(int fn, double x)
{
    switch (fn) {
    case LOG1P: return log1p(x);
    case ASINH: return asinh(x);
    case ACOSH: return acosh(x);
    default: return atanh(x);
    }
}

static int same(double a, double b)
{
    return a == b && sign_of(a) == sign_of(b);
}

int main(void)
{
    double inf = INFINITY;
    for (unsigned k = 0; DBL_MANT_DIG == 53 && k < sizeof refs / sizeof refs[0];
         k++) {
        const struct ref *r = &refs[k];
        double got = of(r->fn, r->x);
        if (!(fabs(got - r->want) <= 8 * DBL_EPSILON * fabs(r->want)))
            return 1 + (int)k;
    }
    if (!same(log1p(inf), inf)) return 60;
    if (!same(log1p(-1.0), -inf)) return 61;
    if (!same(log1p(-0.0), -0.0)) return 62;
    if (!is_nan(log1p(NAN))) return 63;
    if (!same(asinh(inf), inf) || !same(asinh(-inf), -inf)) return 64;
    if (!same(asinh(-0.0), -0.0) || !is_nan(asinh(NAN))) return 65;
    if (!same(acosh(inf), inf) || !same(acosh(1.0), 0.0)) return 66;
    if (!is_nan(acosh(0.5)) || !is_nan(acosh(NAN))) return 67;
    if (!same(atanh(1.0), inf) || !same(atanh(-1.0), -inf)) return 68;
    if (!same(atanh(-0.0), -0.0) || !is_nan(atanh(2.0))) return 69;
    if (!same(log1pf(INFINITY), inf) || !same(atanhf(1.0f), inf)) return 70;
    if (!same((double)asinhf(-INFINITY), -inf)) return 71;
    return 42;
}
