// expect-exit: 42
/* A mask tested by a branch -- `if (x & FLAG)`, `(i & 31) == 0` -- which
 * the Cortex-M backend makes `tst` and a conditional branch: masks a
 * modified immediate holds and ones it does not (0x12345, the sign bit),
 * a mask in a register, both senses, copies between the mask and the
 * branch (a loop's join), a mask whose value is used again, and the same
 * tests with more live values than registers. Every answer is counted
 * against a table worked out by hand. */

#define NI __attribute__((noinline))

NI static int bit2(int x) { if (x & 4) return 1; return 0; }
NI static int nobit8(int x) { if (!(x & 0x100)) return 1; return 0; }
NI static int odd_mask(int x) { if (x & 0x12345) return 1; return 0; }
NI static int sign(int x) { if (x & (int)0x80000000u) return 1; return 0; }
NI static int regmask(int x, int m) { if (x & m) return 1; return 0; }
NI static int regmask_not(unsigned x, unsigned m)
{
    if ((x & m) == 0) return 1;
    return 0;
}

/* The join's copies sit between the mask and its branch. */
NI static int every32(const int *p, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if ((i & 31) == 0)
            c += p[i];
    return c;
}

/* The mask's value is read again: no tst may drop it. */
NI static int kept(int x)
{
    int m = x & 6;
    if (m) return m * 10;
    return 1;
}

/* Flag-setting arithmetic between the mask and its branch. */
NI static int between(int x, int y)
{
    int m = x & 4;
    y = y * 3 + 1;
    if (m) return y;
    return -y;
}
NI static int between2(int x, int y, int *p)
{
    int m = x & 4;
    int t = y - 5;
    p[0] = t;
    if (m) return t;
    return -1;
}

/* Twenty values live across the tests. */
NI static int pressure(volatile int *v, int x)
{
    int a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3], a4 = v[4];
    int a5 = v[5], a6 = v[6], a7 = v[7], a8 = v[8], a9 = v[9];
    int b0 = v[10], b1 = v[11], b2 = v[12], b3 = v[13], b4 = v[14];
    int b5 = v[15], b6 = v[16], b7 = v[17], b8 = v[18], b9 = v[19];
    int s = 0;
    if (x & a3) s += 1;          /* a mask in a register: a3 = 4 */
    if (!(x & 0x40)) s += 2;
    if (x & 0x12345) s += 4;
    return s * 1000 + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 +
           b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 + b9;
}

int main(void)
{
    int bad = 0;
    if (bit2(4) != 1 || bit2(3) != 0 || bit2(-1) != 1 || bit2(0) != 0)
        bad |= 1;
    if (nobit8(0x100) != 0 || nobit8(0xff) != 1 || nobit8(-1) != 0)
        bad |= 2;
    if (odd_mask(0x10000) != 1 || odd_mask(0x8000) != 0 ||
        odd_mask(1) != 1 || odd_mask(0x40000) != 0) bad |= 4;
    if (sign(-5) != 1 || sign(5) != 0 || sign(0x7fffffff) != 0) bad |= 8;
    if (regmask(6, 4) != 1 || regmask(6, 1) != 0 || regmask(0, -1) != 0)
        bad |= 16;
    if (regmask_not(0xf0u, 0x0fu) != 1 || regmask_not(0xf0u, 0x10u) != 0)
        bad |= 32;

    int arr[100];
    for (int i = 0; i < 100; i++)
        arr[i] = i * 3 + 1;
    /* i = 0, 32, 64, 96: 1 + 97 + 193 + 289 */
    if (every32(arr, 100) != 580 || every32(arr, 32) != 1) bad |= 64;

    /* x & 6 != x, so a mask left uncomputed shows */
    if (kept(7) != 60 || kept(3) != 20 || kept(9) != 1)
        bad |= 128;
    int cell = 0;
    if (between(4, 5) != 16 || between(3, 5) != -16 ||
        between(-1, -1) != -2 || between(0, 0) != -1) bad |= 2048;
    if (between2(4, 9, &cell) != 4 || cell != 4 ||
        between2(1, 2, &cell) != -1 || cell != -3) bad |= 4096;

    volatile int v[20];
    for (int i = 0; i < 20; i++)
        v[i] = i + 1;                 /* sum 210 */
    /* x = 4: bit 2 set, 0x40 clear, 4 & 0x12345 = 4: 1 + 2 + 4 */
    if (pressure(v, 4) != 7 * 1000 + 210) bad |= 256;
    /* x = 0x40: 0x40 & 4 = 0, 0x40 set, 0x40 & 0x12345 = 0x40 */
    if (pressure(v, 0x40) != 4 * 1000 + 210) bad |= 512;
    /* x = 8: only 0x40 clear */
    if (pressure(v, 0x8) != 2 * 1000 + 210) bad |= 1024;

    return bad ? 1 + bad % 41 : 42;      /* 1..41: never 42 */
}
