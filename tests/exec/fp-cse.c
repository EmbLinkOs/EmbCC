/* Floating-point operations computed twice on the same operands, which
 * value numbering now computes once:
 *
 *   - `(x*y)/(x*y)`, fdlibm's way of raising an exception and returning a
 *     NaN, with x*y finite, infinite, zero and NaN -- reusing the product
 *     must not turn the quotient into 1, nor `p - p` into 0;
 *   - the same float comparison twice, once into a variable that is also
 *     written on another path, with a NaN making every ordered
 *     comparison false;
 *   - a SIGNED INTEGER add and a FLOATING add of the same two values (a
 *     float and its bits, through a union): the same operation code,
 *     width and signedness on the same operands, and two different
 *     results;
 *   - a product computed before a branch and again inside it.
 *
 * The functions are noinline and their inputs volatile, so nothing is
 * folded at compile time. Only C's arithmetic is asserted (a NaN is
 * recognised by x != x, never by its bits). */
// expect-exit: 42
int printf(const char *fmt, ...);

typedef __INT32_TYPE__ s32;
typedef union { float f; s32 i; } fpun;

static volatile double vd[] = { 3.0, 0.5, 1e300, 0.0, -2.0 };
static volatile float vf[] = { 1.5f, 2.25f, -0.0f };

__attribute__((noinline)) static double quot(double x, double y)
{
    return (x * y) / (x * y);
}

/* Two statements: C lets a compiler fuse `x * y - p` into one rounding
 * when it is one expression, and then inf - inf is not what happens. */
__attribute__((noinline)) static double diff(double x, double y)
{
    double p = x * y;
    double q = x * y;
    return q - p;
}

__attribute__((noinline)) static int cmps(double x, double y, int c)
{
    int q = x < y;
    int r;
    if (c)
        r = x < y;
    else
        r = 7;
    return r * 10 + q + (x < y) * 100;
}

__attribute__((noinline)) static int fcmps(float x, float y, int c)
{
    int q = x <= y;
    int r;
    if (c)
        r = x <= y;
    else
        r = 7;
    return r * 10 + q;
}

__attribute__((noinline)) static float fsum(float x, float y, s32 *bits)
{
    fpun a, b;
    a.f = x;
    b.f = y;
    *bits = a.i + b.i;
    return x + y;
}

__attribute__((noinline)) static double across(double x, double y, int c)
{
    double p = x * y;
    double r = 1.0;
    if (c)
        r = x * y + 1.0;
    return p + r;
}

static int isnan_d(double v) { return v != v; }

int main(void)
{
    int bad = 0;
    double three = vd[0], half = vd[1], big = vd[2], zero = vd[3], m2 = vd[4];
    double inf = big * big, nan = inf - inf;

    if (quot(three, half) != 1.0) { printf("quot finite\n"); bad++; }
    if (!isnan_d(quot(big, big))) { printf("quot inf/inf\n"); bad++; }
    if (!isnan_d(quot(zero, three))) { printf("quot 0/0\n"); bad++; }
    if (!isnan_d(quot(nan, three))) { printf("quot nan\n"); bad++; }
    if (diff(three, m2) != 0.0) { printf("diff finite\n"); bad++; }
    if (!isnan_d(diff(big, big))) { printf("diff inf-inf\n"); bad++; }

    if (cmps(half, three, 1) != 111) { printf("cmps lt\n"); bad++; }
    if (cmps(three, half, 1) != 0) { printf("cmps ge\n"); bad++; }
    if (cmps(half, three, 0) != 171) { printf("cmps other path\n"); bad++; }
    if (cmps(nan, three, 1) != 0) { printf("cmps nan\n"); bad++; }
    if (cmps(three, nan, 0) != 70) { printf("cmps nan other path\n"); bad++; }

    float f15 = vf[0], f225 = vf[1], fm0 = vf[2];
    if (fcmps(f15, f225, 1) != 11) { printf("fcmps le\n"); bad++; }
    if (fcmps(f225, f15, 1) != 0) { printf("fcmps gt\n"); bad++; }
    if (fcmps(fm0, -fm0, 1) != 11) { printf("fcmps -0 <= 0\n"); bad++; }
    if (fcmps(f225, f15, 0) != 70) { printf("fcmps other path\n"); bad++; }

    s32 bits;
    float s = fsum(f15, f225, &bits);
    /* 1.5f is 0x3fc00000 and 2.25f is 0x40100000: no overflow */
    if (s != 3.75f) { printf("fsum float\n"); bad++; }
    if (bits != (s32)0x3fc00000 + (s32)0x40100000) { printf("fsum bits\n"); bad++; }

    if (across(three, half, 1) != 4.0) { printf("across taken\n"); bad++; }
    if (across(three, half, 0) != 2.5) { printf("across not taken\n"); bad++; }

    if (bad) { printf("fp-cse: %d failures\n", bad); return 1; }
    return 42;
}
