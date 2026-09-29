/* 64-bit values in register PAIRS on a 32-bit soft-float target (RV32:
 * rv_pair_alloc). Shaped for what the pair pass has to get right, each
 * result printed as bits so the host's answer is the reference:
 *
 *   chains     a result handed straight on as the next helper's operand
 *   across     doubles live across calls, which must take s2-s11
 *   shuffle    doubles passed in a different argument pair than they
 *              arrive in, next to ints -- the parallel move's job, since
 *              a double may live in a0:a1 while a2:a3 is being loaded
 *   swap       two doubles passed to each other's positions
 *   int64      long long arithmetic and the 64-bit divide helpers, whose
 *              operands are loaded the same way
 */
#include <stdint.h>
void writec(int c);
void puts_(const char *s);

static void hex64(uint64_t v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static void bits(double d)
{
    union { double d; uint64_t u; } x;
    x.d = d;
    hex64(x.u);
}

volatile int vzero;
__attribute__((noinline)) double chains(double x)
{
    return ((x * 0.5 + 1.25) * x - 3.0) * x / 7.0 + 2.0;
}
__attribute__((noinline)) double mix(int k, double a, int j, double b)
{
    return a * k - b / (j + 1);
}
__attribute__((noinline)) double swap(double a, double b) { return a - b * 3.0; }
__attribute__((noinline)) double across(double a, double b)
{
    double s = a + b, p = a * b;
    double q = chains(s);
    double r = mix(3, p, 4, s);
    double u = swap(r, q);
    double w = swap(q, r);
    return s + p + q + r + u * w + vzero;
}
__attribute__((noinline)) double loop(int n, double seed)
{
    double acc = 0, x = seed;
    for (int i = 0; i < n; i++) {
        x = x * 1.000001 + 0.25;
        if (i & 1) acc += x / (i + 1);
        else acc -= swap(x, acc);
    }
    return acc;
}
__attribute__((noinline)) int64_t i64(int64_t a, int64_t b, int k)
{
    int64_t s = a * 3 + b, q = a / (b | 1), m = a % (b | 1);
    for (int i = 0; i < k; i++)
        s = s * 31 + (q ^ m) + i;
    return s - q * m;
}
double (*volatile fp)(double, double) = swap;

int main(void)
{
    bits(chains(1.5));
    bits(chains(-2.75));
    bits(mix(5, 1.0 / 3.0, 2, 10.5));
    bits(swap(2.0, 0.125));
    bits(fp(0.125, 2.0));
    bits(across(1.1, 2.2));
    bits(across(-3.5, 0.001));
    bits(loop(37, 0.3));
    hex64((uint64_t)i64(123456789012345LL, -98765, 20));
    hex64((uint64_t)i64(-7, 3, 5));
    puts_("DONE\n");
    return 0;
}
