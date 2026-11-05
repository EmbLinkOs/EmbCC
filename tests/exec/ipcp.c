/* A parameter every call passes one constant, which -O3 makes that
 * constant inside the function (src/opt/ipcp.c), and the cases it must
 * leave alone: the constants differ between calls, the function's
 * address escapes and an indirect call passes something else, a call
 * passes a variable. Each answer is checked against its value computed
 * by hand, so a constant propagated where it should not have been gives
 * the wrong one. */
// expect-exit: 42
#define NI __attribute__((noinline))
static volatile int vz;

/* n is 16 at both calls: the division becomes a mask, the loop's count
 * is known */
NI static int wrap_sum(int n, int start)
{
    int s = 0;
    for (int i = 0; i < 40; i++)
        s += (start + i) % n;
    return s;
}

/* two parameters, one constant (the step) and one not */
NI static long scaled(long x, int shift)
{
    return (x << shift) + (x >> (shift - 1));
}

/* the constants differ: 3 and 5 */
NI static int differ(int k, int x) { return x * k + k; }

/* its address escapes: called directly with 7, and through a pointer
 * with 9 */
NI static int escapes(int k, int x) { return x - k; }
static int (*volatile fp)(int, int) = escapes;

/* a pointer parameter, always the same table */
static const unsigned char tab[8] = { 3, 1, 4, 1, 5, 9, 2, 6 };
NI static int lookup(const unsigned char *t, int i) { return t[i & 7]; }

/* a narrow parameter: a char given 200 */
NI static int narrow(unsigned char c, int x) { return c + x; }

/* recursive, the constant passed down by every call */
NI static int depth(int lim, int d) { return d >= lim ? d : depth(lim, d + 1) + 1; }

int main(void)
{
    int bad = 0;
    int want = 0;
    for (int i = 0; i < 40; i++) want += (1 + i) % 16;
    bad |= wrap_sum(16, 1 + vz) != want;
    want = 0;
    for (int i = 0; i < 40; i++) want += (-30 + i) % 16;   /* negative too */
    bad |= (wrap_sum(16, -30 + vz) != want) << 1;
    bad |= (scaled(1000 + vz, 3) != 8000 + 250) << 2;
    bad |= (scaled(64 + vz, 3) != 512 + 16) << 2;
    bad |= (differ(3, 10 + vz) != 33) << 3;
    bad |= (differ(5, 10 + vz) != 55) << 3;
    bad |= (escapes(7, 10 + vz) != 3) << 4;
    bad |= (fp(9, 10) != 1) << 4;
    bad |= (lookup(tab, 5 + vz) != 9) << 5;
    bad |= (lookup(tab, 10 + vz) != 4) << 5;
    bad |= (narrow(200, 1 + vz) != 201) << 6;
    bad |= (narrow(200, 100 + vz) != 300) << 6;
    bad |= (depth(6, vz) != 12) << 7;
    return bad ? 1 : 42;
}
