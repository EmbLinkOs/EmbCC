/* va_copy (C99 7.15.1.2): the copy is a list of its own. EmbCC's va_list is
 * a pointer to the tag va_start built (SysV __va_list_tag / the AAPCS64
 * record), and va_arg advances that tag in place — so a copy that merely
 * copied the pointer would advance the original too. Each check below
 * reads from both lists after they diverge, with integer and floating
 * arguments spread past the register save areas into the stack overflow
 * area, and hands a copy to a function taking a va_list, the vprintf shape.
 */
// expect-exit: 42
#include <stdarg.h>

/* sums n ints from a list it is handed — consumes ITS list only */
static long vsum(int n, va_list ap)
{
    long s = 0;
    while (n-- > 0)
        s += va_arg(ap, int);
    return s;
}

/* 12 ints: past the six (x86-64) / eight (aarch64) integer registers */
static int ints(int n, ...)
{
    va_list a, b;
    va_start(a, n);
    int first = va_arg(a, int);           /* a has consumed 1 */
    va_copy(b, a);                        /* b starts at the 2nd */
    long rest_b = vsum(n - 1, b);         /* b consumed to the end */
    int second = va_arg(a, int);          /* a must be unaffected by b */
    long rest_a = vsum(n - 2, a);
    va_end(b);
    va_end(a);
    if (first != 1 || second != 2)
        return 1;
    /* 1..12 sums to 78 */
    if (rest_b != 78 - 1 || rest_a != 78 - 3)
        return 2;
    return 0;
}

/* doubles and ints interleaved, 10 of each: past the FP registers too */
static int mixed(int n, ...)
{
    va_list a;
    va_start(a, n);
    double dsum_a = 0, dsum_c = 0;
    long isum_a = 0, isum_c = 0;
    for (int i = 0; i < n; i++) {
        va_list c;
        va_copy(c, a);                    /* a fresh copy every iteration */
        double d = va_arg(a, double);
        int k = va_arg(a, int);
        dsum_a += d;
        isum_a += k;
        /* the copy reads the same pair a just read */
        double dc = va_arg(c, double);
        int kc = va_arg(c, int);
        if (dc != d || kc != k)
            return 3;
        dsum_c += dc;
        isum_c += kc;
        va_end(c);
    }
    va_end(a);
    if (dsum_a != 27.5 || isum_a != 55 || dsum_c != dsum_a || isum_c != isum_a)
        return 4;
    return 0;
}

/* a copy of a copy, taken at different points */
static int chain(int n, ...)
{
    va_list a, b, c;
    va_start(a, n);
    va_copy(b, a);
    (void)va_arg(b, int);
    va_copy(c, b);                        /* c starts at the 2nd */
    (void)va_arg(b, int);
    int ra = va_arg(a, int), rb = va_arg(b, int), rc = va_arg(c, int);
    va_end(c);
    va_end(b);
    va_end(a);
    return ra == 10 && rb == 30 && rc == 20 ? 0 : 5;
}

int main(void)
{
    int r;
    if ((r = ints(12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)) != 0)
        return r;
    if ((r = mixed(10, 0.5, 1, 1.0, 2, 1.5, 3, 2.0, 4, 2.5, 5,
                       3.0, 6, 3.5, 7, 4.0, 8, 4.5, 9, 5.0, 10)) != 0)
        return r;
    if ((r = chain(3, 10, 20, 30)) != 0)
        return r;
    return 42;
}
