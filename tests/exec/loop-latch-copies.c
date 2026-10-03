// expect-exit: 42
/* A bottom-tested loop's back-edge copies run before the branch when the
 * exit path reads nothing they write -- and stay on the back edge when it
 * does. `prev` below is the loop's own header value of `i` once copies
 * are propagated, and the exit returns it: moving `i`'s copy above the
 * branch would return the NEW i. */
__attribute__((noinline)) static int prev_index(const int *p, int n)
{
    int i = 0, prev = -1;
    do {
        prev = i;
        i = i + p[i];
    } while (i < n);
    return prev;
}

__attribute__((noinline)) static int fib(int n)
{
    int a = 0, b = 1;
    do {
        int t = a + b;
        a = b;
        b = t;
    } while (--n);
    return a;
}

__attribute__((noinline)) static unsigned digits(unsigned v, char *out)
{
    char t[12];
    int n = 0, m = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[m++] = t[--n];
    out[m] = 0;
    return (unsigned)m;
}

int main(void)
{
    int steps[8] = { 2, 9, 3, 9, 1, 9, 9, 9 };
    if (prev_index(steps, 6) != 5) return 1;     /* 0 -> 2 -> 5 -> 6 */
    if (fib(10) != 55) return 2;
    char b[12];
    if (digits(9073u, b) != 4 || b[0] != '9' || b[3] != '3' || b[4]) return 3;
    return 42;
}
