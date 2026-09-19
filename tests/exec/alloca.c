/* __builtin_alloca: n bytes of this function's stack, 16-aligned, alive
 * until it returns — beside a VLA, in a loop, and passed to a callee.
 * libstdc++'s <format> uses it (the C++ front end passes it through).
 * gcc referees every value. */
// expect-exit: 42

static int sum(const char *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

static int f(int n)
{
    char *p = __builtin_alloca(n);
    for (int i = 0; i < n; i++)
        p[i] = 1;
    int total = 0;
    for (int k = 0; k < 3; k++) {
        char *q = __builtin_alloca(8);          /* each its own */
        q[0] = (char)k;
        total += q[0];
    }
    char vla[n];
    vla[0] = 5;
    if (((unsigned long)p & 15) != 0)
        return -1;
    return sum(p, n) + total + vla[0];
}

int main(void)
{
    return f(34);        /* 34 + (0 + 1 + 2) + 5 */
}
