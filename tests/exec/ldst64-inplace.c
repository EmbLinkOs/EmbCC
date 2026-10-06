/* 64-bit loads, stores and block copies whose addresses and values are
 * already in registers, on a 32-bit target where a long long or a double
 * is a register pair.
 *
 *   - `return *p` with p arriving in the register the result's low word
 *     leaves in: the high word must be read first, or the low word's load
 *     overwrites the pointer;
 *   - the same with the pointer in the high word's register;
 *   - stores of a pair to a pointer, at an index;
 *   - a struct copied through pointers, and one passed by value (RV32
 *     passes it by reference and the callee copies it into its frame);
 *   - long long add, subtract and sign extension with operands and
 *     results in register pairs;
 *   - every float and double comparison branched on, either way round,
 *     with a NaN making each ordered one false (on a soft-float target
 *     the branch tests the helper's result directly).
 *
 * Plain C: every value is checked against what C says it is. */
// expect-exit: 42
int printf(const char *fmt, ...);

struct blk { int w[7]; };

__attribute__((noinline)) static long long ld_first(long long *p) { return *p; }
__attribute__((noinline)) static long long ld_second(int k, long long *p)
{
    return *p + k;
}
__attribute__((noinline)) static double ld_idx(const double *p, int i) { return p[i]; }
__attribute__((noinline)) static void st_pair(long long *p, long long v) { *p = v; }
__attribute__((noinline)) static void st_idx(double *p, int i, double v) { p[i] = v; }
__attribute__((noinline)) static void cp_blk(struct blk *d, const struct blk *s) { *d = *s; }
__attribute__((noinline)) static int sum_blk(struct blk b)
{
    int s = 0;
    for (int i = 0; i < 7; i++)
        s += b.w[i] * (i + 1);
    b.w[0] = -1;                     /* the callee's own copy */
    return s + b.w[0];
}

__attribute__((noinline)) static long long add_ext(long long a, int k)
{
    return a + k;
}
__attribute__((noinline)) static long long sub_pair(long long a, long long b)
{
    return a - b;
}

/* Each comparison branched on: bit k of the result is set when the k-th
 * predicate's branch was taken. The negations are in functions of their
 * own, so that no comparison has a second reader (value numbering would
 * otherwise share `a < b` with its negation), and written as the else of
 * an empty then, so that each is a branch on the comparison itself. */
__attribute__((noinline)) static int dbr(double a, double b)
{
    int r = 0;
    if (a < b) r |= 1;
    if (a <= b) r |= 2;
    if (a > b) r |= 4;
    if (a >= b) r |= 8;
    if (a == b) r |= 16;
    if (a != b) r |= 32;
    return r;
}
__attribute__((noinline)) static int dnot(double a, double b)
{
    int r = 0;
    if (a < b) ; else r |= 64;       /* !(a < b), as a branch */
    if (a >= b) ; else r |= 128;
    return r;
}
__attribute__((noinline)) static int fbr(float a, float b)
{
    int r = 0;
    if (a < b) r |= 1;
    if (a <= b) r |= 2;
    if (a > b) r |= 4;
    if (a >= b) r |= 8;
    if (a == b) r |= 16;
    if (a != b) r |= 32;
    return r;
}
__attribute__((noinline)) static int fnot(float a, float b)
{
    int r = 0;
    if (a > b) ; else r |= 64;
    if (a <= b) ; else r |= 128;
    return r;
}

static volatile double vzero = 0.0;

int main(void)
{
    int bad = 0;
    long long a = 0x123456789abcdef0LL, b = -5;
    double d[4] = { 1.5, -2.25, 3.0, 1e300 };
    if (ld_first(&a) != 0x123456789abcdef0LL) { printf("ld_first\n"); bad++; }
    if (ld_second(3, &b) != -2) { printf("ld_second\n"); bad++; }
    if (ld_idx(d, 1) != -2.25 || ld_idx(d, 3) != 1e300) { printf("ld_idx\n"); bad++; }
    st_pair(&b, 0x7fffffff80000001LL);
    if (b != 0x7fffffff80000001LL) { printf("st_pair\n"); bad++; }
    st_idx(d, 2, -0.5);
    if (d[2] != -0.5 || d[1] != -2.25 || d[3] != 1e300) { printf("st_idx\n"); bad++; }
    struct blk x = { { 1, 2, 3, 4, 5, 6, 7 } }, y;
    cp_blk(&y, &x);
    for (int i = 0; i < 7; i++)
        if (y.w[i] != i + 1) { printf("cp_blk %d\n", i); bad++; }
    /* 1*1 + 2*2 + ... + 7*7 = 140, then -1 */
    if (sum_blk(x) != 139 || x.w[0] != 1) { printf("sum_blk\n"); bad++; }
    if (add_ext(0x00000000ffffffffLL, 1) != 0x0000000100000000LL) { printf("add carry\n"); bad++; }
    if (add_ext(5, -7) != -2) { printf("add ext\n"); bad++; }
    if (sub_pair(0x0000000100000000LL, 1) != 0x00000000ffffffffLL) { printf("sub borrow\n"); bad++; }
    if (sub_pair(-1, 0x7fffffffffffffffLL) != (long long)0x8000000000000000ULL) {
        printf("sub wide\n"); bad++;
    }
    double nan = vzero / vzero;
    /* less: < <= != !>= ; equal: <= >= == !< ; greater: > >= != !< */
    if ((dbr(1.0, 2.0) | dnot(1.0, 2.0)) != (1 | 2 | 32 | 128)) { printf("dbr lt\n"); bad++; }
    if ((dbr(2.0, 2.0) | dnot(2.0, 2.0)) != (2 | 8 | 16 | 64)) { printf("dbr eq\n"); bad++; }
    if ((dbr(3.0, 2.0) | dnot(3.0, 2.0)) != (4 | 8 | 32 | 64)) { printf("dbr gt\n"); bad++; }
    /* unordered: only != and the two negations */
    if ((dbr(nan, 2.0) | dnot(nan, 2.0)) != (32 | 64 | 128)) { printf("dbr nan\n"); bad++; }
    if ((fbr(1.0f, 2.0f) | fnot(1.0f, 2.0f)) != (1 | 2 | 32 | 64)) { printf("fbr lt\n"); bad++; }
    if ((fbr(2.0f, 2.0f) | fnot(2.0f, 2.0f)) != (2 | 8 | 16 | 64)) { printf("fbr eq\n"); bad++; }
    if ((fbr(3.0f, 2.0f) | fnot(3.0f, 2.0f)) != (4 | 8 | 32 | 128)) { printf("fbr gt\n"); bad++; }
    if ((fbr((float)nan, 2.0f) | fnot((float)nan, 2.0f)) != (32 | 64 | 128)) { printf("fbr nan\n"); bad++; }
    if (bad) { printf("ldst64-inplace: %d failures\n", bad); return 1; }
    return 42;
}
