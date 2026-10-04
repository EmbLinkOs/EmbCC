/* Loops with constant bounds. A rotated loop's guard reads the counter
 * the block in front of it has just set, so the optimizer decides it at
 * compile time: run for `i = 0; i < 8`, skip for `i = 10; i < 4`. Each
 * function checks both the decision and what the loop computed -- the
 * loops that must not run at all, the loop that runs once, a counter
 * changed between its constant start and the test, and compares whose
 * answer depends on their signedness or width. */
// expect-exit: 42
static volatile int vz;                 /* 0, and opaque */
static int m[8][12];

static int nest(void)
{
    int s = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 12; j++)
            s += m[i][j] ^ j;
    return s;
}

static int never(void)
{
    int s = 7;
    for (int i = 10; i < 4; i++)
        s += i * 1000;
    for (unsigned u = 5; u < 5; u++)
        s += 100;
    for (int k = -1; k >= 0; k--)
        s += 50;
    return s;                           /* 7 */
}

static int once(void)
{
    int s = 0;
    for (int i = 3; i < 4; i++)
        s += i;
    return s;                           /* 3 */
}

/* The counter starts as a constant and is changed before the test, so
 * the constant is not what the guard sees. */
static int reassigned(void)
{
    int s = 0;
    int i = 0;
    i = i + vz + 20;                    /* 20: the guard fails */
    for (; i < 12; i++)
        s += 1000;
    int j = 100;
    j = vz;                             /* 0: the guard holds */
    for (; j < 3; j++)
        s += 2;
    return s;                           /* 6 */
}

/* -16 < 16 signed, but 0xfffffff0 is not below 16 unsigned. */
static int widths(void)
{
    int s = 0;
    for (unsigned long u = 0xfffffff0ul; u < 16ul; u++)
        s += 100;
    for (long i = -16; i < 16; i += 8)
        s += 1;                         /* 4 times */
    for (long long w = 0; w < 3; w++)
        s += 2;                         /* 6 */
    for (long long w = 5000000000LL; w < 3; w++)
        s += 1000;
    return s;                           /* 10 */
}

int main(void)
{
    int ref = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 12; j++)
            m[i][j] = i * 12 + j + vz;
    /* the same sum with bounds the compiler cannot know */
    for (int i = 0; i < 8 + vz; i++)
        for (int j = 0; j < 12 + vz; j++)
            ref += m[i][j] ^ j;
    if (nest() != ref)
        return 1;
    if (never() != 7)
        return 2;
    if (once() != 3)
        return 3;
    if (reassigned() != 6)
        return 4;
    if (widths() != 10)
        return 5;
    return 42;
}
