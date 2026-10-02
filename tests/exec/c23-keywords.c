/* C23 spells static_assert, alignas, alignof and thread_local as plain
 * keywords (gcc 15 compiles C23 by default), and lets a label end a
 * block. EmbCC knew only the _Capitalized forms and wanted a statement
 * after every label. And _Alignas among a FILE-SCOPE declaration's
 * specifiers was dropped before anything read it: `alignas(16) static
 * int g` sat at its type's alignment, as did `_Alignas(64) int buf`. */
// expect-exit: 42
// no-gcc-reference: C23 keywords; the reference builds strict C11
#include <stddef.h>
static_assert(sizeof(int) >= 2);
static_assert(1, "with a message");
alignas(16) static int g = 1;
static thread_local int t = 1;     /* declared only: the bare-metal harness has no thread pointer */
int *tls_addr(void) { return &t; }
struct s { alignas(8) char c; };
static int seen;
void f(int x)
{
    if (x > 0)
        goto out;
    seen = -x;
out:
}
int main(void)
{
    static_assert(alignof(int) <= 8, "x");
    f(-1);
    {
        goto end;
    end:
    }
    return (((size_t)&g % 16) == 0) + (int)alignof(struct s) + seen + 32;
}
