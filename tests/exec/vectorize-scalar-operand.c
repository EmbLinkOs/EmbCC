/* Loops the vectorizer looks at where one operand of the lane op is a
 * scalar: loop-invariant (`g[i] += n`, `7 - g[i]`, `p[i] += c` over a
 * run-time count) or different in every lane (`n * (i + 1) + g[i]`).
 * The scalar was handed to a vector op as it was -- x86-64's lowering
 * stopped with an internal error at -O2 -- where an invariant is now
 * broadcast before the loop and a per-lane value refuses it. */
// expect-exit: 42

int g[24];

__attribute__((noinline)) static void add_n(int n)
{
    for (int k = 0; k < 24; k++)
        g[k] += n;
}

__attribute__((noinline)) static void n_minus(int n)
{
    for (int k = 0; k < 24; k++)
        g[k] = n - g[k];
}

__attribute__((noinline)) static void seven_minus(void)
{
    for (int k = 0; k < 24; k++)
        g[k] = 7 - g[k];
}

__attribute__((noinline)) static void ramp_first(int n)
{
    for (int k = 0; k < 24; k++)
        g[k] = n * (k + 1) + g[k];
}

__attribute__((noinline)) static void ramp_second(int n)
{
    for (int k = 0; k < 24; k++)
        g[k] = g[k] + n * (k + 1);
}

__attribute__((noinline)) static void mask_or(int n, int m)
{
    for (int k = 0; k < 24; k++)
        g[k] = (g[k] & n) | m;
}

/* a run-time count: the vector loop and its scalar remainder */
__attribute__((noinline)) static void p_minus(int *p, int n, int c)
{
    for (int k = 0; k < n; k++)
        p[k] = c - p[k];
}

__attribute__((noinline)) static void p_plus(int *p, int n, int c)
{
    for (int k = 0; k < n; k++)
        p[k] += c;
}

/* what nrvo.c does: a compound assignment, whose right operand C
 * evaluates before the element it adds to */
struct big { int v[24]; };
__attribute__((noinline)) static struct big ramp_struct(int n)
{
    struct big r;
    for (int k = 0; k < 24; k++)
        r.v[k] = k;
    for (int k = 0; k < 24; k++)
        r.v[k] += n * (k + 1);
    return r;
}

int main(void)
{
    int bad = 0;
    for (int k = 0; k < 24; k++)
        g[k] = k;
    add_n(5);
    for (int k = 0; k < 24; k++)
        if (g[k] != k + 5) bad |= 1;
    n_minus(100);
    for (int k = 0; k < 24; k++)
        if (g[k] != 95 - k) bad |= 2;
    seven_minus();
    for (int k = 0; k < 24; k++)
        if (g[k] != k - 88) bad |= 4;
    for (int k = 0; k < 24; k++)
        g[k] = 1;
    ramp_first(3);
    for (int k = 0; k < 24; k++)
        if (g[k] != 3 * (k + 1) + 1) bad |= 8;
    ramp_second(2);
    for (int k = 0; k < 24; k++)
        if (g[k] != 5 * (k + 1) + 1) bad |= 16;
    struct big b = ramp_struct(4);
    for (int k = 0; k < 24; k++)
        if (b.v[k] != k + 4 * (k + 1)) bad |= 32;
    for (int k = 0; k < 24; k++)
        g[k] = k * 37;
    mask_or(0xf0, 3);
    for (int k = 0; k < 24; k++)
        if (g[k] != (((k * 37) & 0xf0) | 3)) bad |= 64;
    int p[23];                          /* five vectors and a remainder */
    for (int k = 0; k < 23; k++)
        p[k] = k;
    p_minus(p, 23, 50);
    p_plus(p, 21, 1000);                /* and with two left over */
    for (int k = 0; k < 23; k++)
        if (p[k] != (k < 21 ? 1050 - k : 50 - k)) bad |= 128;
    return bad ? bad : 42;
}
