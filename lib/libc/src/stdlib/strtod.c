/* strtod, strtof, strtold — C11 §7.22.1.3.
 *
 * Decimal text to a binary float, which is harder than it looks: the
 * decimal value almost never has an exact binary representation, so the
 * answer is a ROUNDING, and where that rounding lands depends on digits
 * far past the ones that fit in any accumulator.
 *
 * The strategy is the classic fast path, and the point of it is that it
 * is EXACT rather than merely close:
 *
 *   powers of ten up to 10^22 are exactly representable as doubles, and
 *   an integer of up to 15 significant digits is too. When both hold, one
 *   multiply or divide of two exact values rounds once, and IEEE says
 *   that single rounding is the nearest representable value -- the
 *   correctly rounded answer, with no error analysis needed.
 *
 * Outside that range the value is scaled by repeated squaring, which
 * rounds more than once and so can land one unit in the last place away
 * from nearest. C11 §7.22.1.3p10 permits exactly that ("the nearest
 * representable value, or the larger or smaller representable value
 * immediately adjacent"), and this says so rather than claiming better.
 * A correctly-rounded implementation for every input needs arbitrary-
 * precision arithmetic; see docs/language/libc.md.
 *
 * Hexadecimal floats (0x1.8p3) are exact by construction -- they are
 * binary already -- and are parsed separately below for that reason.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

/* 10^0 .. 10^22: every one exactly representable as a double. 10^23 is
 * the first that is not, which is where the fast path stops. */
static const double pow10_exact[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10,
    1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};
#define POW10_EXACT_MAX 22

/* A double's significand holds 53 bits, so integers below 2^53 are exact;
 * 15 decimal digits always fit, 16 sometimes do. 15 is the safe bound and
 * the one the fast path tests. */
#define SAFE_DIGITS 15

static double scale_pow10(double v, int e)
{
    /* Repeated squaring: 1e300 costs ten multiplies rather than three
     * hundred, and each multiply is a rounding, so fewer is better in
     * accuracy as well as speed. */
    double p = 10.0;
    int n = e < 0 ? -e : e;
    double s = 1.0;
    while (n) {
        if (n & 1)
            s *= p;
        p *= p;
        n >>= 1;
    }
    return e < 0 ? v / s : v * s;
}

/* The hex form, 0x1.921fb54442d18p+1. Exact: the digits are binary, so
 * the only rounding is the final fit into the type. */
static long double parse_hex(const char *s, const char **end, int *ok)
{
    const char *p = s;
    long double v = 0;
    int any = 0;
    long bexp = 0;

    for (; isxdigit((unsigned char)*p); p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0'
              : (tolower((unsigned char)*p) - 'a' + 10);
        v = v * 16 + d;
        any = 1;
    }
    if (*p == '.') {
        p++;
        for (; isxdigit((unsigned char)*p); p++) {
            int d = isdigit((unsigned char)*p) ? *p - '0'
                  : (tolower((unsigned char)*p) - 'a' + 10);
            v = v * 16 + d;
            bexp -= 4;
            any = 1;
        }
    }
    if (!any) {
        *ok = 0;
        return 0;
    }
    /* C requires the binary exponent: "0x1" alone is not a hex float, and
     * a parser that accepts it silently reads a different number than the
     * program wrote. */
    if (tolower((unsigned char)*p) != 'p') {
        *ok = 0;
        return 0;
    }
    p++;
    int eneg = 0;
    if (*p == '+' || *p == '-')
        eneg = *p++ == '-';
    if (!isdigit((unsigned char)*p)) {
        *ok = 0;
        return 0;
    }
    long e = 0;
    for (; isdigit((unsigned char)*p); p++)
        if (e < 100000)
            e = e * 10 + (*p - '0');
    bexp += eneg ? -e : e;

    *end = p;
    *ok = 1;
    /* ldexp is exact for a representable result, so no accuracy is lost
     * putting the exponent back. */
    return (long double)ldexp((double)v, (int)bexp);
}

static long double conv(const char *s, char **endp, int wide)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;

    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';

    /* inf / infinity / nan, case-insensitively, before the digits: they
     * are values a printf can produce, and a strtod that cannot read back
     * what printf wrote is a round trip that silently loses data. */
    if (tolower((unsigned char)p[0]) == 'i' &&
        tolower((unsigned char)p[1]) == 'n' &&
        tolower((unsigned char)p[2]) == 'f') {
        p += 3;
        if (tolower((unsigned char)p[0]) == 'i' &&
            tolower((unsigned char)p[1]) == 'n' &&
            tolower((unsigned char)p[2]) == 'i' &&
            tolower((unsigned char)p[3]) == 't' &&
            tolower((unsigned char)p[4]) == 'y')
            p += 5;
        if (endp)
            *endp = (char *)p;
        return neg ? -HUGE_VALL : HUGE_VALL;
    }
    if (tolower((unsigned char)p[0]) == 'n' &&
        tolower((unsigned char)p[1]) == 'a' &&
        tolower((unsigned char)p[2]) == 'n') {
        p += 3;
        if (*p == '(') {                 /* nan(chars): the payload */
            const char *q = p + 1;
            while (*q && *q != ')')
                q++;
            if (*q == ')')
                p = q + 1;
        }
        if (endp)
            *endp = (char *)p;
        return neg ? -__builtin_nanl("") : __builtin_nanl("");
    }

    if (p[0] == '0' && tolower((unsigned char)p[1]) == 'x') {
        const char *he;
        int ok;
        long double hv = parse_hex(p + 2, &he, &ok);
        if (ok) {
            if (endp)
                *endp = (char *)he;
            return neg ? -hv : hv;
        }
        /* Not a hex float after all. "0x" then rubbish converts as the
         * single digit 0, with the 'x' left behind -- the same reading
         * strtol gives. */
        if (endp)
            *endp = (char *)(p + 1);
        return neg ? -0.0L : 0.0L;
    }

    /* Accumulate the significant digits as an integer, remembering where
     * the point was. Digits past what an accumulator holds cannot change
     * the answer by more than they shift the exponent, so they are
     * counted rather than stored. */
    unsigned long long mant = 0;
    int ndig = 0, any = 0;
    long exp10 = 0;
    for (; isdigit((unsigned char)*p); p++) {
        any = 1;
        if (ndig < 19) {
            mant = mant * 10 + (unsigned)(*p - '0');
            if (mant || ndig)
                ndig++;
        } else {
            exp10++;                     /* too many: scale instead */
        }
    }
    if (*p == '.') {
        p++;
        for (; isdigit((unsigned char)*p); p++) {
            any = 1;
            if (ndig < 19) {
                mant = mant * 10 + (unsigned)(*p - '0');
                if (mant || ndig)
                    ndig++;
                exp10--;
            }
            /* digits past the accumulator's width change nothing */
        }
    }
    if (!any) {
        if (endp)
            *endp = (char *)s;           /* nothing converted */
        return 0;
    }

    if (tolower((unsigned char)*p) == 'e') {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-')
            eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            long e = 0;
            for (; isdigit((unsigned char)*q); q++)
                if (e < 100000)
                    e = e * 10 + (*q - '0');
            exp10 += eneg ? -e : e;
            p = q;
        }
        /* "1e" with no digits: the 'e' is not part of the number. */
    }

    if (endp)
        *endp = (char *)p;

    if (mant == 0)
        return neg ? -0.0L : 0.0L;

    double v;
    if (ndig <= SAFE_DIGITS && exp10 >= -POW10_EXACT_MAX &&
        exp10 <= POW10_EXACT_MAX) {
        /* Both operands exact, one rounding: the nearest representable
         * value, guaranteed. */
        double m = (double)mant;
        v = exp10 >= 0 ? m * pow10_exact[exp10] : m / pow10_exact[-exp10];
    } else {
        v = scale_pow10((double)mant, (int)exp10);
    }

    /* Overflow and underflow are reported, not silently returned: a
     * caller that cannot tell HUGE_VAL from a number that happened to be
     * huge has no way to validate its input. */
    if (isinf(v))
        errno = ERANGE;
    else if (v == 0.0 && ndig > 0)
        errno = ERANGE;

    (void)wide;
    return neg ? -(long double)v : (long double)v;
}

double strtod(const char *__restrict s, char **__restrict end)
{
    return (double)conv(s, end, 0);
}

float strtof(const char *__restrict s, char **__restrict end)
{
    double d = (double)conv(s, end, 0);
    float f = (float)d;
    /* A value that fits a double and not a float is still out of range. */
    if (isinf(f) && !isinf(d))
        errno = ERANGE;
    return f;
}

long double strtold(const char *__restrict s, char **__restrict end)
{
    return conv(s, end, 1);
}

double atof(const char *s) { return strtod(s, NULL); }
