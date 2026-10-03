// expect-exit: 42
/* A load fused into the operation it feeds (`xor (%r9,%rsi,4), %edi` on
 * x86-64) has to read memory as it stood where the source read it. The
 * load is moved down to its use first, so each function here is a way
 * that move would be wrong: a store through a pointer that may alias it,
 * a call, the address register reused for the result -- and the plain
 * cases, each operation and a scaled index, for the arithmetic. */
typedef unsigned int u32;
#define REL_S 25493832          /* what Clang computes for the loops below */
#define REL_U 36758L

__attribute__((noinline)) static u32 table(const u32 *t, const unsigned char *b,
                                           int n, u32 c)
{
    for (int i = 0; i < n; i++)
        c = t[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c;
}

__attribute__((noinline)) static long ops(const long *p, long x)
{
    long a = x + p[0];
    long s = x - p[1];
    long n = x & p[2];
    long o = x | p[3];
    return a * 1000 + s * 100 + n * 10 + o;
}

__attribute__((noinline)) static int past_store(int *p, int *q, int z)
{
    int v = *p;
    *q = 5;                         /* p and q may be the same */
    return z + v;
}

__attribute__((noinline)) static int bump(int *p) { return ++*p; }

__attribute__((noinline)) static int past_call(int *p, int z)
{
    int v = *p;
    int w = bump(p);                /* changes *p */
    return z + v + w * 100;
}

__attribute__((noinline)) static long self(long *p)
{
    long v = *p;                    /* the address, then its contents */
    return (long)p ^ v;
}

__attribute__((noinline)) static long minus(long *p, long y)
{
    return y - *p;
}

/* The result's register is the one the address died in: copying the
 * first operand there before reading memory would read through the
 * copy. Each keeps its first operand live past the operation. */
__attribute__((noinline)) static long hz1(long *p, long y, long *out)
{
    long r = y + *p;
    *out = y;
    return r;
}
__attribute__((noinline)) static long hz2(long y, long *p)
{
    long r = y - *p;
    return r * y;
}
__attribute__((noinline)) static int hz3(int y, const int *t, long i)
{
    int r = y + t[i];
    return r * y;
}

/* `x[i-1] -= x[i]`: read-modify-write fusion turns the load, the
 * subtraction and the store back into `sub %reg, (mem)`, so it owns the
 * subtraction; fusing the other load into it as well read a register
 * nothing had loaded (a stack machine's arithmetic, which looped). */
__attribute__((noinline)) static void rmw(unsigned *st, int sp)
{
    st[sp - 1] -= st[sp];
    st[sp - 2] ^= st[sp - 1];
    st[sp - 3] += st[sp - 2];
}

__attribute__((noinline)) static void rmw1(unsigned *st, int sp)
{
    st[sp - 1] -= st[sp];
}

/* Compares read their operand from memory too, and one with the loaded
 * value FIRST is turned around (`a < b` as `b > a`): every relation,
 * signed and unsigned, from each side and against a constant. */
__attribute__((noinline)) static int rel_s(const int *p, int x)
{
    /* one load per comparison, so each is that compare's own */
    return (p[0] < x) | (p[1] <= x) << 1 | (p[2] > x) << 2 |
           (p[3] >= x) << 3 | (p[4] == x) << 4 | (p[5] != x) << 5 |
           (x < p[6]) << 6 | (x <= p[7]) << 7 | (x > p[8]) << 8 |
           (x >= p[9]) << 9 | (p[10] < 7) << 10 | (p[11] >= -7) << 11;
}
__attribute__((noinline)) static int rel_u(const unsigned long *p,
                                           unsigned long x)
{
    return (p[0] < x) | (p[1] <= x) << 1 | (p[2] > x) << 2 |
           (p[3] >= x) << 3 | (x < p[4]) << 4 | (x > p[5]) << 5 |
           (p[6] > 7) << 6;
}

int main(void)
{
    u32 t[256];
    unsigned char b[5] = { 1, 2, 3, 4, 5 };
    for (u32 i = 0; i < 256; i++)
        t[i] = i * 0x01010101u;
    if (table(t, b, 5, 0xffffffffu) != 0xfef9f8fbu) return 1;
    long p[4] = { 1, 2, 6, 8 };
    if (ops(p, 3) != 4 * 1000 + 1 * 100 + 2 * 10 + 11) return 2;
    int a = 1, c = 1;
    if (past_store(&a, &c, 10) != 11) return 3;
    if (past_store(&a, &a, 10) != 11) return 4;   /* v read before *q = 5 */
    int d = 7;
    if (past_call(&d, 10) != 17 + 800) return 5;
    long e = 0x55;
    if ((self(&e) ^ (long)&e) != 0x55) return 6;
    long f = 30;
    if (minus(&f, 100) != 70) return 7;
    long g = 5, h = 0;
    if (hz1(&g, 7, &h) != 12 || h != 7) return 8;
    if (hz2(10, &g) != 50) return 9;
    int tt[3] = { 1, 2, 3 };
    if (hz3(4, tt, 2) != 28) return 10;
    unsigned st[4] = { 100, 40, 9, 2 };
    rmw(st, 3);
    if (st[2] != 7 || st[1] != 47 || st[0] != 147) return 11;
    unsigned su[2] = { 50, 8 };
    rmw1(su, 1);
    if (su[0] != 42) return 12;
    int ps[12] = { 3, 9, -9, 5, 7, 1, 4, 6, 8, 2, -9, -8 };
    int rs = 0;
    for (int x = 1; x <= 11; x += 2)
        rs = rs * 7 + rel_s(ps, x);
    if (rs != REL_S) return 13;
    unsigned long pu[7] = { 3, 2, ~0UL, 4, 5, ~0UL - 1, 9 };
    long ru = 0;
    for (unsigned long x = 1; x <= 5; x += 2)
        ru = ru * 7 + rel_u(pu, x);
    ru = ru * 7 + rel_u(pu, ~0UL);
    if (ru != REL_U) return 14;
    return 42;
}
