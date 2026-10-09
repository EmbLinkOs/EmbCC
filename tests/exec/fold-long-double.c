/* Long double arithmetic on constants is folded at -O1 and above, in the
 * target's own format -- x87 extended on x86-64, IEEE binary128 on
 * AArch64, RV64 and the rest -- and the folded bits must be the machine's.
 * Each case is computed twice, once from constants the optimizer folds
 * and once from volatile operands the machine computes (an x87 operation,
 * or a binary128 library call), and the two compared over the format's
 * significant bytes: rounding to nearest-even at 64 and 113 bits,
 * subnormals, overflow to infinity, signed zeros, division by zero, the
 * conversions to and from double, float and the integers, and a value
 * carried through locals. Where long double is double this repeats the
 * double cases, which must hold as well. */
// expect-exit: 42
#include <float.h>

static int same(const void *p, const void *q, int n)
{
    const unsigned char *a = p, *b = q;
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
/* x87's ten bytes carry the value; the rest of its sixteen are padding */
#define LDBYTES (LDBL_MANT_DIG == 64 ? 10 : (int)sizeof(long double))
static int bad, line;
#define FAIL() do { if (!bad) line = __LINE__; bad++; } while (0)
#define CASE(op, x, y) do { \
        volatile long double vx = (x), vy = (y); \
        long double folded = (long double)(x) op (long double)(y); \
        long double run = vx op vy; \
        if (!same(&folded, &run, LDBYTES)) FAIL(); \
    } while (0)
#define ALL(x, y) do { \
        CASE(+, x, y); CASE(-, x, y); CASE(*, x, y); CASE(/, x, y); \
    } while (0)
#define CONV(T, x) do { \
        volatile long double vx = (x); \
        T folded = (T)(long double)(x), run = (T)vx; \
        if (!same(&folded, &run, sizeof(T))) FAIL(); \
    } while (0)
#define TOLD(T, x) do { \
        volatile T vx = (x); \
        long double folded = (long double)(T)(x), run = (long double)vx; \
        if (!same(&folded, &run, LDBYTES)) FAIL(); \
    } while (0)
/* the same conversion from a local, which the front end leaves to the
 * optimizer: an IR conversion whose operand is a constant */
#define LOCAL(T, x) do { \
        T t = (x); volatile T vx = (x); \
        long double folded = t, run = vx; \
        if (!same(&folded, &run, LDBYTES)) FAIL(); \
    } while (0)

int main(void)
{
    ALL(1.0L, 3.0L);
    ALL(0.1L, 0.2L);
    ALL(2.0L, 3.0L);
    ALL(18446744073709551616.0L, 1.0L);      /* 2^64 + 1: x87's ties */
    ALL(18446744073709551616.0L, 3.0L);
    ALL(10384593717069655257060992658440192.0L, 1.0L);  /* 2^113 + 1 */
    ALL(LDBL_MIN, 1e10L);                    /* into the subnormals */
    ALL(LDBL_MIN, 0.75L);
    ALL(LDBL_MAX, 2.0L);                     /* overflow to infinity */
    ALL(LDBL_MAX, LDBL_MAX);
    ALL(-0.0L, 0.0L);                        /* signed zeros */
    ALL(-0.0L, -0.0L);
    ALL(5.0L, 0.0L);                         /* division by zero */
    ALL(-7.125L, 1e300L);
    ALL(1e-300L, 1e300L);
    ALL(0.3L, -0.7L);
    {   /* negation */
        volatile long double v = 2.5L;
        long double n = -(2.5L), r = -v;
        if (!same(&n, &r, LDBYTES)) FAIL();
    }
    /* narrowing, rounded once from the exact value: a tie at double's
     * last bit goes to even, which a double rounding would not */
    CONV(double, 1.0L / 3.0L);
    CONV(double, 1.0L + 0x1p-53L);
    CONV(double, 1.0L + 0x1p-53L + 0x1p-60L);
    CONV(double, LDBL_MAX);
    CONV(double, LDBL_MIN);
    CONV(float, 1.0L / 3.0L);
    CONV(float, 1.0L + 0x1p-24L);
    CONV(float, 0.1L);
    /* widening: exact */
    TOLD(double, 0.1);
    TOLD(double, 1.7976931348623157e308);
    TOLD(double, 4.9e-324);
    TOLD(float, 0.1f);
    TOLD(long long, 9223372036854775807LL);  /* exact in 64 bits, not in 53 */
    TOLD(long long, -9223372036854775807LL - 1);
    TOLD(long long, 123456789012345678LL);
    TOLD(unsigned long long, 18446744073709551615ULL);
    TOLD(unsigned long long, 9223372036854775809ULL);
    TOLD(int, -2147483647 - 1);
    TOLD(unsigned, 4294967295u);
    LOCAL(unsigned, 4294967295u);
    LOCAL(unsigned, 2147483648u);
    LOCAL(int, -2147483647 - 1);
    LOCAL(unsigned long long, 18446744073709551615ULL);
    LOCAL(unsigned long long, 9223372036854775809ULL);
    LOCAL(long long, -5);
    LOCAL(long long, 9223372036854775807LL);
    LOCAL(double, 0.1);
    LOCAL(float, -0.0f);
    {   /* through locals: each step rounded as the machine rounds it */
        long double a = 2.0L, b = a * 3.0L;
        b = b - 0.1L;
        b = b / a;
        long double c = b * b + a;
        volatile long double va = 2.0L, three = 3.0L, tenth = 0.1L;
        long double vb = va * three;
        vb = vb - tenth;
        vb = vb / va;
        long double vc = vb * vb + va;
        if (!same(&c, &vc, LDBYTES)) FAIL();
    }
    {   /* a local overwritten by a value not known: what it held before
         * must not be folded in */
        volatile long double unknown = 5.0L;
        long double a = 2.0L;
        a = unknown;
        long double r = a * 3.0L;
        if (r != 15.0L) FAIL();
    }
    {   /* a folded value that feeds a branch */
        volatile long double a = 0.1L, b = 0.2L, c = 0.3L;
        if ((0.1L + 0.2L == 0.3L) != (a + b == c))
            FAIL();
    }
    return bad ? 1 + (line & 0x3f) : 42;
}
