/* An int index sign-extended past an add the loop's bounds rule out
 * wrapping (src/opt/rangecheck.c, pass_ivext): on a 64-bit target
 * `(long)(i + c)` becomes `(long)i + c` only when i's range keeps i + c
 * inside int. Checked both ways -- a filter's x[i - k] and x[i + k] with
 * constant bounds and with a bound from the caller, where the rewrite
 * applies; and counters near INT_MAX and INT_MIN, where i + c does wrap
 * under -fwrapv and the extension must be of the wrapped value -- each
 * against the same sums made with the index in a long. */
// expect-exit: 42
#include <limits.h>

typedef long long i64;

static int x[300];

__attribute__((noinline)) i64 fir(void)
{
    i64 s = 0;
    for (int i = 15; i < 256; i++)
        for (int k = 0; k < 8; k++)
            s += (i64)x[i - k] * (k + 1) + x[i + k];
    return s;
}

static i64 fir_ref(void)
{
    i64 s = 0;
    for (long i = 15; i < 256; i++)
        for (long k = 0; k < 8; k++)
            s += (i64)x[i - k] * (k + 1) + x[i + k];
    return s;
}

/* B from the caller: i < n <= INT_MAX, so i - 3 >= 0 and i + 1 fits */
__attribute__((noinline)) i64 runs(int n)
{
    i64 s = 0;
    for (int i = 3; i < n; i++)
        s += x[i - 3] - x[i - 1] + (i64)(long)(i + 1);
    return s;
}

static i64 runs_ref(int n)
{
    i64 s = 0;
    for (long i = 3; i < n; i++)
        s += x[i - 3] - x[i - 1] + (i + 1);
    return s;
}

/* i runs up to INT_MAX - 1: i + 10 wraps for the last ten, and its
 * extension must be negative there (the add made in unsigned, so that
 * the test is ISO C; the IR is the same 32-bit add and extension) */
__attribute__((noinline)) i64 top(int n)
{
    i64 s = 0;
    for (int i = INT_MAX - 20; i < n; i++)
        s += (long)(int)((unsigned)i + 10u);
    return s;
}

static i64 top_ref(int n)
{
    i64 s = 0;
    for (long i = INT_MAX - 20; i < n; i++) {
        long v = i + 10;
        if (v > INT_MAX) v -= 4294967296L;   /* -fwrapv's int */
        s += v;
    }
    return s;
}

/* Constant bounds, so the loop is one the rewrite takes: i stays below
 * INT_MAX - 5, so i - 3 is rewritten and i + 10, which wraps for the
 * last five trips, must not be */
__attribute__((noinline)) i64 top2(void)
{
    i64 s = 0;
    for (int i = INT_MAX - 100; i < INT_MAX - 5; i++)
        s += (long)(int)((unsigned)i + 10u) * 3 + (long)(int)((unsigned)i - 3u);
    return s;
}

static i64 top2_ref(void)
{
    i64 s = 0;
    for (long i = INT_MAX - 100; i < (long)INT_MAX - 5; i++) {
        long v = i + 10;
        if (v > INT_MAX) v -= 4294967296L;
        s += v * 3 + (i - 3);
    }
    return s;
}

/* A loop the rewrite takes (its body, sixteen unrolled taps, is too big
 * to unroll) whose taps cross INT_MAX: i < INT_MAX - 5, so i + k fits
 * for k <= 5 and is rewritten, and wraps for the rest, which must not be */
__attribute__((noinline)) i64 edge(void)
{
    i64 s = 0;
    for (int i = INT_MAX - 40; i < INT_MAX - 5; i++)
        for (int k = 0; k < 16; k++)
            s += (long)(int)((unsigned)i + (unsigned)k) * x[k];
    return s;
}

static i64 edge_ref(void)
{
    i64 s = 0;
    for (long i = INT_MAX - 40; i < (long)INT_MAX - 5; i++)
        for (long k = 0; k < 16; k++) {
            long v = i + k;
            if (v > INT_MAX) v -= 4294967296L;
            s += v * x[k];
        }
    return s;
}

/* An inner loop entered again with what its last exit left in i: the
 * first entry runs i up to 100, and each later one once more, to 199 --
 * past what the constant before it says. i + INT_MAX - 150 wraps from
 * i = 151 on. */
__attribute__((noinline)) i64 reent(void)
{
    i64 s = 0;
    int i = 0;
    for (int r = 0; r < 100; r++) {
        do {
            s += (long)(int)((unsigned)i + (unsigned)(INT_MAX - 150));
            i++;
        } while (i < 100);
    }
    return s;
}

static i64 reent_ref(void)
{
    i64 s = 0;
    long i = 0;
    for (int r = 0; r < 100; r++) {
        do {
            long v = i + (INT_MAX - 150);
            if (v > INT_MAX) v -= 4294967296L;
            s += v;
            i++;
        } while (i < 100);
    }
    return s;
}

/* and from INT_MIN: i - 1 wraps on the first trip */
__attribute__((noinline)) i64 bottom(void)
{
    i64 s = 0;
    for (int i = INT_MIN; i < INT_MIN + 4; i++)
        s += (long)(int)((unsigned)i - 1u);
    return s;
}

static i64 bottom_ref(void)
{
    i64 s = 0;
    for (long i = INT_MIN; i < (long)INT_MIN + 4; i++) {
        long v = i - 1;
        if (v < INT_MIN) v += 4294967296L;
        s += v;
    }
    return s;
}

int main(void)
{
    for (int i = 0; i < 300; i++)
        x[i] = (i * 37) % 101 - 50;
    if (fir() != fir_ref()) return 1;
    if (runs(200) != runs_ref(200)) return 2;
    if (runs(2) != runs_ref(2)) return 3;
    if (top(INT_MAX) != top_ref(INT_MAX)) return 4;
    if (bottom() != bottom_ref()) return 5;
    if (top2() != top2_ref()) return 6;
    if (reent() != reent_ref()) return 7;
    if (edge() != edge_ref()) return 8;
    return 42;
}
