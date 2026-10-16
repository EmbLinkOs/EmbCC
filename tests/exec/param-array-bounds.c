/* An array parameter is adjusted to a pointer, but the length in its
 * brackets is still an expression the definition evaluates on entry
 * (C17 6.9.1p10): `int a[i++]` increments i. The adjustment dropped it,
 * so GCC's 970217-1.c and pr77767.c returned the old values. A length is
 * an assignment-expression, too: `int a[i += 2]` did not parse. */
// expect-exit: 42

__attribute__((noinline)) static int sub(int i, int array[i++])
{
    return i;
}

/* two of them, each with its own count (pr77767.c) */
__attribute__((noinline)) static int two(int a, int b[a++], int c,
                                         int d[c++])
{
    return a * 10 + c;
}

static int calls;
__attribute__((noinline)) static int next(void) { return ++calls; }

/* a call in the length is made once per entry, and a row size takes the
 * value n++ yields, n one more after it */
__attribute__((noinline)) static int rows(int n, int q[next()],
                                          int (*p)[n++])
{
    return n * 10 + (int)(sizeof *p / sizeof **p);
}

__attribute__((noinline)) static int assign(int i, int a[i += 2],
                                            int (*cb)(int))
{
    return cb(i);
}

static int id(int j) { return j; }

/* a parenthesized name and a prototype before the definition */
__attribute__((noinline)) static int (paren)(int i, int a[i++])
{
    return i;
}

int proto(int i, int a[i++]);
__attribute__((noinline)) int proto(int i, int a[i++])
{
    return i;
}

/* a length with no effect costs nothing and changes nothing */
__attribute__((noinline)) static int plain(int n, int a[n])
{
    return n;
}

int main(void)
{
    int e[10], m[4][4];
    int ok = 0;
    ok += sub(10, e) == 11;
    ok += two(1, e, 1, e) == 22;
    ok += rows(3, e, m) == 43 && calls == 1;
    ok += rows(3, e, m) == 43 && calls == 2;
    ok += assign(5, e, id) == 7;
    ok += paren(3, e) == 4;
    ok += proto(1, e) == 2;
    ok += plain(5, e) == 5;
    return ok == 8 ? 42 : ok;
}
