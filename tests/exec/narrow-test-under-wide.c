/* A branch on a value's low half, under a branch on the whole value: the
 * whole being nonzero says nothing about the half. The optimizer took
 * the dominating test's answer for both, dropped `if (yy == 0)`, and
 * divided by the zero it guarded (GCC's arith-rand.c). The converse --
 * the half nonzero under the whole's test -- and the sound inferences
 * (whole zero, so half zero) are checked too. */
// expect-exit: 42
#include <stdio.h>

static volatile unsigned long long vy = 0x1ffffff000000000ULL, vz = 0;
static volatile unsigned long long vx = 0x3fffff, vw = 0x100000000ULL;

__attribute__((noinline)) static int guarded(unsigned long long x,
                                             unsigned long long y)
{
    unsigned long long q = 0;
    if (y == 0)
        return 10;
    q = x / y;
    {
        unsigned int xx = (unsigned int)x, yy = (unsigned int)y;
        if (yy == 0)
            return (int)q + 42;          /* y's low half is zero */
        return (int)(xx / yy) + 1;
    }
}

__attribute__((noinline)) static int halves(unsigned long long w)
{
    unsigned int lo = (unsigned int)w;
    if (lo != 0)
        return 1;
    if (w != 0)
        return 2;                         /* low half zero, whole not */
    return 3;
}

__attribute__((noinline)) static int whole_zero(unsigned long long w)
{
    unsigned int lo = (unsigned int)w;
    if (w == 0) {
        if (lo == 0)
            return 4;                     /* follows: whole zero */
        return 5;
    }
    return 6;
}

int main(void)
{
    int a = guarded(vx, vy), b = halves(vw), c = whole_zero(vz);
    printf("%d %d %d\n", a, b, c);
    return a == 42 && b == 2 && c == 4 ? 42 : 1;
}
