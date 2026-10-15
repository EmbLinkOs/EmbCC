/* A self tail call handed the address of the caller's own local: the
 * call needs a fresh frame, and the tail-recursion pass, which reuses
 * the one frame as a loop, read the last level's x where the first
 * level's was asked for (GCC's 20000412-2.c). The same through an array,
 * whose address is taken by its use. */
// expect-exit: 42
#include <stdio.h>

__attribute__((noinline)) static int f(int a, int *y)
{
    int x = a;
    if (a == 0)
        return *y;
    return f(a - 1, &x);
}

__attribute__((noinline)) static int g(int a, const int *y)
{
    int buf[2] = { a, a * 2 };
    if (a == 0)
        return y[1];
    return g(a - 1, buf);
}

int main(void)
{
    int r1 = f(100, 0), r2 = g(5, 0);
    printf("%d %d\n", r1, r2);
    return r1 == 1 && r2 == 2 ? 42 : 1;
}
