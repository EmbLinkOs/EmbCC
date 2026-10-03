// expect-exit: 42
/* A local whose address is kept in a pointer variable is read through
 * that pointer -- also past a join, where the optimizer's forwarding
 * cannot follow the variable. At -O1 the union-forwarding pass judged
 * such a local unread and dropped its stores, so `p->x` read whatever the
 * stack held. */
struct pt { int x, y; };

__attribute__((noinline)) static int after_join(int a, int b, int c)
{
    struct pt s;
    s.x = a;
    s.y = b;
    struct pt *p = &s;
    int r = 0;
    if (c)
        r = 1;
    return p->x * 10 + p->y + r;
}

__attribute__((noinline)) static int in_loop(int n)
{
    int v[4];
    int *q = v;
    for (int i = 0; i < 4; i++)
        v[i] = i + n;
    int sum = 0;
    for (int i = 0; i < 4; i++)
        sum += q[i];
    return sum;                     /* 4n + 6 */
}

__attribute__((noinline)) static int literal(int c)
{
    struct pt *cl = (struct pt[]){ 1, 2, 3, 4 };
    int r = c ? 1 : 0;
    return cl[1].x * 10 + cl[1].y + r;
}

/* Nonzero bytes where the next call's frame will be, so a dropped store
 * reads garbage rather than a lucky zero. */
__attribute__((noinline)) static void scribble(void)
{
    volatile unsigned char junk[256];
    for (int i = 0; i < 256; i++)
        junk[i] = (unsigned char)(0x5a ^ i);
}

int main(void)
{
    scribble();
    if (after_join(4, 1, 1) != 42) return 1;
    scribble();
    if (in_loop(9) != 42) return 2;
    scribble();
    if (literal(1) != 35) return 3;
    return 42;
}
