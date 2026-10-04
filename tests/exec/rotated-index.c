/* Loops whose test reads an array at the index they step: `while (s[n])
 * n++;`. Rotation copies that test to the bottom of the loop, after the
 * index's update, and strength reduction must then not hand the copy the
 * walking pointer, which has not moved yet: at -O2 and -Os on every target
 * the copy read the element BEFORE the one the index named, and strlen by
 * index came back one short. Each loop here is checked against its answer
 * by hand. */
// expect-exit: 42
static volatile int vz;

__attribute__((noinline)) static int len(const char *s)
{
    int n = 0;
    while (s[n] != 0)
        n++;
    return n;
}

/* int elements, and a sentinel other than zero */
__attribute__((noinline)) static int find(const int *a, int x)
{
    int i = 0;
    while (a[i] != x)
        i++;
    return i;
}

/* the element read both in the body (before the update) and in the test
 * (after it) */
__attribute__((noinline)) static long sum_pos(const int *a)
{
    long s = 0;
    int i = 0;
    while (a[i] > 0) {
        s = s * 3 + a[i];
        i++;
    }
    return s * 100 + i;
}

/* a step of two */
__attribute__((noinline)) static int evens(const short *a)
{
    int i = 0;
    while (a[i] != 0)
        i += 2;
    return i;
}

/* the copy in the test is a store, too */
__attribute__((noinline)) static int cpy(char *d, const char *s)
{
    int n = 0;
    while ((d[n] = s[n]) != 0)
        n++;
    return n;
}

int main(void)
{
    static const int a[] = { 5, 9, -4, 7, 3, 0 };
    static const short h[] = { 1, 0, 2, 0, 3, 0, 0, 0 };
    char d[16];
    if (len("abcde" + vz) != 5 || len("" + vz) != 0 || len("x" + vz) != 1)
        return 1;
    if (find(a + vz, 7) != 3 || find(a + vz, 5) != 0)
        return 2;
    if (sum_pos(a + vz) != (5L * 3 + 9) * 100 + 2)
        return 3;
    if (evens(h + vz) != 6)
        return 4;
    if (cpy(d, "hello" + vz) != 5 || d[0] != 'h' || d[4] != 'o' || d[5] != 0)
        return 5;
    return 42;
}
