/* Complex multiply and divide.
 *
 * `a * b` on two complex numbers is four real multiplies and two adds,
 * which a compiler could inline -- and C99 Annex G is the reason no
 * compiler does. The naive formulas give NaN for cases that have a
 * perfectly good answer:
 *
 *     (inf + 0i) * (1 + 1i)      is inf + inf*i
 *                                 but (inf*1 - 0*1) = inf - 0 = inf,
 *                                 and the imaginary part is
 *                                 (inf*1 + 0*1) = inf. That one works.
 *     (inf + 0i) * (inf + 0i)    gives inf - 0*0, fine, but
 *     (inf + nan*i) * (2 + 0i)   gives nan for BOTH parts, when Annex G
 *                                 says an infinite operand makes the
 *                                 result infinite regardless of the
 *                                 other's NaNs.
 *
 * So the recovery pass below runs whenever the straightforward answer
 * comes out NaN in both parts: it replaces each infinity with a signed
 * one and each NaN with a signed zero, and multiplies again. That is
 * the algorithm Annex G specifies and libgcc implements, and it is why
 * this is a library routine.
 *
 * Divide is Smith's method: scale by whichever denominator part is
 * larger, so the intermediate products cannot overflow when both parts
 * are large or underflow when both are small.
 *
 * The names are libgcc's, so an object of ours links beside one of
 * theirs. `__mulsc3` is float, `__muldc3` is double; the long-double
 * pair is NOT here, because on aarch64 that is binary128 and there is
 * no soft-float implementation under it yet (D-014).
 */
#include "rt.h"

/* Neither <math.h> nor libm is available here -- this library is linked
 * by programs that may not use either -- so the three predicates are
 * written from their definitions. */
static int is_nan_d(double x) { return x != x; }
static int is_inf_d(double x)
{
    union { double d; u64 u; } v;
    v.d = x;
    return (v.u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}
static double copysign_d(double m, double s)
{
    union { double d; u64 u; } a, b;
    a.d = m; b.d = s;
    a.u = (a.u & 0x7FFFFFFFFFFFFFFFULL) | (b.u & 0x8000000000000000ULL);
    return a.d;
}
static double fabs_d(double x)
{
    union { double d; u64 u; } v;
    v.d = x;
    v.u &= 0x7FFFFFFFFFFFFFFFULL;
    return v.d;
}

static int is_nan_f(float x) { return x != x; }
static int is_inf_f(float x)
{
    union { float f; u32 u; } v;
    v.f = x;
    return (v.u & 0x7FFFFFFFU) == 0x7F800000U;
}
static float copysign_f(float m, float s)
{
    union { float f; u32 u; } a, b;
    a.f = m; b.f = s;
    a.u = (a.u & 0x7FFFFFFFU) | (b.u & 0x80000000U);
    return a.f;
}
static float fabs_f(float x)
{
    union { float f; u32 u; } v;
    v.f = x;
    v.u &= 0x7FFFFFFFU;
    return v.f;
}

double _Complex __muldc3(double a, double b, double c, double d)
{
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    double re = ac - bd, im = ad + bc;
    double _Complex z;

    if (is_nan_d(re) && is_nan_d(im)) {
        /* Annex G's recovery: an infinity anywhere makes the result
         * infinite, and the NaNs that got there are replaced by signed
         * zeroes so the signs of the parts still come out right. */
        int recalc = 0;
        if (is_inf_d(a) || is_inf_d(b)) {
            a = copysign_d(is_inf_d(a) ? 1.0 : 0.0, a);
            b = copysign_d(is_inf_d(b) ? 1.0 : 0.0, b);
            if (is_nan_d(c)) c = copysign_d(0.0, c);
            if (is_nan_d(d)) d = copysign_d(0.0, d);
            recalc = 1;
        }
        if (is_inf_d(c) || is_inf_d(d)) {
            c = copysign_d(is_inf_d(c) ? 1.0 : 0.0, c);
            d = copysign_d(is_inf_d(d) ? 1.0 : 0.0, d);
            if (is_nan_d(a)) a = copysign_d(0.0, a);
            if (is_nan_d(b)) b = copysign_d(0.0, b);
            recalc = 1;
        }
        if (!recalc &&
            (is_inf_d(ac) || is_inf_d(bd) || is_inf_d(ad) || is_inf_d(bc))) {
            if (is_nan_d(a)) a = copysign_d(0.0, a);
            if (is_nan_d(b)) b = copysign_d(0.0, b);
            if (is_nan_d(c)) c = copysign_d(0.0, c);
            if (is_nan_d(d)) d = copysign_d(0.0, d);
            recalc = 1;
        }
        if (recalc) {
            double inf = 1.0 / 0.0;
            re = inf * (a * c - b * d);
            im = inf * (a * d + b * c);
        }
    }
    __real__ z = re;
    __imag__ z = im;
    return z;
}

float _Complex __mulsc3(float a, float b, float c, float d)
{
    float ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    float re = ac - bd, im = ad + bc;
    float _Complex z;

    if (is_nan_f(re) && is_nan_f(im)) {
        int recalc = 0;
        if (is_inf_f(a) || is_inf_f(b)) {
            a = copysign_f(is_inf_f(a) ? 1.0f : 0.0f, a);
            b = copysign_f(is_inf_f(b) ? 1.0f : 0.0f, b);
            if (is_nan_f(c)) c = copysign_f(0.0f, c);
            if (is_nan_f(d)) d = copysign_f(0.0f, d);
            recalc = 1;
        }
        if (is_inf_f(c) || is_inf_f(d)) {
            c = copysign_f(is_inf_f(c) ? 1.0f : 0.0f, c);
            d = copysign_f(is_inf_f(d) ? 1.0f : 0.0f, d);
            if (is_nan_f(a)) a = copysign_f(0.0f, a);
            if (is_nan_f(b)) b = copysign_f(0.0f, b);
            recalc = 1;
        }
        if (!recalc &&
            (is_inf_f(ac) || is_inf_f(bd) || is_inf_f(ad) || is_inf_f(bc))) {
            if (is_nan_f(a)) a = copysign_f(0.0f, a);
            if (is_nan_f(b)) b = copysign_f(0.0f, b);
            if (is_nan_f(c)) c = copysign_f(0.0f, c);
            if (is_nan_f(d)) d = copysign_f(0.0f, d);
            recalc = 1;
        }
        if (recalc) {
            float inf = 1.0f / 0.0f;
            re = inf * (a * c - b * d);
            im = inf * (a * d + b * c);
        }
    }
    __real__ z = re;
    __imag__ z = im;
    return z;
}

/* Smith's method: divide through by the LARGER of the two denominator
 * parts, so the ratio is at most one and the products that follow can
 * neither overflow from two large parts nor underflow from two small
 * ones. The naive (ac+bd)/(c*c+d*d) squares the denominator, which
 * overflows for any c above about 1e154. */
double _Complex __divdc3(double a, double b, double c, double d)
{
    double re, im, denom, ratio;
    double _Complex z;

    /* A ZERO DENOMINATOR is decided here and not by the recovery
     * pass below, because the general formula destroys the
     * evidence first: with a = inf and c = d = 0 the imaginary
     * part computes `inf * 0`, which is NaN, while the real part
     * comes out inf -- so the "both parts are NaN" test never
     * fires and the NaN is kept. Annex G is explicit: a nonzero
     * finite or infinite numerator over a zero denominator is an
     * INFINITY. A zero over a zero stays NaN, which is what
     * multiplying the infinity by that zero gives. */
    if (c == 0.0 && d == 0.0 && !(is_nan_d(a) && is_nan_d(b))) {
        double inf = 1.0 / 0.0;
        __real__ z = copysign_d(inf, c) * a;
        __imag__ z = copysign_d(inf, c) * b;
        return z;
    }
    if (fabs_d(c) >= fabs_d(d)) {
        ratio = d / c;
        denom = c + d * ratio;
        re = (a + b * ratio) / denom;
        im = (b - a * ratio) / denom;
    } else {
        ratio = c / d;
        denom = c * ratio + d;
        re = (a * ratio + b) / denom;
        im = (b * ratio - a) / denom;
    }

    /* Annex G again: a finite numerator over a zero denominator is an
     * infinity with the numerator's sign, not a NaN, and an infinite
     * numerator over a finite denominator is an infinity. Recovering
     * only when both parts came out NaN keeps the ordinary path exact. */
    if (is_nan_d(re) && is_nan_d(im)) {
        double inf = 1.0 / 0.0;
        if ((is_inf_d(a) || is_inf_d(b)) &&
                   !is_inf_d(c) && !is_inf_d(d)) {
            a = copysign_d(is_inf_d(a) ? 1.0 : 0.0, a);
            b = copysign_d(is_inf_d(b) ? 1.0 : 0.0, b);
            re = inf * (a * c + b * d);
            im = inf * (b * c - a * d);
        } else if ((is_inf_d(c) || is_inf_d(d)) &&
                   !is_nan_d(a) && !is_nan_d(b)) {
            c = copysign_d(is_inf_d(c) ? 1.0 : 0.0, c);
            d = copysign_d(is_inf_d(d) ? 1.0 : 0.0, d);
            re = 0.0 * (a * c + b * d);
            im = 0.0 * (b * c - a * d);
        }
    }
    __real__ z = re;
    __imag__ z = im;
    return z;
}

float _Complex __divsc3(float a, float b, float c, float d)
{
    float re, im, denom, ratio;
    float _Complex z;

    /* A ZERO DENOMINATOR is decided here and not by the recovery
     * pass below, because the general formula destroys the
     * evidence first: with a = inf and c = d = 0 the imaginary
     * part computes `inf * 0`, which is NaN, while the real part
     * comes out inf -- so the "both parts are NaN" test never
     * fires and the NaN is kept. Annex G is explicit: a nonzero
     * finite or infinite numerator over a zero denominator is an
     * INFINITY. A zero over a zero stays NaN, which is what
     * multiplying the infinity by that zero gives. */
    if (c == 0.0f && d == 0.0f && !(is_nan_f(a) && is_nan_f(b))) {
        float inf = 1.0f / 0.0f;
        __real__ z = copysign_f(inf, c) * a;
        __imag__ z = copysign_f(inf, c) * b;
        return z;
    }
    if (fabs_f(c) >= fabs_f(d)) {
        ratio = d / c;
        denom = c + d * ratio;
        re = (a + b * ratio) / denom;
        im = (b - a * ratio) / denom;
    } else {
        ratio = c / d;
        denom = c * ratio + d;
        re = (a * ratio + b) / denom;
        im = (b * ratio - a) / denom;
    }

    if (is_nan_f(re) && is_nan_f(im)) {
        float inf = 1.0f / 0.0f;
        if ((is_inf_f(a) || is_inf_f(b)) &&
                   !is_inf_f(c) && !is_inf_f(d)) {
            a = copysign_f(is_inf_f(a) ? 1.0f : 0.0f, a);
            b = copysign_f(is_inf_f(b) ? 1.0f : 0.0f, b);
            re = inf * (a * c + b * d);
            im = inf * (b * c - a * d);
        } else if ((is_inf_f(c) || is_inf_f(d)) &&
                   !is_nan_f(a) && !is_nan_f(b)) {
            c = copysign_f(is_inf_f(c) ? 1.0f : 0.0f, c);
            d = copysign_f(is_inf_f(d) ? 1.0f : 0.0f, d);
            re = 0.0f * (a * c + b * d);
            im = 0.0f * (b * c - a * d);
        }
    }
    __real__ z = re;
    __imag__ z = im;
    return z;
}
