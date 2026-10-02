/* The GCC bit builtins at the widths a 32-bit or 8-bit target gives
 * them: `int` is two bytes on AVR, `long` four on every 32-bit target,
 * `long long` eight everywhere. Each builtin against the plain loop that
 * defines it, sized by sizeof -- so the same text is right on every
 * target, and prints `ok <count>` only if every answer was.
 *
 * tests/exec/bit-builtins.c covers the same family but assumes an LP64
 * `long`, so it runs only on the 64-bit hosts -- which is how the l forms
 * reading eight bytes of a four-byte long, the ll forms truncating to
 * four, and clz of a 16-bit int counting 32 bits all went unseen.
 *
 * BITOPS_NO_LL leaves the long long forms out, for AVR: each is lowered
 * inline, and at eight bytes, byte at a time, the lot does not fit the
 * part's 32 KB of flash. The 32-bit targets cover them. */
typedef unsigned long long u64;
void writec(int c);
void puts_(const char *s);

static int rpop(u64 x) { int n = 0; while (x) { n += (int)(x & 1); x >>= 1; } return n; }
static int rctz(u64 x, int b) { int n = 0; while (n < b && !(x >> n & 1)) n++; return n; }
static int rclz(u64 x, int b) { int n = 0; while (n < b && !(x >> (b - 1 - n) & 1)) n++; return n; }
static int rclrsb(u64 x, int b)
{
    int top = (int)(x >> (b - 1) & 1), n = 0;
    while (n + 1 < b && (int)(x >> (b - 2 - n) & 1) == top) n++;
    return n;
}

static int bad[18];

static void one(u64 v)
{
    unsigned u = (unsigned)v;
    unsigned long l = (unsigned long)v;
    u64 q = v;
    int bu = (int)sizeof u * 8, bl = (int)sizeof l * 8;

    bad[0] += __builtin_popcount(u) != rpop(u);
    bad[1] += __builtin_popcountl(l) != rpop(l);
#ifndef BITOPS_NO_LL
    bad[2] += __builtin_popcountll(q) != rpop(q);
#endif
    bad[3] += __builtin_parity(u) != (rpop(u) & 1);
    bad[4] += __builtin_parityl(l) != (rpop(l) & 1);
#ifndef BITOPS_NO_LL
    bad[5] += __builtin_parityll(q) != (rpop(q) & 1);
#endif
    bad[6] += __builtin_ffs((int)u) != (u ? rctz(u, bu) + 1 : 0);
    bad[7] += __builtin_ffsl((long)l) != (l ? rctz(l, bl) + 1 : 0);
#ifndef BITOPS_NO_LL
    bad[8] += __builtin_ffsll((long long)q) != (q ? rctz(q, 64) + 1 : 0);
#endif
    bad[9] += __builtin_clrsb((int)u) != rclrsb(u, bu);
    bad[10] += __builtin_clrsbl((long)l) != rclrsb(l, bl);
#ifndef BITOPS_NO_LL
    bad[11] += __builtin_clrsbll((long long)q) != rclrsb(q, 64);
#endif
    if (u) {       /* ctz and clz of 0 are undefined */
        bad[12] += __builtin_ctz(u) != rctz(u, bu);
        bad[13] += __builtin_clz(u) != rclz(u, bu);
    }
    if (l) {
        bad[14] += __builtin_ctzl(l) != rctz(l, bl);
        bad[15] += __builtin_clzl(l) != rclz(l, bl);
    }
#ifndef BITOPS_NO_LL
    if (q) {
        bad[16] += __builtin_ctzll(q) != rctz(q, 64);
        bad[17] += __builtin_clzll(q) != rclz(q, 64);
    }
#endif
    (void)q;
}

static void num(int n)
{
    char b[8];
    int k = 0;
    do { b[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) writec(b[--k]);
}

static const u64 edge[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, 0x100000000ULL,
    0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL,
    0xffff0000ffff0000ULL, 0x00ff00ff00ff00ffULL, 0x8000000000008000ULL,
};

int main(void)
{
    int n = 0;
    u64 x = 0x9e3779b97f4a7c15ULL;
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++, n++)
        one(edge[i]);
    for (int i = 0; i < 60; i++, n += 2) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        one(x);
        one(x >> (i % 64));
    }
    for (int k = 0; k < 18; k++)
        if (bad[k]) {
            puts_("BAD ");
            num(k);
            writec(' ');
            num(bad[k]);
            writec(' ');
        }
    puts_("ok ");
    num(n);
    puts_(" DONE\n");
    return 0;
}
