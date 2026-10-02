/* A struct passed by value in memory is built where the callee will read
 * it -- the outgoing argument area -- when it is a temporary made for the
 * call. Everything that must stop that is here as well: a call between
 * the struct's construction and its own call (it would write the same
 * area), two such structs alive at once, a struct read after the call,
 * one passed to two calls, a callee that writes its parameter, loops,
 * 16-byte alignment, a struct returned in memory alongside, and stack
 * arguments of other kinds around it. Each callee
 * checks every byte it receives. */
// expect-exit: 42

typedef struct { int a, b; long c[18]; } P;                 /* 152 bytes */
typedef struct { long long v[4]; char t[40] __attribute__((aligned(16))); } Q;
typedef struct { long r[20]; } R;                           /* returned in memory */

static volatile int vthree = 3;
static long sum_p(const P *p)
{ long s = p->a * 3 + p->b * 5; for (int k = 0; k < 18; k++) s += p->c[k] * (k + 1); return s; }
static P mk(int a, int b, long base)
{ P p; p.a = a; p.b = b; for (int k = 0; k < 18; k++) p.c[k] = base + k; return p; }

static volatile long sink;
__attribute__((noinline)) void touch(const P *p) { sink = p->a + p->c[3]; }
/* writes its parameter, and keeps the writes: touch reads them */
__attribute__((noinline)) long take(P p) { long s = sum_p(&p); p.a = -1; p.c[3] = 99; touch(&p); return s; }
__attribute__((noinline)) long take2(int x, P p, P q, int y) { return x + sum_p(&p) * 7 + sum_p(&q) + y; }
__attribute__((noinline)) int side(int v) { volatile P scratch = { v, v }; (void)scratch; return v + 1; }
__attribute__((noinline)) long take_q(Q q)
{ long s = 0; for (int k = 0; k < 4; k++) s += q.v[k] << k; for (int k = 0; k < 40; k++) s += q.t[k]; return s; }
__attribute__((noinline)) R take_ret(P p, int k)
{ R r; for (int i = 0; i < 20; i++) r.r[i] = sum_p(&p) + i * k; return r; }
__attribute__((noinline)) long named_after(int at)
{ P k = { 8, 9 }; long t = take(k); return t * 1000 + k.c[3] + at * 0; }
__attribute__((noinline)) long many(long a1, long a2, long a3, long a4, long a5, long a6, long a7, P p, long a8)
{ return a1 + 2*a2 + 3*a3 + 4*a4 + 5*a5 + 6*a6 + 7*a7 + 8*a8 + sum_p(&p); }

int main(void)
{
    int bad = 0;
    long want = sum_p(&(P){ 1, 2, { 3, 4, 5 } });
    if (take((P){ 1, 2, { 3, 4, 5 } }) != want) bad |= 1;

    /* two at once, in one call */
    P a = mk(3, 4, 10), b = mk(5, 6, 20);
    if (take2(7, (P){ 3, 4, { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 } },
              (P){ 5, 6, { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37 } }, 9)
        != 7 + sum_p(&a) * 7 + sum_p(&b) + 9) bad |= 2;

    /* a call between the literal and its call: may not share the area */
    if (take2(side(1), (P){ 3, 4, { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 } },
              (P){ 5, 6, { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37 } }, side(8))
        != 2 + sum_p(&a) * 7 + sum_p(&b) + 9) bad |= 4;

    /* a call that itself passes a struct on the stack, between the
     * literals and their call: it writes the area they would be in */
    P m = mk(2, 3, 50);
    if (take2(9, (P){ 3, 4, { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 } },
              (P){ 5, 6, { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37 } },
              (int)take(m))
        != 9 + sum_p(&a) * 7 + sum_p(&b) + (int)sum_p(&m) || m.c[3] != 53) bad |= 4;

    /* a named struct read after the call, its address used for nothing
     * else: the callee writes its copy, which must not be this */
    {
        P k = { 8, 9, { 0, 0, 0, 7 } };
        long t = take(k);
        int at = vthree;              /* a read the optimizer cannot fold */
        if (k.c[at] != 7 || t != 8 * 3 + 9 * 5 + 7 * 4) bad |= 8;
        if (named_after(at) != (8 * 3 + 9 * 5) * 1000) bad |= 8;  /* take wrote its c[3] */
    }

    /* a named struct read after the call, and one passed twice */
    P n = mk(8, 9, 100);
    long t1 = take(n);
    if (t1 != sum_p(&n) || n.a != 8 || n.c[3] != 103) bad |= 8;
    if (take(n) + take(n) != 2 * t1) bad |= 16;

    /* in a loop */
    long acc = 0, wacc = 0;
    for (int i = 0; i < 10; i++) {
        acc += take((P){ i, i * 2, { i, i + 1 } });
        P w = { i, i * 2, { i, i + 1 } };
        wacc += sum_p(&w);
    }
    if (acc != wacc) bad |= 32;

    /* aligned(16) member */
    Q q = { { 1, 2, 3, 4 }, "abcdefghijklmnopqrstuvwxyz0123456789ABC" };
    long qs = 0; for (int k = 0; k < 4; k++) qs += q.v[k] << k; for (int k = 0; k < 40; k++) qs += q.t[k];
    if (take_q((Q){ { 1, 2, 3, 4 }, "abcdefghijklmnopqrstuvwxyz0123456789ABC" }) != qs) bad |= 64;

    /* a struct return alongside, more stack arguments */
    R r = take_ret((P){ 1, 2, { 3, 4, 5 } }, 3);
    if (r.r[0] != want || r.r[19] != want + 57) bad |= 256;
    if (many(1, 2, 3, 4, 5, 6, 7, (P){ 1, 2, { 3, 4, 5 } }, 8) != 1+4+9+16+25+36+49+64 + want) bad |= 512;

    return bad ? 100 + (bad & 0x7f) + (bad >> 7 ? 1 : 0) : 42;
}
