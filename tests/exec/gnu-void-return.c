/* GNU C: `return f();` in a void function, f void, makes the call and
 * returns nothing. */
// expect-exit: 42
#include <stdio.h>
static int calls;
static void bump(void) { calls++; }
static void twice(void) { bump(); return bump(); }
static void cond(int x) { if (x) return bump(); calls += 10; }
int main(void)
{
    twice();
    cond(1);
    cond(0);
    printf("%d\n", calls);
    return calls == 13 ? 42 : 1;
}
