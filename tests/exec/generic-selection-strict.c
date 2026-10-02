// expect-exit: 42
/* _Generic picks the association whose type is the controlling type, and
 * C keeps apart what this compiler's assignment rules merge: `long` and
 * `long long`, plain `char` and `signed char`, `int *` and
 * `volatile int *`. The first association that matched loosely was
 * taken, so _Generic(1L, long long: 1, long: 2) gave 1 -- and a
 * type-generic macro chose the wrong function with no diagnostic. */
#define K(x) _Generic((x), long long: 1, long: 2, default: 3)
#define C(x) _Generic((x), char: 1, signed char: 2, unsigned char: 3, default: 4)

int main(void)
{
    volatile int vi = 0;
    char ch = 0;
    if (K(1L) != 2 || K(1LL) != 1) return 1;
    if (C(ch) != 1 || C((signed char)1) != 2 || C((unsigned char)1) != 3)
        return 2;
    if (_Generic((volatile int *)0, int *: 1, volatile int *: 2, default: 3) != 2)
        return 3;
    if (_Generic(vi, int: 1, default: 2) != 1) return 4;
    if (_Generic((unsigned short)1, unsigned short: 1, int: 2, default: 3) != 1)
        return 5;
    return 42;
}
