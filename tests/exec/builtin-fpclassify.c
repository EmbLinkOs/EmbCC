/* GCC's __builtin_fpclassify(nan, inf, normal, subnormal, zero, x) --
 * what glibc's fpclassify() expands to -- on float, double and long
 * double (a 16-byte one where the target has it): zero of either sign,
 * normal, subnormal, infinity and NaN each answer their own value, and x
 * is evaluated once. The class values are numbers of the test's own, as
 * the builtin takes any. */
// expect-exit: 42
#include <float.h>

enum { NAN_ = 11, INF_ = 22, NORM_ = 33, SUB_ = 44, ZERO_ = 55 };
#define CLS(x) __builtin_fpclassify(NAN_, INF_, NORM_, SUB_, ZERO_, x)

static int calls;
static double once(double v) { calls++; return v; }

int main(void)
{
    volatile double z = 0.0, one = 1.0, big = DBL_MAX, sub = DBL_MIN / 4;
    volatile float fz = 0.0f, fs = FLT_MIN / 2, fbig = FLT_MAX;
    volatile long double lz = 0.0L, ls = LDBL_MIN / 2, lbig = LDBL_MAX;
    int bad = 0;
    bad |= (CLS(z) != ZERO_ || CLS(-z) != ZERO_ || CLS(one) != NORM_ ||
            CLS(big * 2) != INF_ || CLS(z / z) != NAN_ || CLS(sub) != SUB_ ||
            CLS(-sub) != SUB_ || CLS(-big * 2) != INF_) << 0;
    bad |= (CLS(fz) != ZERO_ || CLS(fs) != SUB_ || CLS(fbig) != NORM_ ||
            CLS(fbig * 2) != INF_ || CLS(fz / fz) != NAN_) << 1;
    bad |= (CLS(lz) != ZERO_ || CLS(ls) != SUB_ || CLS(lbig) != NORM_ ||
            CLS(lbig * 2) != INF_ || CLS(lz / lz) != NAN_ ||
            CLS(1.0L) != NORM_) << 2;
    bad |= (CLS(once(2.0)) != NORM_ || calls != 1) << 3;
    return bad ? bad : 42;
}
