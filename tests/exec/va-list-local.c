/* A va_list held in a register. Where the list is a bare pointer
 * (AAPCS32, RISC-V, Apple arm64), va_arg on a local list reads and writes
 * it as a variable rather than through its address, so the list can live
 * in a register -- and must still agree with every access that does go
 * through the address: va_start, va_copy, a callee given &ap, and a
 * second va_start after va_end. Each shape is checked against what C
 * says it must produce. */
// expect-exit: 42
#include <stdarg.h>

struct pt { int x, y; };

/* the list as a parameter, advanced in a loop, conditionally */
static int __attribute__((noinline)) vsum(int n, va_list ap)
{
    int s = 0;
    while (n-- > 0) {
        int k = va_arg(ap, int);
        if (k < 0)
            k = -va_arg(ap, int);     /* a second read on one path */
        s += k;
    }
    return s;
}
static int sum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = vsum(n, ap);
    va_end(ap);
    return s;
}

/* a callee advancing the caller's list through its address */
static long long __attribute__((noinline)) take2(va_list *pp)
{
    long long a = va_arg(*pp, long long);
    int b = va_arg(*pp, int);
    return a * 10 + b;
}
static long long mixed(int tag, ...)
{
    va_list ap;
    va_start(ap, tag);
    int first = va_arg(ap, int);           /* by variable */
    long long mid = take2(&ap);            /* through its address */
    double d = va_arg(ap, double);         /* by variable, after it */
    struct pt p = va_arg(ap, struct pt);
    const char *s = va_arg(ap, const char *);
    va_end(ap);
    return tag + first + mid + (long long)(d * 4) + p.x * p.y + (s[0] - 'a');
}

/* va_copy, both lists advanced independently */
static int copies(int n, ...)
{
    va_list ap, bp;
    va_start(ap, n);
    int a0 = va_arg(ap, int);
    va_copy(bp, ap);
    int a1 = va_arg(ap, int), a2 = va_arg(ap, int);
    int b1 = va_arg(bp, int);
    va_end(bp);
    va_end(ap);
    return (a0 == n) + (a1 == n + 1) + (a2 == n + 2) + (b1 == n + 1);
}

/* the list started twice */
static int twice(int n, ...)
{
    va_list ap;
    int s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    va_start(ap, n);
    s += 100 * va_arg(ap, int);           /* the first again */
    va_end(ap);
    return s;
}

int main(void)
{
    if (sum(5, 1, 2, -1, 3, 4, 5) != 1 + 2 - 3 + 4 + 5)
        return 1;
    if (sum(0) != 0)
        return 2;
    struct pt p = { 3, 7 };
    /* 1 + 2 + (40000000000 * 10 + 5) + 10 + 21 + 2 */
    if (mixed(1, 2, 40000000000LL, 5, 2.5, p, "cat") != 400000000041LL)
        return 3;
    if (copies(9, 9, 10, 11) != 4)
        return 4;
    if (twice(3, 4, 5, 6) != 4 + 5 + 6 + 400)
        return 5;
    return 42;
}
