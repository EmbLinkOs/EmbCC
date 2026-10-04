/* A 64-bit loop variable read as its low 32 bits, then copied whole in the
 * same block: the copy into the variable an if/else joins in. On x86-64 the
 * narrow read loaded the stack slot with `movl`, which leaves RAX holding
 * only the low half, and the residency cache let the 64-bit copy reuse RAX:
 * the variable came out truncated to 32 bits. The `(double)v` keeps v in its
 * slot there, which is what puts the reads through RAX at all. Found by the
 * gen2 fuzzer (seed 10379). */
// expect-exit: 42
typedef long long i64;
typedef unsigned int u32;
static volatile int vz;
static i64 g[4];
static double seen, after;

__attribute__((noinline)) static i64 f(i64 v, int c, int n)
{
    for (int i = 0; i < n; i++)
        if (g[i & 3] == 7)
            v = 3;
        else
            v = 1961431461522748001LL;
    seen = (double)v;
    if (c > 0) {
        if (v + g[1] < g[c & 3])
            g[2] = 5;
        else if ((u32)(!(u32)v) >= (u32)c)
            v = 104;
    }
    after = (double)v;
    return v;
}

int main(void)
{
    if (f(2 + vz, 100 + vz, 3 + vz) != 1961431461522748001LL ||
        after != (double)1961431461522748001LL)
        return 1;
    if (f(2 + vz, 100 + vz, 0 + vz) != 2)
        return 2;
    /* the low half is zero, so !(u32)v is 1 and equals c */
    if (f(0x500000000LL + vz, 1 + vz, 0 + vz) != 104)
        return 3;
    if (seen != (double)0x500000000LL)
        return 4;
    return 42;
}
