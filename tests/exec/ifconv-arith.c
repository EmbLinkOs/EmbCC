/* A choice made with arithmetic on a comparison's 0 or 1 (src/opt/
 * ifconv.c, ifconv_arith; RISC-V, MIPS and LoongArch): two constants
 * that differ by a power of two, and a value against itself plus or
 * minus one. Every shape is checked against the same choice made by
 * indexing a two-entry table with the condition, over conditions that
 * are masks (not 0 or 1) and comparisons, both polarities, a zero and a
 * non-zero base, differences of +-1, +-2 and +-8, and 64-bit values. */
// expect-exit: 42
typedef unsigned int u32;
typedef long long i64;

static volatile int sink;

__attribute__((noinline)) int k20(u32 x) { return (x & 15) ? 2 : 0; }
__attribute__((noinline)) int k02(u32 x) { return (x & 15) ? 0 : 2; }
__attribute__((noinline)) int k53(u32 x) { return (x & 6) ? 5 : 3; }
__attribute__((noinline)) int k3n5(u32 x) { return x > 7 ? 3 : 11; }
__attribute__((noinline)) int k10(int x) { return x < 0 ? 1 : 0; }
__attribute__((noinline)) int km1(u32 x) { return (x & 1) ? -1 : 0; }

__attribute__((noinline)) u32 inc_if_zero(const u32 *p, int n)
{
    u32 e = 0;
    for (int i = 0; i < n; i++) {
        u32 len = p[i] & 15;
        if (!len) e++;
    }
    return e;
}
__attribute__((noinline)) int dec_if_set(const u32 *p, int n)
{
    int e = 100;
    for (int i = 0; i < n; i++) {
        if (p[i] & 4) e--;
    }
    return e;
}
__attribute__((noinline)) u32 add8_if_less(const u32 *p, int n)
{
    u32 e = 1;
    for (int i = 0; i < n; i++) {
        if (p[i] < 300) e += 8;
    }
    return e;
}
__attribute__((noinline)) i64 wide(const u32 *p, int n)
{
    i64 e = 0x100000000LL;
    for (int i = 0; i < n; i++) {
        if (p[i] & 2) e -= 4;
    }
    return e;
}

int main(void)
{
    static const u32 v[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 30, 31, 64,
                             255, 256, 299, 300, 301, 1000, 0x80000000u,
                             0xfffffff0u, 0xffffffffu };
    const int n = (int)(sizeof v / sizeof v[0]);
    int bad = 0;
    u32 ez = 0, add8 = 1; int dec = 100; i64 w = 0x100000000LL;
    for (int i = 0; i < n; i++) {
        u32 x = v[i];
        const int t20[2] = { 0, 2 }, t02[2] = { 2, 0 }, t53[2] = { 3, 5 },
                  t3n[2] = { 11, 3 }, t10[2] = { 0, 1 }, tm1[2] = { 0, -1 };
        sink = i;
        if (k20(x) != t20[(x & 15) != 0]) bad |= 1;
        if (k02(x) != t02[(x & 15) != 0]) bad |= 1;
        if (k53(x) != t53[(x & 6) != 0]) bad |= 2;
        if (k3n5(x) != t3n[x > 7]) bad |= 2;
        if (k10((int)x) != t10[(int)x < 0]) bad |= 4;
        if (km1(x) != tm1[x & 1]) bad |= 4;
        ez += (x & 15) == 0;
        dec -= (x & 4) != 0;
        add8 += 8u * (x < 300);
        w -= 4 * (i64)((x & 2) != 0);
    }
    if (inc_if_zero(v, n) != ez) bad |= 8;
    if (dec_if_set(v, n) != dec) bad |= 16;
    if (add8_if_less(v, n) != add8) bad |= 32;
    if (wide(v, n) != w) bad |= 64;
    return bad ? bad : 42;
}
