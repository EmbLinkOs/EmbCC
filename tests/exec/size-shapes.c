/* The shapes Cortex-M's size work rewrote (tests/golden/thumb-size.sh
 * checks the code; this checks the values), each run with values that
 * tell every argument and every word apart:
 *
 *   - stack arguments stored straight from the register a value lives in,
 *     or through a free low register when it lives in memory -- scalars,
 *     64-bit values and structs, mixed with register arguments that must
 *     survive the stores;
 *   - incoming stack parameters copied through a pushed low register;
 *   - composites returned in memory, copied into the caller's buffer
 *     through r0-r2, at sizes with and without a tail of single bytes;
 *   - rotated loops entered at their test (-Os on ARM): zero, one and
 *     many trips, with set-up code between the guard and the body. */
// expect-exit: 42
struct s3 { unsigned char a, b, c; };
struct s7 { unsigned char b[7]; };
struct s12 { int x, y, z; };
struct s20 { int w[5]; };
struct s30 { unsigned char b[30]; };

static volatile int sink;

__attribute__((noinline)) long many(int a, int b, int c, int d, int e,
                                    long long f, int g, struct s12 h, int i)
{
    return (long)(a + 2 * b + 3 * c + 5 * d + 7 * e) + (long)(f >> 20) +
           (long)(f & 0xff) + 11 * g + 13 * h.x + 17 * h.y + 19 * h.z +
           23 * i;
}

__attribute__((noinline)) int stk7(int a, int b, int c, int d,
                                   struct s7 e, struct s3 f, int g)
{
    int s = a + b + c + d + g;
    for (int k = 0; k < 7; k++)
        s += (k + 1) * e.b[k];
    return s + 100 * f.a + 1000 * f.b + 10000 * f.c;
}

/* values that live across the call in memory, then are stack arguments */
__attribute__((noinline)) long caller(int n)
{
    int v[9];
    for (int k = 0; k < 9; k++)
        v[k] = n * (k + 3) + sink;
    struct s12 h = { v[5], v[6], v[7] };
    long long f = ((long long)v[8] << 24) | 0x5a;
    long r = many(v[0], v[1], v[2], v[3], v[4], f, v[6], h, v[8]);
    r += many(v[8], v[7], v[6], v[5], v[4], -f, v[2], h, v[0]);
    return r + v[0] + v[8];
}

__attribute__((noinline)) struct s20 mk20(int seed)
{
    struct s20 r;
    for (int k = 0; k < 5; k++)
        r.w[k] = seed * (k + 1) + sink;
    return r;
}

__attribute__((noinline)) struct s30 mk30(int seed)
{
    struct s30 r;
    for (int k = 0; k < 30; k++)
        r.b[k] = (unsigned char)(seed + 3 * k);
    return r;
}

__attribute__((noinline)) struct s7 mk7(int seed, struct s7 *keep)
{
    struct s7 r;
    for (int k = 0; k < 7; k++)
        r.b[k] = (unsigned char)(seed * 5 + k);
    *keep = r;
    return r;
}

__attribute__((noinline)) unsigned slen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (unsigned)(p - s);
}

__attribute__((noinline)) void fill(int *d, int v, unsigned n)
{
    for (unsigned k = 0; k < n; k++)
        d[k] = v + (int)k;
}

__attribute__((noinline)) int sum_until(const int *p, int stop, int lim)
{
    int s = 0, k = 0;
    while (k < lim && p[k] != stop) {
        s += p[k];
        k++;
    }
    return s * 100 + k;
}

int main(void)
{
    int bad = 0;
    long c = caller(3);
    /* worked by hand: see the comment above many() for the weights */
    long want = 0;
    {
        int v[9];
        for (int k = 0; k < 9; k++)
            v[k] = 3 * (k + 3);
        long long f = ((long long)v[8] << 24) | 0x5a;
        want += (long)(v[0] + 2 * v[1] + 3 * v[2] + 5 * v[3] + 7 * v[4]) +
                (long)(f >> 20) + (long)(f & 0xff) + 11 * v[6] +
                13 * v[5] + 17 * v[6] + 19 * v[7] + 23 * v[8];
        want += (long)(v[8] + 2 * v[7] + 3 * v[6] + 5 * v[5] + 7 * v[4]) +
                (long)((-f) >> 20) + (long)((-f) & 0xff) + 11 * v[2] +
                13 * v[5] + 17 * v[6] + 19 * v[7] + 23 * v[0];
        want += v[0] + v[8];
    }
    if (c != want) bad |= 1;

    struct s7 e = { { 1, 2, 3, 4, 5, 6, 7 } };
    struct s3 f = { 9, 8, 7 };
    if (stk7(10, 20, 30, 40, e, f, 50) !=
        150 + (1 + 4 + 9 + 16 + 25 + 36 + 49) + 900 + 8000 + 70000)
        bad |= 2;

    struct s20 a = mk20(7);
    for (int k = 0; k < 5; k++)
        if (a.w[k] != 7 * (k + 1)) bad |= 4;
    /* the result written straight into an array element, its neighbour
     * a canary: a copy rounded up to whole words overwrites it */
    static struct s30 arr[2];
    for (int k = 0; k < 30; k++)
        arr[1].b[k] = 0xa5;
#if defined(__arm__)
    /* A caller of our own always gives a scratch of its own and copies
     * out of it, which hides an overrun. AAPCS passes the buffer as a
     * hidden first argument in r0, so through a pointer of that type the
     * callee writes straight into arr[0] -- next to the canary. */
    ((void (*)(struct s30 *, int))(void (*)(void))mk30)(&arr[0], 11);
#else
    arr[0] = mk30(11);
#endif
    for (int k = 0; k < 30; k++)
        if (arr[0].b[k] != (unsigned char)(11 + 3 * k) || arr[1].b[k] != 0xa5)
            bad |= 8;
    struct s7 keep, g = mk7(3, &keep);
    for (int k = 0; k < 7; k++)
        if (g.b[k] != 15 + k || keep.b[k] != 15 + k) bad |= 16;

    if (slen("") != 0 || slen("x") != 1 || slen("hello, world") != 12)
        bad |= 32;
    int buf[6] = { -1, -1, -1, -1, -1, -1 };
    fill(buf, 5, 0);
    if (buf[0] != -1) bad |= 64;
    fill(buf, 5, 1);
    if (buf[0] != 5 || buf[1] != -1) bad |= 64;
    fill(buf, 5, 5);
    for (int k = 0; k < 5; k++)
        if (buf[k] != 5 + k) bad |= 64;
    if (buf[5] != -1) bad |= 64;
    int q[5] = { 1, 2, 3, 0, 9 };
    if (sum_until(q, 0, 5) != 603 || sum_until(q, 1, 5) != 0 ||
        sum_until(q, 0, 0) != 0 || sum_until(q, 77, 5) != 1505)
        bad |= 128;
    return bad ? bad : 42;
}
