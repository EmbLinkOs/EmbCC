// expect-exit: 42
/* `= {}` zero-initializes the whole object (C23, and a GNU extension
 * before it). irgen cleared a local aggregate only when the flattened
 * initializer had at least one leaf, and `{}` has none: `int a[4] = {}`
 * and `struct p s = {}` emitted no code and kept whatever the stack held,
 * on every target at every level. The stack is dirtied first so that the
 * garbage is not zero by luck. */
struct p { int x, y; };
struct n { struct p a; char tag[6]; long long big; };

__attribute__((noinline)) static void dirty(void)
{
    volatile unsigned char junk[512];
    for (int i = 0; i < 512; i++)
        junk[i] = (unsigned char)(0x5a + i);
}

__attribute__((noinline)) static int test(void)
{
    int a[4] = {};
    struct p s = {};
    struct n deep = {};
    union { int i; char c[8]; } u = {};
    int bad = 0;
    for (int i = 0; i < 4; i++)
        if (a[i]) bad |= 1;
    if (s.x || s.y) bad |= 2;
    if (deep.a.x || deep.a.y || deep.big) bad |= 4;
    for (int i = 0; i < 6; i++)
        if (deep.tag[i]) bad |= 8;
    for (int i = 0; i < 8; i++)
        if (u.c[i]) bad |= 16;
    return bad;
}

int main(void)
{
    dirty();
    int r = test();
    return r ? r : 42;
}
