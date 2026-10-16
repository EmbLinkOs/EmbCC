/* GNU C: `$` in identifiers -- in the middle, at the start and at the end
 * of a name, and in a macro's name and its pasted tokens. */
// expect-exit: 42
#include <stdio.h>
#define MAKE$NAME(x) x##$tail
static int a$b = 20;
static int $lead(int v) { return v + 1; }
int MAKE$NAME(head) = 1;
int main(void)
{
    int x$ = a$b + head$tail;
    printf("%d\n", $lead(x$) + 20);
    return $lead(x$) + 20;
}
