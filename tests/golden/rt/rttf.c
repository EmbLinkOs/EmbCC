/* IEEE-754 binary128, against libgcc's soft-float.
 *
 * On aarch64 `long double` is binary128 and the machine has no
 * instruction for any of it, so every line below is a call into a
 * runtime: `a + b` is `__addtf3`, `a < b` is `__lttf2`, `(double)a` is
 * `__trunctfdf2`. Compiling this file with gcc calls libgcc's; with
 * EmbCC it calls lib/rt's. The two must print the same bits.
 *
 * Values are printed as their BITS. A decimal spelling would be testing
 * printf, and printf for a 113-bit significand is a different program
 * from the arithmetic under test.
 *
 * The corpus is chosen where soft-float goes wrong: the boundaries
 * between normal and subnormal, values that differ by one unit in the
 * last place, sums that cancel almost exactly, products that overflow
 * or underflow on the way, and every combination of zero, infinity and
 * NaN. A soft-float that is right on ordinary numbers and wrong at
 * those edges passes any test that does not include them.
 */
#include <stdio.h>

typedef unsigned long long u64;

static void put(const char *tag, long double x)
{
    union { long double f; struct { u64 lo, hi; } h; } v;
    v.f = x;
    /* A NaN's payload and sign are not fixed by the arithmetic -- the
     * two implementations may quiet a NaN differently and both be
     * right -- so what is compared is that both say NaN. Everything
     * else, including the sign of a zero and of an infinity, is
     * compared exactly. */
    if ((v.h.hi & 0x7FFF000000000000ULL) == 0x7FFF000000000000ULL &&
        ((v.h.hi & 0x0000FFFFFFFFFFFFULL) | v.h.lo) != 0)
        printf("%s nan\n", tag);
    else
        printf("%s %016llx%016llx\n", tag, v.h.hi, v.h.lo);
}

static void putd(const char *tag, double d)
{
    union { double d; u64 u; } v;
    v.d = d;
    if ((v.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
        (v.u & 0x000FFFFFFFFFFFFFULL) != 0)
        printf("%s nan\n", tag);
    else
        printf("%s %016llx\n", tag, v.u);
}

static void putf(const char *tag, float f)
{
    union { float f; unsigned u; } v;
    v.f = f;
    if ((v.u & 0x7F800000U) == 0x7F800000U && (v.u & 0x007FFFFFU) != 0)
        printf("%s nan\n", tag);
    else
        printf("%s %08x\n", tag, v.u);
}

static long double bits(u64 hi, u64 lo)
{
    union { long double f; struct { u64 lo, hi; } h; } v;
    v.h.hi = hi; v.h.lo = lo;
    return v.f;
}

#define NV 32
static long double V[NV];

static void fill(void)
{
    int i = 0;
    V[i++] = bits(0, 0);                          /* +0 */
    V[i++] = bits(0x8000000000000000ULL, 0);      /* -0 */
    V[i++] = bits(0x3FFF000000000000ULL, 0);      /* 1 */
    V[i++] = bits(0xBFFF000000000000ULL, 0);      /* -1 */
    V[i++] = bits(0x4000000000000000ULL, 0);      /* 2 */
    V[i++] = bits(0x3FFE000000000000ULL, 0);      /* 0.5 */
    V[i++] = bits(0x4002400000000000ULL, 0);      /* 10 */
    V[i++] = bits(0x3FFF000000000000ULL, 1);      /* 1 + 1ulp */
    V[i++] = bits(0x3FFEFFFFFFFFFFFFULL,
                  0xFFFFFFFFFFFFFFFFULL);         /* 1 - 1ulp */
    V[i++] = bits(0x7FFE000000000000ULL, 0);      /* a huge normal */
    V[i++] = bits(0x7FFEFFFFFFFFFFFFULL,
                  0xFFFFFFFFFFFFFFFFULL);         /* the largest finite */
    V[i++] = bits(0x0001000000000000ULL, 0);      /* the smallest normal */
    V[i++] = bits(0x0000FFFFFFFFFFFFULL,
                  0xFFFFFFFFFFFFFFFFULL);         /* the largest subnormal */
    V[i++] = bits(0, 1);                          /* the smallest subnormal */
    V[i++] = bits(0, 0x8000000000000000ULL);      /* a middling subnormal */
    V[i++] = bits(0x0000800000000000ULL, 0);      /* another */
    V[i++] = bits(0x7FFF000000000000ULL, 0);      /* +inf */
    V[i++] = bits(0xFFFF000000000000ULL, 0);      /* -inf */
    V[i++] = bits(0x7FFF800000000000ULL, 0);      /* a quiet NaN */
    V[i++] = bits(0x7FFF000000000000ULL, 1);      /* a signalling NaN */
    V[i++] = bits(0x4001921FB54442D1ULL,
                  0x8469898CC51701B8ULL);         /* pi */
    V[i++] = bits(0xC001921FB54442D1ULL,
                  0x8469898CC51701B8ULL);         /* -pi */
    V[i++] = bits(0x400062E42FEFA39EULL,
                  0xF35793C7673007E6ULL);         /* ln 2 * 4 */
    V[i++] = bits(0x3F8F000000000000ULL, 0);      /* 2^-112 */
    V[i++] = bits(0x3F8E000000000000ULL, 0);      /* 2^-113: a tie maker */
    V[i++] = bits(0x4034000000000000ULL, 0);      /* 2^53 */
    V[i++] = bits(0x4070000000000000ULL, 0);      /* 2^113 */
    V[i++] = bits(0x406F000000000000ULL, 0);      /* 2^112 */
    V[i++] = bits(0x3FFF000000000000ULL,
                  0x0000000000000001ULL);         /* 1 + ulp again, low */
    V[i++] = bits(0x4005000000000000ULL, 7);      /* an odd significand */
    V[i++] = bits(0xC005000000000000ULL, 7);
    V[i++] = bits(0x0000000000000000ULL,
                  0x0000000000000003ULL);         /* three ulps of nothing */
}

int main(void)
{
    int i, j;
    fill();

    /* Arithmetic over every ordered pair: 1024 of each. */
    for (i = 0; i < NV; i++)
        for (j = 0; j < NV; j++) {
            volatile long double a = V[i], b = V[j];
            printf("add %d %d ", i, j); put("", a + b);
            printf("sub %d %d ", i, j); put("", a - b);
            printf("mul %d %d ", i, j); put("", a * b);
            printf("div %d %d ", i, j); put("", a / b);
            printf("cmp %d %d %d %d %d %d %d %d\n", i, j,
                   a == b, a != b, a < b, a <= b, a > b, a >= b);
        }

    /* Conversions, both directions, through every width. */
    for (i = 0; i < NV; i++) {
        volatile long double a = V[i];
        printf("tod %d ", i); putd("", (double)a);
        printf("tof %d ", i); putf("", (float)a);
        printf("neg %d ", i); put("", -a);
    }
    {
        static const double ds[] = {
            0.0, -0.0, 1.0, -1.0, 0.5, 3.14159265358979,
            1e-300, 1e300, -1e300, 4.9406564584124654e-324,
            2.2250738585072014e-308, 1.7976931348623157e308,
            9007199254740993.0, 1e18, -1e18
        };
        int n = (int)(sizeof ds / sizeof *ds);
        for (i = 0; i < n; i++) {
            volatile double d = ds[i];
            volatile float f = (float)ds[i];
            printf("fromd %d ", i); put("", (long double)d);
            printf("fromf %d ", i); put("", (long double)f);
        }
    }
    {
        static const long long is[] = {
            0, 1, -1, 2, -2, 1000000007LL,
            9223372036854775807LL, -9223372036854775807LL - 1,
            4503599627370496LL, 10384593717069655LL
        };
        int n = (int)(sizeof is / sizeof *is);
        for (i = 0; i < n; i++) {
            volatile long long v = is[i];
            volatile unsigned long long u = (unsigned long long)is[i];
            printf("fromi %d ", i); put("", (long double)v);
            printf("fromu %d ", i); put("", (long double)u);
        }
    }
    /* Back to integers, over values that FIT -- out of range is
     * undefined in C and the two runtimes need not agree. */
    for (i = 0; i < NV; i++) {
        volatile long double a = V[i];
        if (a > -1e18L && a < 1e18L && a == a)
            printf("toi %d %lld\n", i, (long long)a);
        if (a >= 0.0L && a < 1e18L && a == a)
            printf("tou %d %llu\n", i, (unsigned long long)a);
    }
    /* The 128-bit pair, over values that fit in 113 bits exactly and a
     * few that do not, so the rounding on the way in is exercised. */
    {
        static const long long hi[] = { 0, 1, 255, 1LL << 40, 1LL << 62 };
        int n = (int)(sizeof hi / sizeof *hi);
        for (i = 0; i < n; i++)
            for (j = 0; j < n; j++) {
                volatile __int128 v = ((__int128)hi[i] << 64) | (unsigned long long)hi[j];
                volatile unsigned __int128 u = (unsigned __int128)v;
                printf("from128 %d %d ", i, j); put("", (long double)v);
                printf("fromu128 %d %d ", i, j); put("", (long double)u);
            }
    }
    printf("done\n");
    return 42;
}
