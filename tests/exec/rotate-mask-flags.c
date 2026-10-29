// expect-exit: 42
/* Three Cortex-M fusions (src/arch/thumb/codegen.c), each against a
 * reference computed another way:
 *   - `x << k | x >> (32 - k)` as one ror, and as the shifted operand of
 *     the eor, add, sub, rsb, and, orr that reads it -- every k, both
 *     shifts first, the or written as | ^ and +;
 *   - `(x & m) ? a : b` as tst and an IT block, the mask an immediate a
 *     tst takes, one it cannot, and a register;
 *   - a branch on the result of an adds/subs, taken from its flags. */

#define NOINLINE __attribute__((noinline))

typedef unsigned u32;
typedef unsigned long long u64;

static u32 rot_ref(u32 x, int k)    /* left, by halves of a 64-bit shift */
{
    u64 w = ((u64)x << 32 | x) << k;
    return (u32)(w >> 32);
}

#define ROT(k)                                                          \
NOINLINE static u32 rl##k(u32 x) { return x << k | x >> (32 - k); }     \
NOINLINE static u32 rr##k(u32 x) { return x >> (32 - k) ^ x << k; }     \
NOINLINE static u32 ra##k(u32 x) { return (x << k) + (x >> (32 - k)); } \
NOINLINE static u32 xe##k(u32 x, u32 y) { return y ^ (x << k | x >> (32 - k)); } \
NOINLINE static u32 xa##k(u32 x, u32 y) { return (x >> (32 - k) | x << k) + y; } \
NOINLINE static u32 xs##k(u32 x, u32 y) { return y - (x << k | x >> (32 - k)); } \
NOINLINE static u32 xr##k(u32 x, u32 y) { return (x << k | x >> (32 - k)) - y; } \
NOINLINE static u32 xn##k(u32 x, u32 y) { return y & (x << k | x >> (32 - k)); } \
NOINLINE static u32 xo##k(u32 x, u32 y) { return (x << k | x >> (32 - k)) | y; }

ROT(1) ROT(3) ROT(7) ROT(8) ROT(13) ROT(16) ROT(19) ROT(24) ROT(29) ROT(31)

typedef u32 (*f1)(u32);
typedef u32 (*f2)(u32, u32);
static const struct { int k; f1 l, r, a; f2 e, d, s, rs, n, o; } rots[] = {
#define R(k) { k, rl##k, rr##k, ra##k, xe##k, xa##k, xs##k, xr##k, xn##k, xo##k }
    R(1), R(3), R(7), R(8), R(13), R(16), R(19), R(24), R(29), R(31),
};

NOINLINE static int sel_imm(u32 x, int a, int b) { return (x & 31) ? a : b; }
NOINLINE static int sel_bit(u32 x, int a, int b) { return (x & 0x100) ? a : b; }
NOINLINE static int sel_odd(u32 x, int a, int b)    /* 0x12345 is no tst #imm */
{
    return (x & 0x12345) ? a : b;
}
NOINLINE static int sel_reg(u32 x, u32 m, int a, int b) { return (x & m) ? a : b; }
NOINLINE static int sel_zero(u32 x, int a, int b) { return (x & 0xff) == 0 ? a : b; }

/* the counted-down loop: `subs; bne` */
NOINLINE static int count_down(int *p, int n)
{
    int s = 0;
    while (--n)
        s += p[n];
    return s;
}
/* an add that reaches zero, the branch on its flags, and on a value the
 * flags are not from */
NOINLINE static int step_to_zero(int x, int y)
{
    int k = 0;
    while (x != 0) {
        x += 3;
        if (y + 1 == 0)
            k += 100;
        k++;
    }
    return k;
}

static const u32 probe[] = { 0, 1, 2, 0x80000000u, 0x7fffffffu, 0xffffffffu,
                             0x12345678u, 0xdeadbeefu, 0x00ff00ffu, 0x31u };

int main(void)
{
    for (unsigned r = 0; r < sizeof rots / sizeof rots[0]; r++)
        for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
            u32 x = probe[i], y = probe[(i + 3) % 10], w = rot_ref(x, rots[r].k);
            if (rots[r].l(x) != w || rots[r].r(x) != w || rots[r].a(x) != w)
                return 1;
            if (rots[r].e(x, y) != (y ^ w) || rots[r].d(x, y) != w + y ||
                rots[r].s(x, y) != y - w || rots[r].rs(x, y) != w - y ||
                rots[r].n(x, y) != (y & w) || rots[r].o(x, y) != (w | y))
                return 2;
        }
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        u32 x = probe[i];
        if (sel_imm(x, 5, 9) != ((x % 32) ? 5 : 9)) return 3;
        if (sel_bit(x, 5, 9) != ((x >> 8 & 1) ? 5 : 9)) return 4;
        if (sel_odd(x, 5, 9) != ((x & 0x12345) != 0 ? 5 : 9)) return 5;
        if (sel_reg(x, probe[(i + 1) % 10], 5, 9) !=
            ((x & probe[(i + 1) % 10]) != 0 ? 5 : 9)) return 6;
        if (sel_zero(x, 5, 9) != ((x % 256) ? 9 : 5)) return 7;
    }
    int a[20];
    for (int i = 0; i < 20; i++)
        a[i] = i * i;
    for (int n = 1; n <= 20; n++) {
        int want = 0;
        for (int i = 1; i < n; i++)
            want += i * i;
        if (count_down(a, n) != want) return 8;
    }
    if (step_to_zero(-30, 5) != 10 || step_to_zero(-3, -1) != 101 ||
        step_to_zero(0, 0) != 0)
        return 9;
    return 42;
}
