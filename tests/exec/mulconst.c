/* Multiplication by the constants a backend turns into a shifted-operand
 * add or subtract and a shift -- 3, 5, 9 (2^k + 1), 7, 15 (2^k - 1), 6, 10,
 * 12, 20 (those shifted) -- beside ones it does not (37, 25, 100, -3), at
 * both widths, with values that overflow 32 bits and negative ones. Each
 * is a function of its argument, so nothing folds to a constant first,
 * and each result is compared with the product formed by repeated
 * addition. */
// expect-exit: 42
#define M(N) __attribute__((noinline)) long m##N(long x) { return x * N; } \
             __attribute__((noinline)) int  i##N(int x)  { return x * N; }
M(3) M(5) M(6) M(7) M(9) M(10) M(12) M(15) M(20) M(24) M(25) M(37) M(100)
__attribute__((noinline)) long mneg3(long x) { return x * -3; }
__attribute__((noinline)) unsigned u7(unsigned x) { return x * 7u; }
static long slow(long x, int n) { long r = 0; for (int k = 0; k < n; k++) r += x; return r; }
static int slowi(int x, int n) { int r = 0; for (int k = 0; k < n; k++) r += x; return r; }

int main(void)
{
    long xs[] = { 0, 1, -1, 7, -13, 1000003, -2147483647L - 1, 2147483647L, 123456789012L, -98765432109L };
    int xi[] = { 0, 1, -1, 7, -13, 1000003, -2147483647 - 1, 2147483647, 305419896 };
    int bad = 0;
#define CHK(N) for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) if (m##N(xs[k]) != slow(xs[k], N)) bad |= 1; \
               for (unsigned k = 0; k < sizeof xi / sizeof xi[0]; k++) if (i##N(xi[k]) != slowi(xi[k], N)) bad |= 2;
    CHK(3) CHK(5) CHK(6) CHK(7) CHK(9) CHK(10) CHK(12) CHK(15) CHK(20) CHK(24) CHK(25) CHK(37) CHK(100)
    for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) if (mneg3(xs[k]) != -slow(xs[k], 3)) bad |= 4;
    if (u7(0xdeadbeefu) != 0xdeadbeefu * 7u || u7(3u) != 21u) bad |= 8;
    if (bad) return bad;
    return 42;
}
