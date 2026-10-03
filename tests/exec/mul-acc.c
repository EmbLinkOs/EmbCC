// expect-exit: 42
/* A product whose one reader is the add or subtract right after it is
 * one multiply-accumulate on ARMv7-M (mla, mls) and AArch64 (madd, msub):
 * c + a*b and c - a*b. a*b - c has no such instruction and must stay a
 * multiply and a subtract -- fusing it would compute c - a*b, the
 * negation. A product with a second reader must still be computed.
 * The operands alias each other in every combination the fusion reads
 * them in, at 32 and 64 bits, wrapping and negative. */
typedef unsigned u32;
typedef unsigned long long u64;

__attribute__((noinline)) static int add_r(int a, int b, int c) { return c + a * b; }
__attribute__((noinline)) static int add_l(int a, int b, int c) { return a * b + c; }
__attribute__((noinline)) static int sub_r(int a, int b, int c) { return c - a * b; }
__attribute__((noinline)) static int sub_l(int a, int b, int c) { return a * b - c; }
__attribute__((noinline)) static int self(int a) { return a + a * a; }
__attribute__((noinline)) static int self2(int a, int b) { return a - a * b; }
__attribute__((noinline)) static int twice(int a, int b, int c, int *p)
{
    int m = a * b;
    int r = c + m;
    *p = m;                      /* a second reader, after the add */
    return r;
}
__attribute__((noinline)) static long long twice64(long long a, long long b,
                                                   long long c, long long *p)
{
    long long m = a * b;
    long long r = c - m;
    *p = m;
    return r;
}
__attribute__((noinline)) static u32 wrap(u32 a, u32 b, u32 c) { return c - a * b; }
__attribute__((noinline)) static long long add64(long long a, long long b, long long c)
{
    return c + a * b;
}
__attribute__((noinline)) static long long sub64(long long a, long long b, long long c)
{
    return c - a * b;
}
__attribute__((noinline)) static u64 rev64(u64 a, u64 b, u64 c) { return a * b - c; }

__attribute__((noinline)) static int dot(const short *x, const short *y, int n)
{
    int s = 0;
    for (int k = 0; k < n; k++)
        s += x[k] * y[k];
    return s;
}

__attribute__((noinline)) static int horner(const int *c, int n, int x)
{
    int r = 0;
    for (int k = 0; k < n; k++)
        r = r * x + c[k];
    return r;
}

__attribute__((noinline)) static int resid(const int *a, const int *b, int n, int s)
{
    int r = 1000;
    for (int k = 0; k < n; k++)
        r -= a[k] * b[k] * s;
    return r;
}

int main(void)
{
    short x[7] = { 3, -4, 5, -6, 7, -8, 9 }, y[7] = { -1, 2, -3, 4, -5, 6, -7 };
    int cs[5] = { 2, -3, 0, 7, -1 };
    int a[4] = { 1, -2, 3, -4 }, b[4] = { 5, 6, -7, 8 };
    int keep = 0;
    if (add_r(6, 7, 100) != 142 || add_l(-6, 7, 100) != 58) return 1;
    if (sub_r(6, 7, 100) != 58 || sub_r(-6, 7, 100) != 142) return 2;
    if (sub_l(6, 7, 100) != -58 || sub_l(-6, -7, 2) != 40) return 3;
    if (self(9) != 90 || self(-9) != 72 || self2(5, 3) != -10) return 4;
    if (twice(3, 4, 5, &keep) != 17 || keep != 12) return 5;
    {
        long long k64 = 0;
        if (twice64(-3, 4000000000LL, 5, &k64) != 12000000005LL ||
            k64 != -12000000000LL)
            return 13;
    }
    if (wrap(65536u, 65536u, 7u) != 7u || wrap(3u, 5u, 2u) != 4294967283u) return 6;
    if (add64(3000000000LL, 3, -1) != 8999999999LL) return 7;
    if (sub64(3000000000LL, 3, 1) != -8999999999LL) return 8;
    if (rev64(0x100000000ULL, 0x100000000ULL, 1) != 0xffffffffffffffffULL) return 9;
    if (dot(x, y, 7) != -196 || dot(x, y, 3) != -26 || dot(x, y, 0) != 0) return 10;
    if (horner(cs, 5, 3) != 2 * 81 - 3 * 27 + 0 * 9 + 7 * 3 - 1) return 11;
    if (resid(a, b, 4, 2) != 1000 - 2 * (5 - 12 - 21 - 32)) return 12;
    return 42;
}
