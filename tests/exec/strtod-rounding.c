/* strtod, strtof and strtold correctly rounded: the cases a decimal (or hex)
 * converter gets wrong -- midpoints and a digit past them, the subnormal and
 * overflow edges, 2.2250738585072011e-308, long inputs, a hex significand
 * wider than the type, the input macOS 27.2's strtod aborts on -- each
 * against the exact answer (~/EmbRef/scratch-tools/strtod's referee
 * computed these tables), and at long double's own width for the x87 and
 * binary128. scanf and wcstod give the same answers; and so does the
 * compiler, for the same text as a literal. The old strtod computed in
 * double and scaled by powers of ten: 65% of the referee's 414,612 cases
 * came out wrong, and every long double was a double. */
// expect-exit: 42
// needs-libc: EmbCC's strtod is what is asked; newlib's long double is not
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define INF HUGE_VALL

struct dcase { const char *s; double v; };
struct fcase { const char *s; float v; };
struct lcase { const char *s; long double v; };

static const struct dcase dcases[] = {
    { "2.2250738585072011e-308", 0xfffffffffffffp-1074 },
    { "2.2250738585072012e-308", 0x10000000000000p-1074 },
    { "4.9406564584124654e-324", 0x1p-1074 },
    { "2.4703282292062327e-324", 0.0 },
    { "2.4703282292062328e-324", 0x1p-1074 },
    { "1.7976931348623157e308", 0x1fffffffffffffp971 },
    { "1.7976931348623158e308", 0x1fffffffffffffp971 },
    { "9007199254740993", 0x10000000000000p1 },
    { "9007199254740993.000000000000000001", 0x10000000000001p1 },
    { "1.00000000000000011102230246251565404236316680908203125", 0x10000000000000p-52 },
    { "1.00000000000000011102230246251565404236316680908203126", 0x10000000000001p-52 },
    { "0.1", 0x1999999999999ap-56 },
    { "1e23", 0x152d02c7e14af6p24 },
    { "8.5e-324", 0x2p-1074 },
    { "123456789012345678901234567890e-30", 0x1f9add3746f65fp-56 },
    { "4.5200874236838313e+186", 0x109f18d8f16adfp568 },
    { "0x1.fffffffffffff7ffp1023", 0x1fffffffffffffp971 },
    { "0x1.00000000000008p0", 0x10000000000000p-52 },
    { "0x1.000000000000080000001p0", 0x10000000000001p-52 },
    { "0x1", 0x10000000000000p-52 },
    { "-0x.8p-1073", -0x1p-1074 },
    { "0.000000000009215456089201046196990319003816694021224975585937499999999", 0x1443d730000000p-89 },
    { "3.14159265358979323846264338327950288419716939937510582097494459", 0x1921fb54442d18p-51 },
};
static const struct fcase fcases[] = {
    { "1.00000005960464477539062499", 0x800000p-23f },
    { "1.000000059604644775390625", 0x800000p-23f },
    { "1.00000005960464477539062501", 0x800001p-23f },
    { "3.4028235677973366e38", 0xffffffp104f },
    { "3.4028235677973365e38", 0xffffffp104f },
    { "1.4012984643248170709e-45", 0x1p-149f },
    { "7.006492321624085354e-46", 0.0f },
    { "7.006492321624085355e-46", 0x1p-149f },
    { "0x1.000001p0", 0x800000p-23f },
    { "0x1.0000010000001p0", 0x800001p-23f },
    { "1.17549435e-38", 0x800000p-149f },
    { "0.1", 0xcccccdp-27f },
    { "16777217", 0x800000p1f },
};
static const struct lcase x87cases[] = {
    { "1.18973149535723176502e+4932", 0xffffffffffffffffp16320L },
    { "1.18973149535723176508575932662800702e4932", INF },
    { "3.64519953188247460253e-4951", 0x1p-16445L },
    { "1.82259976594123730126e-4951", 0.0L },
    { "0.1", 0xcccccccccccccccdp-67L },
    { "3.141592653589793238462643383279502884197169399375105820974944592", 0xc90fdaa22168c235p-62L },
    { "1.0000000000000000000542101086242752217003726400434970855712890625", 0x8000000000000000p-63L },
    { "1.00000000000000000005421010862427522170037264004349708557128906251", 0x8000000000000001p-63L },
    { "1.000000000000000000000000000000000096296497219361792652798897965090932", 0x8000000000000000p-63L },
    { "1e-4960", 0.0L },
    { "6.475175119438025110924438958227646552e-4966", 0.0L },
    { "1e4933", INF },
};
static const struct lcase qcases[] = {
    { "1.18973149535723176502e+4932", 0x1fffffffffffffffdf5f7837da5b2p16271L },
    { "1.18973149535723176508575932662800702e4932", 0x1ffffffffffffffffffffffffffffp16271L },
    { "3.64519953188247460253e-4951", 0x2000000000000p-16494L },
    { "1.82259976594123730126e-4951", 0x1000000000000p-16494L },
    { "0.1", 0x1999999999999999999999999999ap-116L },
    { "3.141592653589793238462643383279502884197169399375105820974944592", 0x1921fb54442d18469898cc51701b8p-111L },
    { "1.0000000000000000000542101086242752217003726400434970855712890625", 0x10000000000000001000000000000p-112L },
    { "1.00000000000000000005421010862427522170037264004349708557128906251", 0x10000000000000001000000000000p-112L },
    { "1.000000000000000000000000000000000096296497219361792652798897965090932", 0x10000000000000000000000000001p-112L },
    { "1e-4960", 0x25b44p-16494L },
    { "6.475175119438025110924438958227646552e-4966", 0x1p-16494L },
    { "1e4933", INF },
};

static int same_d(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }
static int same_f(float a, float b) { return memcmp(&a, &b, sizeof a) == 0; }
static int same_l(long double a, long double b)
{
    return a == b && signbit(a) == signbit(b);
}

int main(void)
{
    char *end;
    if (DBL_MANT_DIG == 53)
        for (unsigned i = 0; i < sizeof dcases / sizeof *dcases; i++) {
            double v = strtod(dcases[i].s, &end), w = 0;
            if (!same_d(v, dcases[i].v) || *end)
                return 1;
            if (sscanf(dcases[i].s, "%lf", &w) != 1 || !same_d(w, v))
                return 2;
        }
    for (unsigned i = 0; i < sizeof fcases / sizeof *fcases; i++) {
        float v = strtof(fcases[i].s, &end), w = 0;
        if (!same_f(v, fcases[i].v) || *end)
            return 3;
        if (sscanf(fcases[i].s, "%f", &w) != 1 || !same_f(w, v))
            return 4;
    }
#if LDBL_MANT_DIG == 64 || LDBL_MANT_DIG == 113
    const struct lcase *lc = LDBL_MANT_DIG == 64 ? x87cases : qcases;
    for (unsigned i = 0; i < sizeof x87cases / sizeof *x87cases; i++) {
        long double v = strtold(lc[i].s, &end), w = 0;
        if (!same_l(v, lc[i].v) || *end)
            return 5;
        if (sscanf(lc[i].s, "%Lf", &w) != 1 || !same_l(w, v))
            return 6;
    }
#endif

    /* the compiler's literal and the library's strtod, from the same text */
    if (DBL_MANT_DIG == 53 &&
        (!same_d(2.2250738585072011e-308, strtod("2.2250738585072011e-308", 0)) ||
         !same_d(0.000000000009215456089201046196990319003816694021224975585937499999999,
                 strtod("0.000000000009215456089201046196990319003816694021224975585937499999999", 0)) ||
         !same_d(4.5200874236838313e+186, strtod("4.5200874236838313e+186", 0))))
        return 7;
    if (!same_f(1.00000005960464477539062501f, strtof("1.00000005960464477539062501", 0)) ||
        !same_f(7.006492321624085355e-46f, strtof("7.006492321624085355e-46", 0)))
        return 8;

    /* errno: overflow, and underflow to a subnormal or zero */
    errno = 0;
    if (strtod("1e999", 0) != HUGE_VAL || errno != ERANGE)
        return 9;
    errno = 0;
    if (strtof("1e-50", 0) != 0 || errno != ERANGE)
        return 10;
    errno = 0;
    if (strtod("1.5", 0) != 1.5 || errno != 0)
        return 11;

    /* a hex float needs no binary exponent in strtod; "0x" alone is a 0 */
    if (strtod("0x1", &end) != 1 || *end || strtod("0xg", &end) != 0 ||
        strcmp(end, "xg") != 0)
        return 12;

    /* wcstod rounds by all of a long numeral's digits: 600 of them, the
     * last one past a midpoint */
    static wchar_t w[700];
    static char n[700];
    const char *mid = "1.00000000000000011102230246251565404236316680908203125";
    size_t k = strlen(mid);
    memcpy(n, mid, k);
    memset(n + k, '0', 600);
    n[k + 599] = '1';
    for (size_t i = 0; i < k + 600; i++)
        w[i] = (wchar_t)n[i];
    wchar_t *wend;
    if (DBL_MANT_DIG == 53 &&
        (!same_d(wcstod(w, &wend), 0x10000000000001p-52) || *wend ||
         !same_d(strtod(n, 0), 0x10000000000001p-52)))
        return 13;
    return 42;
}
