/* Division and remainder of two constants are folded by the optimizer
 * (src/opt/fold.c, fold_bin), and the folded value must be C's: the
 * quotient truncated toward zero, the remainder taking the dividend's
 * sign, unsigned at full width, 32- and 64-bit. `sizeof a / sizeof a[0]`
 * is the common case, which every target computed with a divide at -O2
 * (a __udivsi3 call on AVR). The operands reach the division through
 * locals, so the front end does not fold them first. The exit status is
 * the number of the first wrong result (0: all right), so the test needs
 * no printf on a board without one. */
// expect-exit: 0

static const unsigned char tab[] = { 1, 2, 3, 4, 5, 6, 7 };
static long words[9];

static int nth, first_bad;

static void check(long long got, long long want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    int a = -7, b = 2, c = 7, d = -2;
    unsigned long u = 0xfffffffful, v = 3ul, w = 0x80000000ul, x = 7ul;
    long long p = -9000000000000LL, q = 7LL;
    unsigned long long r = 0xffffffffffffffffULL, s = 10ULL;

    check(sizeof tab / sizeof tab[0], 7);
    check(sizeof words / sizeof words[0], 9);
    check(a / b, -3);
    check(a % b, -1);
    check(c / d, -3);
    check(c % d, 1);
    check(a / d, 3);
    check(a % d, -1);
    check(u / v, 0x55555555);
    check(u % v, 0);
    check(w / x, 306783378);
    check(w % x, 2);
    check(p / q, -1285714285714LL);
    check(p % q, -2);
    check((long long)(r / s), 1844674407370955161LL);
    check((long long)(r % s), 5);
    return first_bad;
}
