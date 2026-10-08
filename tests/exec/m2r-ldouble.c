/* long double locals in registers (mem2reg): every 16-byte value mem2reg
 * moves must move all 16 bytes. Its copies were 4 bytes wide for anything
 * that was not 8 -- harmless while 16-byte locals stayed in memory, a
 * quarter of the value once they did not. Each function here makes one of
 * mem2reg's copies: a merge after a branch (the phi copies), a value
 * carried around a loop, two values swapped in a loop (read-all-then-
 * write-all through temps), a merge with a path that never assigned (the
 * undefined seed, a pooled zero), and a tail call turned into a loop (the
 * parameters' copies). Values whose low bits differ from their high bits
 * make a partial copy visible. */
// expect-exit: 42

static volatile long double third = 1.0L / 3.0L;
static volatile int vn = 5;

__attribute__((noinline)) static long double pick(long double x, int c)
{
    long double r;
    if (c)
        r = x * 3.0L;
    else
        r = x - 1.0L;
    return r;
}

__attribute__((noinline)) static long double accumulate(int n)
{
    long double s = 0.0L, step = third;
    for (int i = 0; i < n; i++)
        s += step;
    return s;
}

__attribute__((noinline)) static long double swaps(int n)
{
    long double a = third, b = 2.0L;
    for (int i = 0; i < n; i++) {
        long double t = a;
        a = b;
        b = t;
    }
    return a - b;           /* n odd: 2 - 1/3 */
}

__attribute__((noinline)) static long double maybe(int c)
{
    long double r;          /* unassigned on one path, read only on the other */
    if (c)
        r = third;
    return c ? r : 0.0L;
}

__attribute__((noinline)) static long double sum_to(int n, long double acc)
{
    if (n == 0)
        return acc;
    return sum_to(n - 1, acc + third);
}

int main(void)
{
    long double x = third;
    if (pick(x, 1) != x * 3.0L) return 1;
    if (pick(x, 0) != x - 1.0L) return 2;
    if (accumulate(vn) != third + third + third + third + third) return 3;
    if (swaps(vn) != 2.0L - third) return 4;
    if (maybe(1) != third) return 5;
    if (maybe(0) != 0.0L) return 6;
    if (sum_to(vn, 0.0L) != third + third + third + third + third) return 7;
    return 42;
}
