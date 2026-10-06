// expect-exit: 42
/* The conditional shapes if-conversion turns into selects -- an IT block
 * on ARMv7-M, cmov/csel on x86-64 and AArch64: both arms constants, one
 * arm only (`if (c) x = v;`), signed and unsigned and 64-bit conditions,
 * a select whose result is one of its own operands, and selects in a
 * function with more live values than registers, where an operand comes
 * from the stack between the compare and the select. Each answer is
 * checked against a table worked out by hand, not against another path
 * through the same compiler. */

__attribute__((noinline)) static int st(unsigned len) { return len ? 2 : 0; }

__attribute__((noinline)) static int sat16(int y)
{
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    return y;
}

__attribute__((noinline)) static unsigned umax(unsigned a, unsigned b)
{
    return a > b ? a : b;
}

__attribute__((noinline)) static int pick(int c, int a, int b)
{
    return c ? a : b;
}

__attribute__((noinline)) static int pick_le(int x, int y, int a, int b)
{
    int r = b;
    if (x <= y) r = a;
    return r;
}

__attribute__((noinline)) static int wide_cond(long long x, int a, int b)
{
    return x ? a : b;
}

__attribute__((noinline)) static int wide_cmp(long long x, long long y)
{
    return x < y ? 7 : -7;
}

__attribute__((noinline)) static unsigned ucmp_pick(unsigned x, unsigned y)
{
    unsigned r = 100;
    if (x >= y) r = 200;
    return r;
}

/* Constants as the arms: an IT block moves each as an immediate when one
 * instruction can -- 0..255, a modified immediate (0x12000000,
 * 0xff00ff00), its complement (-256) -- and from a register when not
 * (0x12345678). */
__attribute__((noinline)) static unsigned imm_arms(int c, int k)
{
    switch (k) {
    case 0: return c ? 0x12000000u : 0xff00ff00u;
    case 1: return c > 0 ? (unsigned)-256 : 255u;
    case 2: return c < 0 ? 0x12345678u : 0u;
    default: return c ? 0u : 0xab00ab00u;
    }
}

/* One constant, two readers: value numbering gives the store and the
 * select one temp, which the select may not take as its own immediate
 * and leave the store without. */
static volatile int vsink;
__attribute__((noinline)) static int shared_k(int c, int x)
{
    vsink = 77;
    return c ? 77 : x;
}

/* Twenty values live across the selects: some operands are spilled. */
__attribute__((noinline)) static int pressure(volatile int *v, int c)
{
    int a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3], a4 = v[4];
    int a5 = v[5], a6 = v[6], a7 = v[7], a8 = v[8], a9 = v[9];
    int b0 = v[10], b1 = v[11], b2 = v[12], b3 = v[13], b4 = v[14];
    int b5 = v[15], b6 = v[16], b7 = v[17], b8 = v[18], b9 = v[19];
    int s = c > 3 ? a0 : b9;
    int t = c < 3 ? b0 : a9;
    if (c == 3) s = a5;
    if (c != 3) t = b5;
    return s * 3 + t + a1 + a2 + a3 + a4 + a6 + a7 + a8 +
           b1 + b2 + b3 + b4 + b6 + b7 + b8;
}

/* A loop that carries the selected value: the select's result is one of
 * its operands on the next trip. */
__attribute__((noinline)) static int runmax(const int *p, int n)
{
    int m = p[0];
    for (int i = 1; i < n; i++)
        if (p[i] > m) m = p[i];
    return m;
}

/* A tiny state machine, the shape the bench's `state` kernel has. */
__attribute__((noinline)) static int machine(const unsigned char *s, int n)
{
    int st = 0, count = 0;
    for (int i = 0; i < n; i++) {
        unsigned c = s[i];
        int letter = (c | 32) - 'a' < 26u;
        int nst = letter ? 1 : 0;
        if (st == 0 && nst == 1) count++;
        st = nst;
    }
    return count;
}

int main(void)
{
    int bad = 0;
    if (st(0) != 0 || st(1) != 2 || st(0x80000000u) != 2) bad |= 1;
    if (sat16(40000) != 32767 || sat16(-40000) != -32768 ||
        sat16(123) != 123 || sat16(32767) != 32767 ||
        sat16(-32768) != -32768 || sat16(32768) != 32767) bad |= 2;
    if (umax(3u, 0xfffffff0u) != 0xfffffff0u || umax(9u, 2u) != 9u)
        bad |= 4;
    if (pick(0, 5, 6) != 6 || pick(-1, 5, 6) != 5) bad |= 8;
    if (pick_le(1, 1, 10, 20) != 10 || pick_le(2, 1, 10, 20) != 20 ||
        pick_le(-5, 1, 10, 20) != 10) bad |= 16;
    if (wide_cond(0x100000000ll, 1, 2) != 1 || wide_cond(0, 1, 2) != 2)
        bad |= 32;
    if (wide_cmp(-1, 0) != 7 || wide_cmp(0x100000000ll, 1) != -7 ||
        wide_cmp(1, 0x100000000ll) != 7) bad |= 64;
    if (ucmp_pick(5, 5) != 200 || ucmp_pick(4, 5) != 100 ||
        ucmp_pick(0xffffffffu, 1) != 200) bad |= 128;

    if (imm_arms(1, 0) != 0x12000000u || imm_arms(0, 0) != 0xff00ff00u ||
        imm_arms(5, 1) != 0xffffff00u || imm_arms(-5, 1) != 255u ||
        imm_arms(-1, 2) != 0x12345678u || imm_arms(1, 2) != 0u ||
        imm_arms(0, 3) != 0xab00ab00u || imm_arms(9, 3) != 0u)
        bad |= 8192;

    vsink = 0;
    if (shared_k(0, 5) != 5 || vsink != 77) bad |= 16384;
    vsink = 0;
    if (shared_k(1, 5) != 77 || vsink != 77) bad |= 16384;

    volatile int v[20];
    for (int i = 0; i < 20; i++)
        v[i] = i + 1;
    /* the sum of the fixed terms: 2+3+4+5+7+8+9 + 12+13+14+15+17+18+19 */
    int fixed = 38 + 108;
    if (pressure(v, 5) != 1 * 3 + 16 + fixed) bad |= 256;   /* s=a0 t=b5 */
    if (pressure(v, 1) != 20 * 3 + 16 + fixed) bad |= 512;  /* s=b9 t=b5 */
    if (pressure(v, 3) != 6 * 3 + 10 + fixed) bad |= 1024;  /* s=a5 t=a9 */

    int arr[7] = { 3, -9, 14, 2, 14, -100, 11 };
    if (runmax(arr, 7) != 14 || runmax(arr, 2) != 3) bad |= 2048;

    static const unsigned char text[] = "ab  cd9e,,Fg h";
    if (machine(text, (int)sizeof text - 1) != 5) bad |= 4096;

    return bad ? 1 + bad % 41 : 42;      /* 1..41: never 42 */
}
