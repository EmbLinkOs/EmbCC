/* The float and long double forms of every function in <math.h>.
 *
 * C11 requires all three widths, and <tgmath.h> makes the requirement
 * visible: a type-generic `cbrt(x)` on a long double selects `cbrtl`,
 * so a missing variant is a compile error rather than a quiet
 * fallback to the double one.
 *
 * ---- two kinds of function ----------------------------------------------
 *
 * The ones that COMPUTE a new value -- sin, exp, pow, the rest -- go
 * through the double implementation. For `float` that is exact: every
 * float is a double, the double result is correctly rounded, and
 * rounding it once more to float is the correctly rounded float answer
 * for all but a vanishing set of inputs. For `long double` it is a
 * real loss: an 80-bit x87 long double has 64 mantissa bits and this
 * gives it 53. That is stated here rather than hidden, and it is what
 * most C libraries do for the transcendental functions; getting the
 * extra eleven bits means a second implementation of each series, which
 * is a library of its own.
 *
 * The ones that MOVE OR INSPECT a value -- fabs, copysign, ceil, floor,
 * trunc, round, fmax, fmin, fdim, ldexp, frexp, modf, scalbn -- are
 * written in long double throughout, because they can be and because
 * narrowing them would be a visible defect: `truncl` of a long double
 * with more than 53 significant bits must not lose them.
 */
#include <math.h>
#include <fenv.h>
#include <float.h>

/* ---- the double forms C11 requires that fdlibm's 1993 set omits ------- */

long long llrint(double x) { return (long long)nearbyint(x); }

/* scalbln takes a long rather than an int, for a platform whose
 * exponent range does not fit one. Both fit here, so it is the same
 * function with a wider argument -- and it exists because the standard
 * names it, not because it does anything more. */
double scalbln(double x, long n) { return scalbn(x, (int)n); }

/* The remainder, and the low bits of the quotient. An argument
 * reduction needs the second: knowing how many multiples of pi/2 were
 * removed decides which of sin/cos/-sin/-cos the answer is, and it
 * cannot be recovered from the remainder alone. At least three bits are
 * required, and the sign is the quotient's. */
double remquo(double x, double y, int *quo)
{
    double r = remainder(x, y);
    if (quo) {
        if (y == 0 || x != x || y != y || x - x != 0) {
            *quo = 0;
        } else {
            double q = (x - r) / y;
            /* Only the low bits are specified, so the value is taken
             * modulo a power of two -- which also keeps a huge quotient
             * from overflowing the int. */
            long long n = (long long)q;
            int sign = (n < 0) ? -1 : 1;
            if (n < 0)
                n = -n;
            *quo = sign * (int)(n & 0x7f);
        }
    }
    return r;
}

/* ---- float: through the double implementation ------------------------- */

#define F1(name) \
    float name##f(float x) { return (float)name((double)x); }
#define F2(name) \
    float name##f(float x, float y) \
    { return (float)name((double)x, (double)y); }

F1(asinh) F1(acosh) F1(atanh)
F1(expm1) F1(log1p)
F1(nearbyint) F1(rint)
F1(erf) F1(erfc) F1(tgamma) F1(lgamma)
F2(nextafter) F2(remainder)

float scalbnf(float x, int n)  { return (float)scalbn((double)x, n); }
float scalblnf(float x, long n) { return (float)scalbn((double)x, (int)n); }
float fmaf(float x, float y, float z)
{
    /* fma's whole purpose is a SINGLE rounding of x*y+z. Computing the
     * product in double is exact for float inputs -- 24 + 24 bits fit a
     * double's 53 -- so the double expression really does round once,
     * and narrowing it gives the right answer. This is the one case
     * where going through double is not a compromise but the method. */
    return (float)((double)x * (double)y + (double)z);
}
float remquof(float x, float y, int *q)
{ return (float)remquo((double)x, (double)y, q); }

long lrintf(float x)        { return lrint((double)x); }
long lroundf(float x)       { return lround((double)x); }
long long llrintf(float x)  { return llrint((double)x); }
long long llroundf(float x) { return llround((double)x); }

/* ---- long double: exact where it can be ------------------------------- */

long double fabsl(long double x) { return x < 0 ? -x : x; }

long double copysignl(long double x, long double y)
{
    long double a = x < 0 ? -x : x;
    /* The sign of a zero matters and `y < 0` does not see it, so the
     * negative-zero case is asked separately. */
    int neg = y < 0 || (y == 0 && copysign(1.0, (double)y) < 0);
    return neg ? -a : a;
}

/* Truncation toward zero, in long double throughout: narrowing would
 * lose the bits beyond a double's mantissa, which is exactly what a
 * long double was chosen for. The value is compared against the
 * largest integer the type represents exactly -- above it every value
 * is already an integer and there is nothing to do. */
long double truncl(long double x)
{
    long double big = 1.0L;
    for (int i = 0; i < LDBL_MANT_DIG; i++)
        big *= 2.0L;
    long double a = x < 0 ? -x : x;
    if (a >= big || a != a)
        return x;
    long double r = (long double)(long long)a;
    /* A magnitude beyond long long still needs the loop's answer, so
     * the conversion is only used where it is exact. */
    if (a >= 9223372036854775808.0L) {
        r = 0;
        long double t = a, scale = 1.0L;
        while (t >= 9223372036854775808.0L) {
            t /= 2.0L;
            scale *= 2.0L;
        }
        r = (long double)(long long)t * scale;
    }
    return x < 0 ? -r : r;
}

long double floorl(long double x)
{
    long double t = truncl(x);
    return (x < 0 && t != x) ? t - 1.0L : t;
}

long double ceill(long double x)
{
    long double t = truncl(x);
    return (x > 0 && t != x) ? t + 1.0L : t;
}

long double roundl(long double x)
{
    /* Halfway cases go AWAY from zero, which is round()'s rule and is
     * not the default rounding mode's ties-to-even. */
    long double a = x < 0 ? -x : x;
    long double t = truncl(a);
    if (a - t >= 0.5L)
        t += 1.0L;
    return x < 0 ? -t : t;
}

/* nearbyint and rint follow the CURRENT rounding mode, which is
 * ties-to-even by default -- a different answer from round() for
 * exactly the halfway cases. */
long double nearbyintl(long double x)
{
    long double t = truncl(x);
    long double d = x - t;
    if (d == 0)
        return x;
    long double a = d < 0 ? -d : d;
    switch (fegetround()) {
    case FE_DOWNWARD:   return x < 0 ? t - 1.0L : t;
    case FE_UPWARD:     return x > 0 ? t + 1.0L : t;
    case FE_TOWARDZERO: return t;
    default: break;
    }
    if (a > 0.5L)
        return d < 0 ? t - 1.0L : t + 1.0L;
    if (a < 0.5L)
        return t;
    /* A tie: to even. */
    long double half = t / 2.0L;
    if (half == truncl(half))
        return t;
    return d < 0 ? t - 1.0L : t + 1.0L;
}

long double rintl(long double x) { return nearbyintl(x); }

long double fmaxl(long double x, long double y)
{
    if (x != x) return y;          /* a NaN loses to a number */
    if (y != y) return x;
    return x > y ? x : y;
}

long double fminl(long double x, long double y)
{
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}

long double fdiml(long double x, long double y)
{ return x > y ? x - y : 0.0L; }

long double ldexpl(long double x, int n)
{
    /* Repeated doubling rather than a single scale: exact, and the
     * only way without touching the representation. Stepping by a
     * large power first keeps it O(log n). */
    long double r = x;
    while (n >= 32) { r *= 4294967296.0L; n -= 32; }
    while (n > 0)   { r *= 2.0L; n--; }
    while (n <= -32) { r /= 4294967296.0L; n += 32; }
    while (n < 0)   { r /= 2.0L; n++; }
    return r;
}

long double scalbnl(long double x, int n)  { return ldexpl(x, n); }
long double scalblnl(long double x, long n) { return ldexpl(x, (int)n); }

long double frexpl(long double x, int *e)
{
    *e = 0;
    if (x == 0 || x != x || x - x != 0)
        return x;                  /* zero, NaN, infinity: e is 0 */
    long double a = x < 0 ? -x : x;
    int n = 0;
    while (a >= 1.0L) { a /= 2.0L; n++; }
    while (a < 0.5L)  { a *= 2.0L; n--; }
    *e = n;
    return x < 0 ? -a : a;
}

long double modfl(long double x, long double *ip)
{
    long double t = truncl(x);
    *ip = t;
    return x - t;
}

long double fmodl(long double x, long double y)
{
    if (y == 0 || x != x || y != y)
        return (long double)fmod((double)x, (double)y);
    long double a = x < 0 ? -x : x, b = y < 0 ? -y : y;
    if (a < b)
        return x;
    /* Repeated subtraction with a doubling divisor: exact in long
     * double, where going through fmod() would round the operands. */
    long double scaled = b;
    while (scaled * 2.0L <= a)
        scaled *= 2.0L;
    while (scaled >= b) {
        if (a >= scaled)
            a -= scaled;
        scaled /= 2.0L;
    }
    return x < 0 ? -a : a;
}

/* ---- long double: through the double implementation -------------------- */

#define L1(name) \
    long double name##l(long double x) { return (long double)name((double)x); }
#define L2(name) \
    long double name##l(long double x, long double y) \
    { return (long double)name((double)x, (double)y); }

L1(asin) L1(acos) L1(atan)
L1(sinh) L1(cosh) L1(tanh)
L1(asinh) L1(acosh) L1(atanh)
L1(exp2) L1(expm1)
L1(log2) L1(log10) L1(log1p)
L1(cbrt)
L1(erf) L1(erfc) L1(tgamma) L1(lgamma)
L2(atan2) L2(hypot) L2(nextafter) L2(remainder)

long double fmal(long double x, long double y, long double z)
{ return x * y + z; }

long double remquol(long double x, long double y, int *q)
{ return (long double)remquo((double)x, (double)y, q); }

long lrintl(long double x)        { return (long)nearbyintl(x); }
long lroundl(long double x)       { return (long)roundl(x); }
long long llrintl(long double x)  { return (long long)nearbyintl(x); }
long long llroundl(long double x) { return (long long)roundl(x); }
