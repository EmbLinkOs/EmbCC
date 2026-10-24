/* quick_exit runs what at_quick_exit registered, last first, and nothing
 * atexit did: the atexit handler here would end the program with 1. The
 * status is the one quick_exit was given. */
// expect-exit: 42
#include <stdlib.h>

static int step;

static void never(void) { _Exit(1); }

static void third(void)
{
    if (step != 0)
        _Exit(10 + step);
    step = 1;
}
static void second(void)
{
    if (step != 1)
        _Exit(20 + step);
    step = 2;
}
static void first(void)
{
    if (step != 2)
        _Exit(30 + step);
}

int main(void)
{
    if (atexit(never) != 0)
        return 2;
    if (at_quick_exit(first) != 0 || at_quick_exit(second) != 0 ||
        at_quick_exit(third) != 0)
        return 3;
    quick_exit(42);
    return 4;
}
