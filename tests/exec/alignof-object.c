// expect-exit: 42
/* __alignof__ of an OBJECT counts the alignment it was declared with, not
 * only its type's: `int gx __attribute__((aligned(64)))` is 64-aligned,
 * and __alignof__(gx) answered the int's 4 -- code that sizes a buffer or
 * checks an address by it was wrong with no diagnostic. */
int gx __attribute__((aligned(64)));
char cx __attribute__((aligned(32)));
_Alignas(16) int ax;
struct s { char c; int m __attribute__((aligned(16))); } sv;

__attribute__((noinline)) static int locals(void)
{
    int lx __attribute__((aligned(8)));
    char la[4] __attribute__((aligned(16)));
    (void)lx; (void)la;
    return (int)(__alignof__(lx) * 100 + __alignof__(la));
}

int main(void)
{
    if (__alignof__(gx) != 64 || __alignof__(cx) != 32) return 1;
    if (__alignof__(ax) != 16 || __alignof__(sv.m) != 16) return 2;
    if (__alignof__(int) != _Alignof(int)) return 3;
    if (locals() != 816) return 4;
    return 42;
}
