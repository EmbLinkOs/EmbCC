/* A loop's counter, read after the loop.
 *
 * Induction-variable strength reduction (src/opt/ivsr.c) turns
 * `a[i]` into a walking pointer and, when the loop's bound is a
 * constant, tests the pointer instead of the counter, so the counter can
 * die. It gave the counter a name of its own inside the loop and left the
 * old name holding the value from before the loop: right for the loop's
 * guard, wrong for a read after the loop. `while (k < 9 && isdigit(t[k]))
 * k++; return k;` returned 0 at -O1 and above on every target (found by
 * an EmbCC-built embdoc, whose ordered lists vanished). Each function
 * here reads its counter after the loop in another shape; the last two
 * do not, and are the ones the rewrite is for.
 */
// expect-exit: 42

/* an early exit, the counter read after: the reported shape */
__attribute__((noinline)) static int digits(const char *t)
{
    int k = 0;
    while (k < 9 && t[k] >= '0' && t[k] <= '9')
        k++;
    return k;
}

/* the same as a for loop with a break */
__attribute__((noinline)) static int first_zero(const int *a)
{
    int i;
    for (i = 0; i < 16; i++)
        if (a[i] == 0)
            break;
    return i;
}

/* a full trip, the counter read after: it must be the bound */
__attribute__((noinline)) static int full(const short *a, int *sum)
{
    int i, s = 0;
    for (i = 0; i < 10; i++)
        s += a[i];
    *sum = s;
    return i;
}

/* the counter read after the loop through arithmetic */
__attribute__((noinline)) static int scaled(const unsigned char *p)
{
    int k = 0;
    while (k < 12 && p[k] != 0xff)
        k++;
    return k * 3 + 1;
}

/* nested: the inner counter is reset each outer trip and not read after
 * its loop -- the rewrite may happen, and the sums must hold */
__attribute__((noinline)) static int grid(const int (*m)[8])
{
    int s = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++)
            s += m[r][c];
    return s;
}

/* the counter not read after the loop at all */
__attribute__((noinline)) static long copy_sum(int *dst, const int *src)
{
    long s = 0;
    for (int i = 0; i < 20; i++) {
        dst[i] = src[i] * 2;
        s += dst[i];
    }
    return s;
}

int main(void)
{
    static const int z[16] = { 5, 4, 3, 2, 1, 0, 9, 9 };
    static const int nz[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    static const short h[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    static const unsigned char u[16] = { 1, 2, 3, 0xff, 7 };
    static const unsigned char un[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    int m[4][8], src[20], dst[20], s;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++)
            m[r][c] = r * 8 + c;
    for (int i = 0; i < 20; i++)
        src[i] = i;

    if (digits("123x") != 3 || digits("x") != 0 || digits("1234567890") != 9)
        return 1;
    if (first_zero(z) != 5 || first_zero(nz) != 16)
        return 2;
    if (full(h, &s) != 10 || s != 55)
        return 3;
    if (scaled(u) != 10 || scaled(un) != 37)
        return 4;
    if (grid((const int (*)[8])m) != 496)
        return 5;
    if (copy_sum(dst, src) != 380 || dst[19] != 38)
        return 6;
    return 42;
}
