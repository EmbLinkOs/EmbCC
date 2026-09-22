/* Complex multiply and divide, over the values where the naive formulas
 * give the wrong answer: infinities beside NaNs, zeroes in the
 * denominator, and magnitudes far enough apart that a squared
 * denominator would overflow or underflow.
 *
 * Same discipline as rt128.c -- the host's own runtime is the oracle,
 * and the two builds must print identical bits. The interesting cases
 * are exactly the ones C99 Annex G exists for, and they are the ones a
 * self-comparison cannot judge.
 */
#include <stdio.h>
#include <complex.h>

typedef unsigned long long u64;

/* Bit patterns, because a decimal spelling depends on the printf that
 * formats it -- except for NaN, which is printed as the word.
 *
 * The sign of a NaN is chosen by the ARCHITECTURE, not by the
 * arithmetic: x86-64's default NaN from 0.0/0.0 has its sign bit set
 * and aarch64's does not. Comparing those bits across the two would be
 * asserting which machine the reference was built on, so what is
 * compared is that both agree it is a NaN. Every other value, including
 * the sign of a zero and of an infinity, is compared exactly. */
static void put1d(u64 u)
{
    if ((u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
        (u & 0x000FFFFFFFFFFFFFULL) != 0)
        printf(" nan");
    else
        printf(" %016llx", u);
}

static void putd(const char *tag, double _Complex z)
{
    union { double d; u64 u; } r, i;
    r.d = __real__ z;
    i.d = __imag__ z;
    printf("%s", tag);
    put1d(r.u);
    put1d(i.u);
    printf("\n");
}

static void put1f(unsigned u)
{
    if ((u & 0x7F800000U) == 0x7F800000U && (u & 0x007FFFFFU) != 0)
        printf(" nan");
    else
        printf(" %08x", u);
}

static void putf(const char *tag, float _Complex z)
{
    union { float f; unsigned u; } r, i;
    r.f = __real__ z;
    i.f = __imag__ z;
    printf("%s", tag);
    put1f(r.u);
    put1f(i.u);
    printf("\n");
}

/* ---- what is compared exactly, and what within one ulp -----------------
 *
 * Multiply is compared BIT FOR BIT, including every special value:
 * Annex G pins those down, and lib/rt agrees with libgcc on all 2401
 * combinations of zero, one, infinity and NaN.
 *
 * Ordinary DIVISION is compared within one unit in the last place, and
 * the reason is not laxity. Annex G fixes the special values -- those
 * are diffed exactly too, as `sd` lines -- but for finite operands it
 * requires no particular rounding, and three respected implementations
 * disagree: lib/rt uses Smith's method, modern libgcc something more
 * elaborate, clang's compiler-rt a third algorithm again. Measured
 * against libgcc, 141 of 10683 lines differed and every one of them by
 * exactly one ulp, with libgcc the closer of the two. Demanding
 * bit-identity there would be asserting which toolchain built the
 * reference; ignoring the lines would let a wildly wrong divide pass.
 * One ulp is the bound, and tests/golden/rt.sh enforces it by comparing
 * the printed bit patterns, which for two finite values of the same
 * sign differ by one exactly when the values are one ulp apart. */

int main(void)
{
    /* volatile so the values reach the runtime rather than the folder:
     * a constant-folded complex multiply never calls __muldc3, and this
     * would then compare two compilers' folders. */
    volatile double zero = 0.0, one = 1.0;
    double inf = one / zero;
    double nan = zero / zero;
    static const double parts[] = {
        0.0, -0.0, 1.0, -1.0, 2.0, 0.5, -3.25,
        1e-300, 1e300, -1e300, 1e-160, 1e160,
        1.7976931348623157e308,      /* the largest finite double */
        4.9406564584124654e-324      /* the smallest subnormal */
    };
    int n = (int)(sizeof parts / sizeof *parts);
    int i, j, k, l;

    /* The ordinary grid, at a stride that keeps the output finite while
     * still crossing every sign and magnitude boundary. */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j += 3)
            for (k = 0; k < n; k += 2)
                for (l = 0; l < n; l += 5) {
                    volatile double a = parts[i], b = parts[j];
                    volatile double c = parts[k], d = parts[l];
                    double _Complex x = a + b * I, y = c + d * I;
                    printf("dm %d %d %d %d ", i, j, k, l);
                    putd("", x * y);
                    printf("dd %d %d %d %d ", i, j, k, l);
                    putd("", x / y);
                    {
                        float _Complex fx = (float)a + (float)b * I;
                        float _Complex fy = (float)c + (float)d * I;
                        printf("fm %d %d %d %d ", i, j, k, l);
                        putf("", fx * fy);
                        printf("fd %d %d %d %d ", i, j, k, l);
                        putf("", fx / fy);
                    }
                }

    /* And the Annex G cases: infinities and NaNs in every position. */
    {
        double sp[] = { 0.0, -0.0, 1.0, -1.0, inf, -inf, nan };
        int m = (int)(sizeof sp / sizeof *sp);
        for (i = 0; i < m; i++)
            for (j = 0; j < m; j++)
                for (k = 0; k < m; k++)
                    for (l = 0; l < m; l++) {
                        volatile double a = sp[i], b = sp[j];
                        volatile double c = sp[k], d = sp[l];
                        double _Complex x = a + b * I, y = c + d * I;
                        printf("sm %d %d %d %d ", i, j, k, l);
                        putd("", x * y);
                        printf("sd %d %d %d %d ", i, j, k, l);
                        putd("", x / y);   /* Annex G pins these exactly */
                    }
    }
    printf("done\n");
    return 42;
}
