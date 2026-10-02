/* Loops whose counter is compared with `!=` and advances by more than
 * one: what strength reduction leaves of every array walk (a pointer
 * stepped by the element size up to its end), and what the unroller
 * takes since it learned that shape. Trip counts from 0 to well past
 * the unroll factor, so that the entry guard, the full runs and the
 * remainder each get exercised; element sizes 1, 4, 8 and 12; a walk
 * with a wrapping unsigned count; the pointer's value AFTER the loop;
 * and an inner loop under an outer counter that must survive it. Every
 * expectation is a closed form, never another loop. */
// expect-exit: 42
struct s12 { int a, b, c; };

__attribute__((noinline)) long sum_int(const int *a, int n)
{ long s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
__attribute__((noinline)) long sum_char(const signed char *a, int n)
{ long s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
__attribute__((noinline)) long sum_long(const long *a, int n)
{ long s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
__attribute__((noinline)) long sum_s12(const struct s12 *a, int n)
{ long s = 0; for (int i = 0; i < n; i++) s += a[i].b; return s; }
__attribute__((noinline)) long walk_end(const int *p, const int *end)
{ long s = 0; for (; p != end; p++) s += *p; return s; }
__attribute__((noinline)) long step3(int n)            /* n a multiple of 3 */
{ long s = 0; for (int i = 0; i != n; i += 3) s += i; return s; }
__attribute__((noinline)) long two_adds(const int *a, int n)   /* n a multiple of 3 */
{ long s = 0; for (int i = 0; i != n; ) { i += 1; s += a[i]; i += 2; } return s; }
__attribute__((noinline)) int wrap(unsigned from, unsigned to)
{ int c = 0; for (unsigned i = from; i != to; i += 4) c++; return c; }
__attribute__((noinline)) long final_ptr(int *a, int n)
{ int *p = a; while (p != a + n) *p++ = 1; return p - a; }
__attribute__((noinline)) long nested(const int *a, int n, int m)
{ long s = 0; for (int j = 0; j < m; j++) for (int i = 0; i < n; i++) s += a[i] * (j + 1); return s; }
__attribute__((noinline)) long copy_two(const int *a, int *b, int n)
{ long s = 0; for (int i = 0; i < n; i++) b[i] = a[i] + 1; for (int i = 0; i < n; i++) s += b[i]; return s; }

static int a[40]; static signed char c[40]; static long l[40]; static struct s12 t[40]; static int b[40];
/* a[i] = 7i - 3, so sum over i < n is 7n(n-1)/2 - 3n */
static long closed(long n) { return 7 * n * (n - 1) / 2 - 3 * n; }

int main(void)
{
    static const int tries[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 40 };
    for (int i = 0; i < 40; i++) { a[i] = 7 * i - 3; c[i] = (signed char)(7 * i - 3); l[i] = 7 * i - 3; t[i].b = 7 * i - 3; t[i].a = t[i].c = 99; }
    int bad = 0;
    for (unsigned k = 0; k < sizeof tries / sizeof *tries; k++) {
        int n = tries[k];
        if (sum_int(a, n) != closed(n)) bad |= 1;
        if (sum_char(c, n) != closed(n) - 256L * (n > 19 ? n - 19 : 0)) bad |= 2;  /* 7i-3 wraps from i = 19 */
        if (sum_long(l, n) != closed(n)) bad |= 4;
        if (sum_s12(t, n) != closed(n)) bad |= 8;
        if (walk_end(a, a + n) != closed(n)) bad |= 16;
        if (final_ptr(b, n) != n) bad |= 32;
        for (int i = 0; i < 40; i++) if (b[i] != (i < n)) bad |= 64;
        for (int i = 0; i < 40; i++) b[i] = 0;
        if (nested(a, n, 3) != 6 * closed(n)) bad |= 128;
        if (nested(a, 3, n) != closed(3) * (long)n * (n + 1) / 2) bad |= 256;
        if (copy_two(a, b, n) != closed(n) + n) bad |= 512;
    }
    /* 0, 3, 6, ..., n-3: sum = 3 * (n/3)(n/3 - 1)/2 */
    for (int n = 0; n <= 60; n += 3) if (step3(n) != 3L * (n / 3) * (n / 3 - 1) / 2) bad |= 1024;
    /* a[1] + a[4] + ... + a[n-2]: (n/3) terms, 7(3k+1)-3 = 21k+4 */
    for (int n = 0; n <= 39; n += 3) if (two_adds(a, n) != 21L * (n / 3) * (n / 3 - 1) / 2 + 4L * (n / 3)) bad |= 4096;
    if (wrap(0xFFFFFFF0u, 16) != 8 || wrap(0, 0) != 0 || wrap(0, 4) != 1 || wrap(0xFFFFFFFCu, 0) != 1 || wrap(0, 4 * 37) != 37) bad |= 2048;
    if (bad) return bad;
    return 42;
}
