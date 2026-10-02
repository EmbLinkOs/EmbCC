/* A parameter used as a plain local: its incoming value is never read, it
 * is only assigned. The prologue still stores the incoming value into its
 * slot at the entry -- and slot sharing, judging by liveness, started such
 * a parameter at its first assignment, so it could be given the slot of
 * a parameter read AFTER the entry: x86-64 put `u8 a3` in the slot of
 * `u64 a1`, and the prologue's store of a3 overwrote a1 before the body
 * read it. Found by random programs at -O0. Each function here reads an
 * early parameter after the entry and reuses a later one, at several
 * widths, with and without other locals competing for the slots. */
// expect-exit: 42
typedef unsigned long long u64;
static volatile u64 g;

__attribute__((noinline)) static u64 reuse8(signed char a0, u64 a1, unsigned a2,
                                            unsigned char a3)
{
    g = a1;
    a3 = (unsigned char)(g * 3u);
    return g + a3 + (u64)a0 + a2;
}
__attribute__((noinline)) static u64 reuse32(u64 a, unsigned b, u64 c)
{
    u64 t = a + 1;
    g = t;
    b = (unsigned)(g >> 8);
    c = g ^ b;
    return c + b;
}
__attribute__((noinline)) static int reuse_all(int x, int y, int z)
{
    int s = x * 7 + y;
    x = s + 1;
    y = x * 2;
    z = y - s;
    return x + y + z;
}

int main(void)
{
    u64 a1 = 0x1122334455667788ULL;
    if (reuse8(-3, a1, 5u, 200) != a1 + (unsigned char)(a1 * 3u) - 3 + 5)
        return 1;
    u64 t = 0xABCDEF0123456789ULL + 1;
    unsigned b = (unsigned)(t >> 8);
    if (reuse32(0xABCDEF0123456789ULL, 99u, 7) != ((t ^ b) + b))
        return 2;
    int s = 4 * 7 + 9, x = s + 1, y = x * 2, z = y - s;
    if (reuse_all(4, 9, 1000) != x + y + z)
        return 3;
    return 42;
}
