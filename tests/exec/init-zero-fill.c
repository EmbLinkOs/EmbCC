// expect-exit: 42
/* An initializer zero-fills whatever it does not mention (C11 6.7.9p21).
 * The fill is skipped only when the listed values write every byte; each
 * function here is a case where some member C requires to be zero is left
 * to the fill. (Not a union's other bytes, nor padding: C leaves those
 * indeterminate, and GCC, which every test here is compared with, does.) */
struct whole { int a, b; };
struct part { char c[3]; short s; };
struct pair { int a, b; };

__attribute__((noinline)) static struct whole mk(int x)
{
    struct whole w = { x, x + 1 };
    return w;
}

__attribute__((noinline)) static int copied(int x)
{
    struct whole w = mk(x);         /* every byte: no fill needed */
    return w.a + w.b;
}

__attribute__((noinline)) static int unmentioned(char a)
{
    struct part p = { { a, a, a } };    /* s is not mentioned */
    return p.s;
}

__attribute__((noinline)) static int array_tail(int x)
{
    int v[4] = { x };               /* v[1..3] are zero */
    return v[1] + v[2] + v[3] + (v[0] == x ? 0 : 100);
}

__attribute__((noinline)) static int literal(int x)
{
    struct pair *q = &(struct pair){ x };   /* b is zero */
    return q->b + (q->a == x ? 0 : 100);
}

/* Nonzero bytes where the next call's frame will be, so a fill wrongly
 * skipped reads garbage rather than a lucky zero. */
__attribute__((noinline)) static void scribble(void)
{
    volatile unsigned char junk[256];
    for (int i = 0; i < 256; i++)
        junk[i] = (unsigned char)(0x5a ^ i);
}

int main(void)
{
    scribble();
    if (copied(20) != 41) return 1;
    scribble();
    if (unmentioned(3) != 0) return 2;
    scribble();
    if (array_tail(9) != 0) return 3;
    scribble();
    if (literal(7) != 0) return 4;
    return 42;
}
