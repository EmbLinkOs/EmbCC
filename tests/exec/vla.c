/* C99 variable length arrays: storage carved off the stack where the
 * declaration is reached, sizeof a run-time value, indexing and pointer
 * arithmetic scaled by run-time row sizes, VLA parameters, and the stack
 * released when the array's scope ends — including by break, continue and
 * a backward goto, which is what keeps a VLA in a loop from growing the
 * stack every iteration. Each release is checked by address: an iteration's
 * array must land exactly where the previous one did.
 */
// expect-exit: 42

/* a VLA parameter: arrives as int (*)[m], row stride m * 4 at run time */
static long sum2d(int n, int m, int a[n][m])
{
    long s = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++)
            s += a[i][j];
    return s;
}

/* the stride must come from THIS call's m */
static int row_bytes(int m, int (*p)[m])
{
    return (int)((char *)(p + 1) - (char *)p) + (int)sizeof *p - 8 * m;
}

/* ten arguments: stack arguments on both targets, stored below the VLA */
static long ten(long a, long b, long c, long d, long e, long f, long g,
                long h, long i, long j)
{
    return a + b + c + d + e + f + g + h + i + j;
}

static int recurse(int depth)
{
    int n = depth + 1;
    int v[n];
    for (int i = 0; i < n; i++)
        v[i] = depth;
    if ((int)sizeof v != n * (int)sizeof(int))
        return -1000;
    int below = depth ? recurse(depth - 1) : 0;
    long s = 0;
    for (int i = 0; i < n; i++)   /* untouched by the deeper frames */
        s += v[i];
    return below + (int)s;
}

static int basics(int n)
{
    int a[n];
    if (sizeof a != (unsigned long)n * sizeof(int))
        return 1;
    for (int i = 0; i < n; i++)
        a[i] = i * i;
    long s = 0;
    for (int *p = a; p < a + n; p++)
        s += *p;
    if (s != (long)(n - 1) * n * (2 * n - 1) / 6)
        return 2;
    int m = n;          /* the size is fixed at the declaration */
    n = 1000;
    if (sizeof a != (unsigned long)m * sizeof(int))
        return 3;
    if (sizeof(int[m + 1]) != (unsigned long)(m + 1) * 4 ||
        sizeof(double[2][m]) != (unsigned long)m * 16)
        return 4;
    return 0;
}

static int two_dims(int n, int m)
{
    int a[n][m], b[3][m], c[n][3];
    if (sizeof a != (unsigned long)(n * m) * 4 || sizeof a[0] != (unsigned long)m * 4 ||
        sizeof b != (unsigned long)(3 * m) * 4 || sizeof c != (unsigned long)n * 12)
        return 10;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++)
            a[i][j] = i * 100 + j;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < m; j++)
            b[i][j] = -1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            c[i][j] = 7;
    /* row-major, contiguous: a[i][j] is element i*m+j */
    int *flat = &a[0][0];
    if (flat[2 * m + 3] != 203 || &a[1][0] - &a[0][0] != m)
        return 11;
    /* a pointer to a row of run-time size */
    int (*row)[m] = a;
    row += 2;
    if ((*row)[4] != 204 || row - a != 2 || sizeof *row != (unsigned long)m * 4)
        return 12;
    row++;
    row--;
    if (row[1][1] != 301)
        return 13;
    int (*r2)[m] = a;
    r2 += 1;
    if (r2[0][5] != 105)
        return 14;
    long want = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++)
            want += i * 100 + j;
    if (sum2d(n, m, a) != want)
        return 15;
    int fixed[2][5] = { { 1, 2, 3, 4, 5 }, { 6, 7, 8, 9, 10 } };
    if (sum2d(2, 5, fixed) != 55)
        return 16;
    if (row_bytes(m, a) != 0 || row_bytes(3, c) != 0)
        return 17;
    /* the neighbours were not overrun */
    if (b[2][m - 1] != -1 || c[n - 1][2] != 7)
        return 18;
    return 0;
}

/* each iteration's array must reuse the previous one's storage */
static int loops(int n)
{
    char *first = 0;
    for (int i = 0; i < 20000; i++) {
        char buf[n];            /* 20000 x 4 KiB would overflow any stack */
        char var[i % 7 + 1];    /* the length is re-read every iteration */
        buf[0] = (char)i;
        var[i % 7] = 1;
        if (sizeof var != (unsigned long)(i % 7 + 1))
            return 19;
        if (!first)
            first = buf;
        else if (buf != first)
            return 20;
        if (i % 3 == 0)
            continue;           /* releases buf */
        if (i == 19000)
            break;              /* releases buf */
    }
    int k = 0;
    while (k < 5000) {
        long w[n / 8];
        w[0] = k;
        if (k == 0)
            first = (char *)w;
        else if ((char *)w != first)
            return 21;
        k++;
    }
    do {
        int x[n];
        x[n - 1] = k;
        switch (k % 4) {
        case 0: { int y[n]; y[0] = 1; k += y[0]; continue; }   /* releases y and x */
        case 1: k++; break;
        default: k += 2; break;
        }
        if (x[n - 1] > 1 << 30)
            return 22;
    } while (k < 6000);
    return 0;
}

/* a backward goto leaves the array's scope, so it is released */
static int backward_goto(int n)
{
    int k = 0;
    char *first = 0;
again:;
    char buf[n];
    buf[n - 1] = (char)k;
    if (!first)
        first = buf;
    else if (buf != first)
        return 30;
    if (++k < 20000)
        goto again;
    return 0;
}

/* a live VLA across a call with stack arguments */
static int calls(int n)
{
    long v[n];
    for (int i = 0; i < n; i++)
        v[i] = 1000 + i;
    long r = ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    long t = ten(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    if (r != 55 || t != 10045)
        return 40;
    for (int i = 0; i < n; i++)
        if (v[i] != 1000 + i)
            return 41;
    int a[n], b[2 * n];         /* two declarators, two allocations */
    for (int i = 0; i < 2 * n; i++)
        b[i] = i;
    for (int i = 0; i < n; i++)
        a[i] = -i;
    if (b[2 * n - 1] != 2 * n - 1 || a[n - 1] != -(n - 1) ||
        sizeof b != 2 * sizeof a)
        return 43;
    return 0;
}

int main(void)
{
    int r;
    if ((r = basics(13)) != 0) return r;
    if ((r = two_dims(4, 6)) != 0) return r;
    if ((r = loops(4096)) != 0) return r;
    if ((r = backward_goto(4000)) != 0) return r;
    if ((r = calls(10)) != 0) return r;
    /* sum over depth d of (d+1)*d, d = 0..9 */
    if (recurse(9) != 330) return 50;
    return 42;
}
