/* Values that are live across a call OUTSIDE a loop and used inside it:
 * the live-range split gives each another name inside the loop, copied
 * in on entry and back on every exit. Every shape of exit is here -- the
 * latch falling out, a break (a branch out through a trampoline), a
 * return from inside, a do-while, a nested loop, a value only READ in
 * the loop, two values at once -- and every expectation is a closed
 * form or a host-checked constant. g is written by the calls so that
 * they are real calls with real clobbers. */
// expect-exit: 42
#define NI __attribute__((noinline))
static long g;
NI void sink(long v) { g += v; }
NI long opaque(long a, long b) { return a + b; }

NI long f_read(long n, long k)          /* k only read inside; s carried */
{
    long s = k;
    sink(1);
    for (long i = 0; i < n; i++) s += i ^ k;
    sink(2);
    return s + k;
}
NI long f_break(long n)                 /* a break and the normal exit; two values */
{
    long s = 0, t = 1;
    sink(0);
    for (long i = 0; i < n; i++) {
        s += i;
        if (s > 50) break;
        t *= 2;
    }
    sink(s);
    return s * 1000 + t;
}
NI long f_nested(long n)
{
    long s = 0;
    sink(0);
    for (long i = 0; i < n; i++) {
        for (long j = 0; j < 3; j++) s += j;
        s += i;
    }
    return opaque(s, 1);
}
NI long f_dowhile(long n)
{
    long s = 7;
    sink(0);
    do { s = s * 3 + 1; } while (s < n);
    sink(s);
    return s;
}
NI long f_return(long n)                /* a return from inside the loop */
{
    long s = 0;
    sink(0);
    for (long i = 0; i < n; i++) {
        if (i == 5) return s + g;
        s += i;
    }
    return s;
}
NI long f_cond_exit(long n, long m)     /* both arms of a branch leave the loop */
{
    long s = 3;
    sink(0);
    for (long i = 0; i < n; i++) {
        s += i;
        if (s > m) break;
    }
    sink(1);
    return s;
}
NI long f_two(long n, long a, long b)   /* two carried values, both live after */
{
    sink(0);
    for (long i = 0; i < n; i++) { a += b; b ^= i; }
    sink(a);
    sink(b);
    return a * 7 + b;
}
/* A VOID function that falls off its end: the split's exit trampolines
 * are placed after the last block, and one that is fallen into instead
 * of returned from re-runs the code after the loop (a red-black tree's
 * erase in lib/libcxx looped forever on exactly this). Walks to the
 * leftmost node the way an rb-tree's minimum does. */
struct nd { struct nd *left; long v; };
static long tailsum;
NI void f_tail(struct nd *n, long *out, int extra)
{
    sink(1);
    if (extra <= 100)
        while (n->left) n = n->left;
    else
        tailsum += 1000;
    sink(n->v);
    *out = n->v + (n->left != 0) * 1000;   /* n itself, read after the call */
    if (extra)
        tailsum += extra;
}
NI void f_tail2(struct nd *n, long *out, int extra)   /* the loop's value used directly */
{
    sink(1);
    while (n->left) n = n->left;
    sink(n->v);
    *out = n->v + (n->left != 0) * 1000;
    if (extra)
        tailsum += extra;
}
int main(void)
{
    int bad = 0;
    g = 0;
    /* s = k + sum(i ^ k) for i < n; n = 10, k = 5: i^5 = 5,4,7,6,1,0,3,2,13,12 -> 53 */
    if (f_read(10, 5) != 5 + 53 + 5) bad |= 1;
    if (f_read(0, 9) != 18) bad |= 1;
    /* s: 0,1,3,6,10,15,21,28,36,45,55 -> breaks at i=10 (s=55), t doubled 10 times = 1024 */
    if (f_break(20) != 55 * 1000 + 1024) bad |= 2;
    if (f_break(3) != 3 * 1000 + 8) bad |= 2;
    /* n = 4: each i adds 3 + i -> 3*4 + 6 = 18, +1 */
    if (f_nested(4) != 19) bad |= 4;
    if (f_nested(0) != 1) bad |= 4;
    /* 7 -> 22 -> 67 -> 202: first >= 100 is 202 */
    if (f_dowhile(100) != 202) bad |= 8;
    if (f_dowhile(0) != 22) bad |= 8;
    g = 0;
    if (f_return(10) != 10) bad |= 16;         /* s = 0+1+2+3+4 = 10, g = 0 */
    if (f_return(3) != 3) bad |= 16;
    if (f_cond_exit(10, 20) != 24) bad |= 32;  /* 3,3,4,6,9,13,18,24 > 20 */
    if (f_cond_exit(2, 100) != 4) bad |= 32;
    /* a,b: (1,2) i=0: a=3,b=2; i=1: a=5,b=3; i=2: a=8,b=1; i=3: a=9,b=2 */
    if (f_two(4, 1, 2) != 9 * 7 + 2) bad |= 64;
    if (g == 0) bad |= 128;
    {
        struct nd a = { 0, 11 }, b = { &a, 22 }, c = { &b, 33 };
        long o = 0;
        tailsum = 0;
        f_tail(&c, &o, 5);                 /* walks to a: a.left is 0 */
        if (o != 11 || tailsum != 5) return 99;
        f_tail(&a, &o, 0);
        if (o != 11 || tailsum != 5) return 98;
        f_tail(&c, &o, 200);               /* stays at c: c.left is set */
        if (o != 1033 || tailsum != 1205) return 97;
        f_tail2(&c, &o, 7);
        if (o != 11 || tailsum != 1212) return 96;
        f_tail2(&a, &o, 0);
        if (o != 11 || tailsum != 1212) return 95;
    }
    if (bad) return bad;
    return 42;
}
