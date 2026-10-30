/* The rest of glibc's <math.h>: the Bessel functions' other widths, the
 * old XSI and BSD names (gamma, scalb, significand, drem, finite, isinf
 * and isnan as functions), sincos, pow10, clog10, and C23's narrowing
 * arithmetic, total order, payloads, canonicalize and fromfp.
 *
 * The Bessel functions themselves are FreeBSD's (msun/e_j0.c and the
 * rest); their long double forms go through double, as every
 * transcendental function's does here (widths.c says why).
 *
 * ---- narrowing, correctly ------------------------------------------------
 *
 * fadd(x, y) is x + y rounded once, to float. Computing x + y in double
 * and then converting rounds twice, and is wrong for the few sums whose
 * double result lands exactly half-way between two floats. So each
 * operation finds its exact error (an add's by two-sum, a multiply's,
 * divide's and square root's by fma) and rounds the wide result to ODD:
 * truncated toward zero, with its last bit set when anything was lost.
 * A value rounded to odd with two bits to spare rounds correctly to the
 * narrow type, which double has for float and x87's 64 bits have for
 * double. ffma's error is not exact this way; it asks the floating-point
 * environment instead, rounding toward zero and reading FE_INEXACT.
 */
#define _GNU_SOURCE 1
#include <complex.h>
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fdlibm/fdlibm.h"

#define WEAK __attribute__((weak))

/* ---- Bessel: the other widths --------------------------------------------- */

#ifdef _DOUBLE_IS_32BITS
/* double is binary32: msun's double files are compiled away (msun/
 * math_private.h) and the double functions are the float ones */
double j0(double x) { return j0f((float)x); }
double j1(double x) { return j1f((float)x); }
double jn(int n, double x) { return jnf(n, (float)x); }
double y0(double x) { return y0f((float)x); }
double y1(double x) { return y1f((float)x); }
double yn(int n, double x) { return ynf(n, (float)x); }
#endif

WEAK long double j0l(long double x) { return j0((double)x); }
WEAK long double j1l(long double x) { return j1((double)x); }
WEAK long double jnl(int n, long double x) { return jn(n, (double)x); }
WEAK long double y0l(long double x) { return y0((double)x); }
WEAK long double y1l(long double x) { return y1((double)x); }
WEAK long double ynl(int n, long double x) { return yn(n, (double)x); }

/* ---- the old names -------------------------------------------------------------- */

WEAK double gamma(double x) { return lgamma(x); }
WEAK float gammaf(float x) { return lgammaf(x); }
WEAK long double gammal(long double x) { return lgammal(x); }

WEAK long double lgammal_r(long double x, int *sign)
{
    return lgamma_r((double)x, sign);
}

WEAK double drem(double x, double y) { return remainder(x, y); }
WEAK float dremf(float x, float y) { return remainderf(x, y); }
WEAK long double dreml(long double x, long double y) { return remainderl(x, y); }

WEAK double pow10(double x) { return exp10(x); }
WEAK float pow10f(float x) { return exp10f(x); }
WEAK long double pow10l(long double x) { return exp10l(x); }

/* x * 2^n for a whole n: NaN for a fractional one, as BSD's scalb. A
 * larger n than any exponent range is clamped to one that still
 * overflows or underflows -- within an int, which on AVR is 16 bits. */
#define SCALB_MAX (INT_MAX < 65000 ? INT_MAX : 65000)
#define SCALB(T, S)                                                         \
    WEAK T scalb##S(T x, T n)                                               \
    {                                                                       \
        if (isnan(x) || isnan(n))                                           \
            return x * n;                                                   \
        if (isinf(n))                                                       \
            return n > 0 ? x * n : x / -n;                                  \
        if (rint##S(n) != n)                                                \
            return (n - n) / (n - n);                                       \
        if (n > SCALB_MAX)                                                  \
            return scalbn##S(x, SCALB_MAX);                                 \
        if (n < -SCALB_MAX)                                                 \
            return scalbn##S(x, -SCALB_MAX);                                \
        return scalbn##S(x, (int)n);                                        \
    }
SCALB(double, ) SCALB(float, f) SCALB(long double, l)

/* the significand in [1, 2) */
#define SIGNIFICAND(T, S)                                                   \
    WEAK T significand##S(T x)                                              \
    {                                                                       \
        if (x == 0 || isinf(x) || isnan(x))                                 \
            return x;                                                       \
        return scalbn##S(x, -ilogb##S(x));                                  \
    }
SIGNIFICAND(double, ) SIGNIFICAND(float, f) SIGNIFICAND(long double, l)

WEAK void sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
WEAK void sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
WEAK void sincosl(long double x, long double *s, long double *c)
{
    *s = sinl(x);
    *c = cosl(x);
}

/* the function forms of the classification macros, BSD's and glibc's:
 * isinf's sign is that of the infinity */
#define CLASSIFY(T, S)                                                      \
    WEAK int (isinf##S)(T x)                                                \
    {                                                                       \
        return x == x && x - x != x - x ? (x < 0 ? -1 : 1) : 0;             \
    }                                                                       \
    WEAK int (isnan##S)(T x) { return x != x; }
CLASSIFY(double, ) CLASSIFY(float, f) CLASSIFY(long double, l)
WEAK int finitel(long double x) { return x - x == x - x; }   /* math.c has finite */

/* base-10 logarithm of a complex number */
WEAK double complex clog10(double complex z) { return clog(z) / M_LN10; }
WEAK float complex clog10f(float complex z) { return clogf(z) / (float)M_LN10; }
WEAK long double complex clog10l(long double complex z)
{
    return clogl(z) / 2.302585092994045684017991454684364208L;
}

/* by magnitude, and fmax's or fmin's answer when the magnitudes tie */
#define MAG(T, S)                                                           \
    WEAK T fmaxmag##S(T x, T y)                                             \
    {                                                                       \
        T ax = fabs##S(x), ay = fabs##S(y);                                 \
        return ax > ay ? x : ay > ax ? y : fmax##S(x, y);                   \
    }                                                                       \
    WEAK T fminmag##S(T x, T y)                                             \
    {                                                                       \
        T ax = fabs##S(x), ay = fabs##S(y);                                 \
        return ax < ay ? x : ay < ax ? y : fmin##S(x, y);                   \
    }                                                                       \
    WEAK long llogb##S(T x)                                                 \
    {                                                                       \
        int e = ilogb##S(x);                                                \
        return e == FP_ILOGB0 ? FP_LLOGB0 : e == FP_ILOGBNAN ? FP_LLOGBNAN  \
               : e == INT_MAX ? LONG_MAX : (long)e;                         \
    }
MAG(double, ) MAG(float, f) MAG(long double, l)

/* ---- narrowing arithmetic -------------------------------------------------------- */

/* w rounded to odd: err is the sign of what rounding w lost (0 when
 * nothing was), w the round-to-nearest result */
#define TO_ODD(T, NX)                                                       \
    static T odd_##NX(T w, int err)                                         \
    {                                                                       \
        if (!err || !isfinite(w))                                           \
            return w;                                                       \
        if ((err < 0) != (w < 0) && w != 0)                                 \
            w = nextafter##NX(w, (T)0);  /* rounded away from zero */       \
        unsigned char b[sizeof(T)];                                         \
        memcpy(b, &w, sizeof b);                                            \
        b[__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? 0 : sizeof b - 1] |= 1; \
        memcpy(&w, b, sizeof b);                                            \
        return w;                                                           \
    }
TO_ODD(double, )
TO_ODD(long double, l)

static int sgn_d(double v) { return (v > 0) - (v < 0); }
static int sgn_l(long double v) { return (v > 0) - (v < 0); }

/* two-sum: the exact error of a + b, which it returns the sign of */
static int add_err(double a, double b, double s)
{
    double bb = s - a, e = (a - (s - bb)) + (b - bb);
    return sgn_d(e);
}

static int add_errl(long double a, long double b, long double s)
{
    long double bb = s - a, e = (a - (s - bb)) + (b - bb);
    return sgn_l(e);
}

/* Each narrowing operation is the wide one, rounded to odd, converted.
 * N narrow type, W wide type, R the narrow name's prefix (f or d),
 * WS the wide functions' suffix, O the odd-rounder. */
#define NARROW(N, R, W, WS, NS, O, SGN, ADDERR)                             \
    WEAK N R##add##NS(W x, W y)                                             \
    {                                                                       \
        W s = x + y;                                                        \
        return (N)O(s, isfinite(s) ? ADDERR(x, y, s) : 0);                  \
    }                                                                       \
    WEAK N R##sub##NS(W x, W y)                                             \
    {                                                                       \
        W s = x - y;                                                        \
        return (N)O(s, isfinite(s) ? ADDERR(x, -y, s) : 0);                 \
    }                                                                       \
    WEAK N R##mul##NS(W x, W y)                                             \
    {                                                                       \
        W p = x * y;                                                        \
        return (N)O(p, isfinite(p) ? SGN(fma##WS(x, y, -p)) : 0);           \
    }                                                                       \
    WEAK N R##div##NS(W x, W y)                                             \
    {                                                                       \
        W q = x / y;                                                        \
        if (!isfinite(q) || y == 0)                                         \
            return (N)q;                                                    \
        W r = fma##WS(-q, y, x);   /* x - q y, exactly */                   \
        return (N)O(q, SGN(r) * SGN(y));                                    \
    }                                                                       \
    WEAK N R##sqrt##NS(W x)                                                 \
    {                                                                       \
        W r = sqrt##WS(x);                                                  \
        if (!isfinite(r) || r == 0)                                         \
            return (N)r;                                                    \
        return (N)O(r, SGN(fma##WS(-r, r, x)));                             \
    }                                                                       \
    WEAK N R##fma##NS(W x, W y, W z)                                        \
    {                                                                       \
        fenv_t env;                                                         \
        if (feholdexcept(&env) || fesetround(FE_TOWARDZERO)) {              \
            fesetenv(&env);                                                 \
            return (N)fma##WS(x, y, z);                                     \
        }                                                                   \
        W t = fma##WS(x, y, z);                                             \
        int inexact = fetestexcept(FE_INEXACT) != 0;                        \
        fesetenv(&env);                                                     \
        if (inexact && isfinite(t)) {                                       \
            unsigned char b[sizeof(W)];                                     \
            memcpy(b, &t, sizeof b);                                        \
            b[__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? 0 : sizeof b - 1] |= 1; \
            memcpy(&t, b, sizeof b);                                        \
        }                                                                   \
        return (N)t;                                                        \
    }

NARROW(float, f, double, , , odd_, sgn_d, add_err)
NARROW(float, f, long double, l, l, odd_l, sgn_l, add_errl)
#if LDBL_MANT_DIG > DBL_MANT_DIG
NARROW(double, d, long double, l, l, odd_l, sgn_l, add_errl)
#else
/* long double is double: nothing to narrow, each is the operation */
WEAK double daddl(long double x, long double y) { return (double)(x + y); }
WEAK double dsubl(long double x, long double y) { return (double)(x - y); }
WEAK double dmull(long double x, long double y) { return (double)(x * y); }
WEAK double ddivl(long double x, long double y) { return (double)(x / y); }
WEAK double dsqrtl(long double x) { return (double)sqrtl(x); }
WEAK double dfmal(long double x, long double y, long double z)
{
    return (double)fmal(x, y, z);
}
#endif

/* ---- total order, payloads, canonical encodings ------------------------------------
 *
 * Read from the bits. A value's key orders like an unsigned integer:
 * the sign bit flipped for a positive value, every bit flipped for a
 * negative one -- so -NaN < -inf < ... < -0 < +0 < ... < +inf < +NaN,
 * NaNs ordered by payload. */

static uint64_t key64(double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u >> 63 ? ~u : u | (1ull << 63);
}

static uint32_t key32(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof u);
    return u >> 31 ? ~u : u | ((uint32_t)1 << 31);
}

WEAK int totalorder(const double *x, const double *y) { return key64(*x) <= key64(*y); }
WEAK int totalorderf(const float *x, const float *y) { return key32(*x) <= key32(*y); }

WEAK int totalordermag(const double *x, const double *y)
{
    double a = fabs(*x), b = fabs(*y);
    return key64(a) <= key64(b);
}

WEAK int totalordermagf(const float *x, const float *y)
{
    float a = fabsf(*x), b = fabsf(*y);
    return key32(a) <= key32(b);
}

/* long double's: by width. x87's 80 bits are 64 of significand below 16
 * of sign and exponent; binary128 is 128 bits with the sign on top. */
#if LDBL_MANT_DIG == DBL_MANT_DIG
static void keyl(long double x, uint64_t k[2])
{
    k[0] = key64((double)x);
    k[1] = 0;
}
#else
static void keyl(long double x, uint64_t k[2])
{
    unsigned char b[sizeof x];
    memset(b, 0, sizeof b);
    memcpy(b, &x, sizeof x);
    uint64_t hi, lo;
#if LDBL_MANT_DIG == 64
    uint16_t se;
    memcpy(&lo, b, 8);
    memcpy(&se, b + 8, 2);
    hi = (uint64_t)se << 48;
#else
    memcpy(&lo, b, 8);
    memcpy(&hi, b + 8, 8);
#endif
    if (hi >> 63) {
        hi = ~hi;
        lo = ~lo;
    } else {
        hi |= 1ull << 63;
    }
    k[0] = hi;
    k[1] = lo;
}
#endif

WEAK int totalorderl(const long double *x, const long double *y)
{
    uint64_t a[2], b[2];
    keyl(*x, a);
    keyl(*y, b);
    return a[0] < b[0] || (a[0] == b[0] && a[1] <= b[1]);
}

WEAK int totalordermagl(const long double *x, const long double *y)
{
    long double p = fabsl(*x), q = fabsl(*y);
    return totalorderl(&p, &q);
}

/* the payload: the significand's bits below the quiet bit, as a number;
 * -1 for a value that is not a NaN */
WEAK double getpayload(const double *x)
{
    if (!isnan(*x))
        return -1;
    uint64_t u;
    memcpy(&u, x, sizeof u);
    return (double)(u & ((1ull << 51) - 1));
}

WEAK float getpayloadf(const float *x)
{
    if (!isnan(*x))
        return -1;
    uint32_t u;
    memcpy(&u, x, sizeof u);
    return (float)(u & (((uint32_t)1 << 22) - 1));
}

#define PAYLOAD(T, S, U, BITS, QUIET, EXPMASK)                              \
    static int setpl##S(T *res, T pl, int quiet)                            \
    {                                                                       \
        T lim = (T)((U)1 << (BITS));                                        \
        if (!(pl >= 0 && pl < lim && trunc##S(pl) == pl) ||                 \
            (!quiet && pl == 0)) {                                          \
            *res = 0;                                                       \
            return 1;                                                       \
        }                                                                   \
        U u = (U)pl | (EXPMASK) | (quiet ? (QUIET) : 0);                    \
        memcpy(res, &u, sizeof u);                                          \
        return 0;                                                           \
    }                                                                       \
    WEAK int setpayload##S(T *res, T pl) { return setpl##S(res, pl, 1); }   \
    WEAK int setpayloadsig##S(T *res, T pl) { return setpl##S(res, pl, 0); }
PAYLOAD(double, , uint64_t, 51, 1ull << 51, 0x7FFull << 52)
PAYLOAD(float, f, uint32_t, 22, (uint32_t)1 << 22, (uint32_t)0xFF << 23)

#if LDBL_MANT_DIG == DBL_MANT_DIG
WEAK long double getpayloadl(const long double *x)
{
    double d = (double)*x;
    return getpayload(&d);
}
WEAK int setpayloadl(long double *res, long double pl)
{
    double d;
    int r = setpayload(&d, (double)pl);
    *res = d;
    return r;
}
WEAK int setpayloadsigl(long double *res, long double pl)
{
    double d;
    int r = setpayloadsig(&d, (double)pl);
    *res = d;
    return r;
}
#elif LDBL_MANT_DIG == 64
/* x87: an explicit integer bit (63), the quiet bit (62), 62 of payload */
WEAK long double getpayloadl(const long double *x)
{
    if (!isnan(*x))
        return -1;
    uint64_t m;
    memcpy(&m, x, 8);
    return (long double)(m & ((1ull << 62) - 1));
}
static int setplx(long double *res, long double pl, int quiet)
{
    if (!(pl >= 0 && pl < 0x1p62L && truncl(pl) == pl) || (!quiet && pl == 0)) {
        *res = 0;
        return 1;
    }
    unsigned char b[sizeof(long double)];
    memset(b, 0, sizeof b);
    uint64_t m = (uint64_t)pl | 1ull << 63 | (quiet ? 1ull << 62 : 0);
    uint16_t se = 0x7FFF;
    memcpy(b, &m, 8);
    memcpy(b + 8, &se, 2);
    memcpy(res, b, sizeof b);
    return 0;
}
WEAK int setpayloadl(long double *res, long double pl) { return setplx(res, pl, 1); }
WEAK int setpayloadsigl(long double *res, long double pl) { return setplx(res, pl, 0); }
#else
/* binary128: 111 bits of payload below the quiet bit (111) */
WEAK long double getpayloadl(const long double *x)
{
    if (!isnan(*x))
        return -1;
    uint64_t lo, hi;
    memcpy(&lo, x, 8);
    memcpy(&hi, (const char *)x + 8, 8);
    hi &= (1ull << 47) - 1;
    return (long double)hi * 0x1p64L + (long double)lo;
}
static int setplq(long double *res, long double pl, int quiet)
{
    if (!(pl >= 0 && pl < 0x1p111L && truncl(pl) == pl) || (!quiet && pl == 0)) {
        *res = 0;
        return 1;
    }
    long double h = truncl(pl * 0x1p-64L);
    uint64_t hi = (uint64_t)h, lo = (uint64_t)(pl - h * 0x1p64L);
    hi |= 0x7FFFull << 48 | (quiet ? 1ull << 47 : 0);
    memcpy(res, &lo, 8);
    memcpy((char *)res + 8, &hi, 8);
    return 0;
}
WEAK int setpayloadl(long double *res, long double pl) { return setplq(res, pl, 1); }
WEAK int setpayloadsigl(long double *res, long double pl) { return setplq(res, pl, 0); }
#endif

/* the canonical encoding: every value here has one, except a signaling
 * NaN, which becomes quiet -- as any arithmetic on it would make it */
#define CANON(T, S)                                                         \
    WEAK int canonicalize##S(T *cx, const T *x)                             \
    {                                                                       \
        T v = *x;                                                           \
        *cx = isnan(v) ? v + v : v;                                         \
        return 0;                                                           \
    }
CANON(double, ) CANON(float, f) CANON(long double, l)

/* ---- fromfp: to an integer of a given width, in a given direction ----------------
 *
 * glibc 2.36's (TS 18661-1) forms, returning intmax_t or uintmax_t. A
 * NaN, an infinity, a width of 0, or a result outside `width` bits is a
 * domain error, and the result is then 0. The x forms also raise
 * FE_INEXACT when the integer is not x. */
#define ROUNDDIR(T, S)                                                      \
    static T round_dir##S(T x, int dir)                                     \
    {                                                                       \
        switch (dir) {                                                      \
        case FP_INT_UPWARD:            return ceil##S(x);                   \
        case FP_INT_DOWNWARD:          return floor##S(x);                  \
        case FP_INT_TOWARDZERO:        return trunc##S(x);                  \
        case FP_INT_TONEARESTFROMZERO: return round##S(x);                  \
        default:                       return roundeven##S(x);              \
        }                                                                   \
    }                                                                       \
    WEAK intmax_t fromfp##S(T x, int dir, unsigned width)                   \
    {                                                                       \
        if (dir < 0 || dir > FP_INT_TONEAREST || !isfinite(x) || !width)    \
            goto bad;                                                       \
        {                                                                   \
            T r = round_dir##S(x, dir);                                     \
            if (width > 64)                                                 \
                width = 64;                                                 \
            T lim = ldexp##S((T)1, (int)width - 1);                         \
            if (r < -lim || r >= lim)                                       \
                goto bad;                                                   \
            return (intmax_t)r;                                             \
        }                                                                   \
    bad:                                                                    \
        errno = EDOM;                                                       \
        feraiseexcept(FE_INVALID);                                          \
        return 0;                                                           \
    }                                                                       \
    WEAK uintmax_t ufromfp##S(T x, int dir, unsigned width)                 \
    {                                                                       \
        if (dir < 0 || dir > FP_INT_TONEAREST || !isfinite(x) || !width)    \
            goto bad;                                                       \
        {                                                                   \
            T r = round_dir##S(x, dir);                                     \
            if (width > 64)                                                 \
                width = 64;                                                 \
            T lim = ldexp##S((T)1, (int)width);                             \
            if (r < 0 || r >= lim)                                          \
                goto bad;                                                   \
            return (uintmax_t)r;                                            \
        }                                                                   \
    bad:                                                                    \
        errno = EDOM;                                                       \
        feraiseexcept(FE_INVALID);                                          \
        return 0;                                                           \
    }                                                                       \
    WEAK intmax_t fromfpx##S(T x, int dir, unsigned width)                  \
    {                                                                       \
        intmax_t v = fromfp##S(x, dir, width);                              \
        if (isfinite(x) && round_dir##S(x, dir) != x)                       \
            feraiseexcept(FE_INEXACT);                                      \
        return v;                                                           \
    }                                                                       \
    WEAK uintmax_t ufromfpx##S(T x, int dir, unsigned width)                \
    {                                                                       \
        uintmax_t v = ufromfp##S(x, dir, width);                            \
        if (isfinite(x) && round_dir##S(x, dir) != x)                       \
            feraiseexcept(FE_INEXACT);                                      \
        return v;                                                           \
    }
ROUNDDIR(double, ) ROUNDDIR(float, f) ROUNDDIR(long double, l)
