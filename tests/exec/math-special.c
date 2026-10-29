/* erf, erfc, lgamma and tgamma at full precision -- FreeBSD's now, where
 * they were a 1950s erf good to 1.5e-7 and a Lanczos sum -- with C23's
 * sinpi and the others, exact where the argument is (sinpi(1) is 0), and
 * ilogbl and logbl at long double's own range. References from mpmath at
 * 200 bits, within a few units in the last place, in double and in float.
 * And FP_ILOGB0, float_t and M_PI, which <math.h> did not have. */
// expect-exit: 42
// needs-libc: sinpi and the others are C23's, which newlib has not got
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

struct ref1 { double x, f, g; };
struct ref3 { double x, a, b, c; };

static const struct ref1 erfs[] = {
    { -0x1.c000000000000p+1, -0x1.ffffe710d565ep-1, 0x1.fffff3886ab2fp+0 },
    { -0x1.3333333333333p+0, -0x1.d214ac7b2d067p-1, 0x1.e90a563d96834p+0 },
    { -0x1.0000000000000p-1, -0x1.0a7ef5c18edd2p-1, 0x1.853f7ae0c76e9p+0 },
    { -0x1.4f8b588e368f1p-17, -0x1.7a9f084b5e44cp-17, 0x1.0000bd4f8425bp+0 },
    { 0x1.56e1fc2f8f359p-997, 0x1.82e6d98711d3ap-997, 0x1.0000000000000p+0 },
    { 0x1.999999999999ap-4, 0x1.cca5ea24fb334p-4, 0x1.c66b42bb60999p-1 },
    { 0x1.999999999999ap-2, 0x1.b6ac7c4b20a05p-2, 0x1.24a9c1da6fafdp-1 },
    { 0x1.b000000000000p-1, 0x1.88d1cd474a2e0p-1, 0x1.dcb8cae2d747fp-3 },
    { 0x1.0000000000000p+0, 0x1.af767a741088bp-1, 0x1.4226162fbddd5p-3 },
    { 0x1.4000000000000p+0, 0x1.d8865d98abe01p-1, 0x1.3bcd133aa0ffcp-4 },
    { 0x1.4000000000000p+1, 0x1.ffcaa8f4c9beap-1, 0x1.aab859b20ac9ep-12 },
    { 0x1.0000000000000p+2, 0x1.ffffff7b91176p-1, 0x1.08ddd13bd35e7p-26 },
    { 0x1.799999999999ap+2, 0x1.fffffffffffffp-1, 0x1.4b99601a343bep-54 },
    { 0x1.a000000000000p+2, 0x1.0000000000000p+0, 0x1.6ae172414cebap-65 },
    { 0x1.b000000000000p+4, 0x1.0000000000000p+0, 0x0.0000000019e0fp-1022 },
};
static const struct ref1 gams[] = {
    { 0x1.0624dd2f1a9fcp-10, 0x1.ba0f3807161acp+2, 0x1.f3b63e2d41242p+9 },
    { 0x1.0000000000000p-2, 0x1.49bbd81c16efbp+0, 0x1.d013fc47eeeeap+1 },
    { 0x1.0000000000000p-1, 0x1.250d048e7a1bdp-1, 0x1.c5bf891b4ef6bp+0 },
    { 0x1.8000000000000p+0, -0x1.eeb95b094c191p-4, 0x1.c5bf891b4ef6bp-1 },
    { 0x1.4000000000000p+1, 0x1.2383e809a67e8p-2, 0x1.544fa6d47b390p+0 },
    { 0x1.d99999999999ap+1, 0x1.6d9625e359b87p+0, 0x1.0aebf5759a454p+2 },
    { 0x1.3000000000000p+3, 0x1.760f04f64ba68p+3, 0x1.d1fc76454758ap+16 },
    { 0x1.4400000000000p+4, 0x1.40ac422d5a6a9p+5, 0x1.c6d19285a4ccap+57 },
    { 0x1.9200000000000p+6, 0x1.696f7f9481308p+8, 0x1.5b98374db8c0bp+521 },
    { 0x1.5500000000000p+7, 0x1.6000911686cd6p+9, 0x1.9589f849167a8p+1015 },
    { -0x1.0000000000000p-1, 0x1.43f89a3f0edd6p+0, -0x1.c5bf891b4ef6bp+1 },
    { -0x1.8000000000000p+0, 0x1.b858151820f86p-1, 0x1.2e7fb0bcdf4f2p+1 },
    { -0x1.6000000000000p+1, 0x1.261e6d250cf63p-8, -0x1.0126c7a2df451p+0 },
    { 0x1.5798ee2308c3ap-27, 0x1.26bb1bb9c8a88p+4, 0x1.7d783fdb0ee61p+26 },
    { 0x1.000001ad7f29bp+0, -0x1.efd30c8e518b9p-25, 0x1.fffffe102cf46p-1 },
    { 0x1.fffffe5280d65p+0, -0x1.6b2b405fa85a3p-25, 0x1.fffffe94d4c02p-1 },
};
static const struct ref3 pis[] = {
    { 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0 },
    { 0x1.999999999999ap-4, 0x1.3c6ef372fe950p-2, 0x1.e6f0e134454ffp-1, 0x1.4cb7bfb4961afp-2 },
    { 0x1.0000000000000p-2, 0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bcdp-1, 0x1.0000000000000p+0 },
    { 0x1.0000000000000p-1, 0x1.0000000000000p+0, 0x0.0p+0, INFINITY },
    { 0x1.8000000000000p-1, 0x1.6a09e667f3bcdp-1, -0x1.6a09e667f3bcdp-1, -0x1.0000000000000p+0 },
    { 0x1.0000000000000p+0, 0x0.0p+0, -0x1.0000000000000p+0, 0x0.0p+0 },
    { 0x1.4000000000000p+0, -0x1.6a09e667f3bcdp-1, -0x1.6a09e667f3bcdp-1, 0x1.0000000000000p+0 },
    { 0x1.4000000000000p+1, 0x1.0000000000000p+0, 0x0.0p+0, INFINITY },
    { -0x1.3333333333333p-2, -0x1.9e3779b97f4a8p-1, 0x1.2cf2304755a5ep-1, -0x1.605a90c73ab78p+0 },
    { 0x1.edd2f1a9fbe77p+6, -0x1.fb1dc28ea220dp-1, -0x1.1a31cbc19312ap-3, 0x1.cc0b248d8fc01p+2 },
    { 0x1.b7cdfd9d7bdbbp-34, 0x1.596bf8ce7631ep-32, 0x1.0000000000000p+0, 0x1.596bf8ce7631ep-32 },
    { 0x1.ffe5c91d14e3cp-2, 0x1.fffffe581a8d9p-1, 0x1.496b7bf8dc391p-12, 0x1.8de32904b046cp+11 },
};
static const struct ref3 invpis[] = {
    { 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p-1, 0x0.0p+0 },
    { 0x1.999999999999ap-4, 0x1.05322eb667bedp-5, 0x1.df59ba2933082p-2, 0x1.03e53b7a03528p-5 },
    { 0x1.0000000000000p-1, 0x1.5555555555555p-3, 0x1.5555555555555p-2, 0x1.2e4051d9df308p-3 },
    { -0x1.0000000000000p-1, -0x1.5555555555555p-3, 0x1.5555555555555p-1, -0x1.2e4051d9df308p-3 },
    { 0x1.8000000000000p-1, 0x1.146cdde5cc971p-2, 0x1.d726443466d1dp-3, 0x1.a37f5c4c419efp-3 },
    { 0x1.0000000000000p+0, 0x1.0000000000000p-1, 0x0.0p+0, 0x1.0000000000000p-2 },
    { -0x1.0000000000000p+0, -0x1.0000000000000p-1, 0x1.0000000000000p+0, -0x1.0000000000000p-2 },
    { 0x1.12e0be826d695p-30, 0x1.5dfc448e90628p-32, 0x1.fffffffa880efp-2, 0x1.5dfc448e90628p-32 },
    { 0x1.ffffde7210be9p-1, 0x1.ff89fe6516678p-2, 0x1.d8066ba661e3cp-12, 0x1.ffffeaa3799dap-3 },
};

static uint64_t key(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    return u >> 63 ? ~u : u | (uint64_t)1 << 63;
}
static int near(double got, double want, unsigned ulps)
{
    if (want != want)
        return got != got;
    if (want - want != 0 || got - got != 0)
        return got == want;
    uint64_t a = key(got), b = key(want);
    return (a > b ? a - b : b - a) <= ulps;
}
static int nearf(float got, double want, unsigned ulps)
{
    float w = (float)want;
    uint32_t a, b;
    if (w - w != 0 || got - got != 0)
        return got == w;
    memcpy(&a, &got, 4);
    memcpy(&b, &w, 4);
    a = a >> 31 ? ~a : a | 0x80000000u;
    b = b >> 31 ? ~b : b | 0x80000000u;
    return (a > b ? a - b : b - a) <= ulps;
}

int main(void)
{
    if (DBL_MANT_DIG == 53) {
        for (unsigned i = 0; i < sizeof erfs / sizeof *erfs; i++)
            if (!near(erf(erfs[i].x), erfs[i].f, 2) ||
                !near(erfc(erfs[i].x), erfs[i].g, 2))
                return 1;
        for (unsigned i = 0; i < sizeof gams / sizeof *gams; i++)
            /* a negative argument goes through the reflection formula,
             * which loses digits beside Gamma's zero crossings: 93 ulp at
             * -2.75, as FreeBSD's has it */
            if (!near(lgamma(gams[i].x), gams[i].f, gams[i].x < 0 ? 128 : 4) ||
                !near(tgamma(gams[i].x), gams[i].g, 4))
                return 2;
        for (unsigned i = 0; i < sizeof pis / sizeof *pis; i++)
            if (!near(sinpi(pis[i].x), pis[i].a, 2) ||
                !near(cospi(pis[i].x), pis[i].b, 2) ||
                !near(tanpi(pis[i].x), pis[i].c, 2))
                return 3;
        for (unsigned i = 0; i < sizeof invpis / sizeof *invpis; i++)
            if (!near(asinpi(invpis[i].x), invpis[i].a, 2) ||
                !near(acospi(invpis[i].x), invpis[i].b, 2) ||
                !near(atanpi(invpis[i].x), invpis[i].c, 2))
                return 4;
    }
    for (unsigned i = 0; i < sizeof erfs / sizeof *erfs; i++)
        if (!nearf(erff((float)erfs[i].x), erf((float)erfs[i].x), 2) ||
            !nearf(erfcf((float)erfs[i].x), erfc((float)erfs[i].x), 2))
            return 5;
    for (unsigned i = 0; i < sizeof gams / sizeof *gams; i++)
        if (!nearf(tgammaf((float)gams[i].x), tgamma((float)gams[i].x), 2) ||
            !nearf(lgammaf((float)gams[i].x), lgamma((float)gams[i].x),
                   gams[i].x < 0 ? 256 : 2))
            return 6;
    /* exactly, where the argument is a multiple of a half or a quarter */
    if (sinpi(1.0) != 0 || signbit(sinpi(1.0)) || cospi(0.5) != 0 ||
        sinpi(0.5) != 1 || tanpi(0.25) != 1 || sinpif(-2.0f) != 0 ||
        !signbit(sinpif(-2.0f)) || cospif(1.0f) != -1)
        return 7;
    /* the sign lgamma leaves */
    signgam = 0;
    (void)lgamma(-0.5);
    int s1 = signgam;
    int s2 = 0;
    (void)lgamma_r(-1.5, &s2);
    if (s1 != -1 || s2 != 1)
        return 8;
    /* tgamma's poles and edges */
    if (!isinf(tgamma(0.0)) || !signbit(tgamma(-0.0)) || !isnan(tgamma(-1.0)) ||
        tgamma(1.0) != 1 || tgamma(5.0) != 24 || !isinf(lgamma(0.0)))
        return 9;
    /* ilogb's answers for zero and NaN, and at long double's range */
    if (ilogb(0.0) != FP_ILOGB0 || FP_ILOGB0 != INT_MIN ||
        ilogb(NAN) != FP_ILOGBNAN || ilogb(INFINITY) != INT_MAX ||
        ilogbl(0.0L) != FP_ILOGB0 || ilogbl(LDBL_MAX) != LDBL_MAX_EXP - 1 ||
        ilogbl(LDBL_TRUE_MIN) != LDBL_MIN_EXP - LDBL_MANT_DIG ||
        logbl(LDBL_MIN) != LDBL_MIN_EXP - 1)
        return 10;
    float_t ft = 1.0f;
    double_t dt = 2.0;
    if (sizeof ft != sizeof(float) || sizeof dt != sizeof(double) ||
        M_PI != 3.14159265358979323846 || M_SQRT2 * M_SQRT1_2 > 1.0000001)
        return 11;
    return 42;
}
