/* A function inlined into its caller brings its calls along, and with
 * them the stack space their arguments need. sum16 takes ten of its
 * sixteen arguments on the stack on x86-64; helper, its only caller, is
 * inlined into main, which itself calls nothing that way. The inliner
 * did not carry the callee's outgoing-argument area into the caller, so
 * main's frame had none, and the ten stack arguments were written over
 * the bottom of main's frame -- where the values live across the call
 * that found no register are kept. (embedded-libc.c at -O1: a printf of
 * four long doubles, inlined into main, printed its fourth as its
 * first.) */
// expect-exit: 42
__attribute__((noinline)) static long sum16(long a, long b, long c, long d,
                                            long e, long f, long g, long h,
                                            long i, long j, long k, long l,
                                            long m, long n, long o, long p)
{
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p;
}

static long helper(long x)
{
    return sum16(x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7,
                 x + 8, x + 9, x + 10, x + 11, x + 12, x + 13, x + 14, x + 15);
}

int main(void)
{
    volatile long k = 3;
    long v0 = k * 1, v1 = k * 2, v2 = k * 3, v3 = k * 4, v4 = k * 5;
    long v5 = k * 6, v6 = k * 7, v7 = k * 8, v8 = k * 9, v9 = k * 10;
    long v10 = k * 11, v11 = k * 12;
    long r = helper(k);
    long s = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11;
    if (r != 16 * 3 + 120)
        return 1;
    if (s != 3 * 78)
        return 2;
    return 42;
}
