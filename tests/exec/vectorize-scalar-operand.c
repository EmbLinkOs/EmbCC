/* Loops the vectorizer looks at where one operand of the lane op is a
 * scalar: loop-invariant (`g[i] += n`, `7 - g[i]`) or different in every
 * lane (`n * (i + 1) + g[i]`). Only the form with the scalar second was
 * refused; with it first -- where canonical operand order puts a lower-
 * numbered temp -- the scalar was handed to a vector add, and x86-64's
 * lowering stopped with an internal error at -O2. */
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
    return bad ? bad : 42;
}
