/* Soft-float additions and multiplications whose helper call takes its
 * operands in either order (Cortex-M without an FPU: the setup that needs
 * fewer moves -- codegen.c's fp_swap_args), with more values live across
 * calls than there are registers, so some operands come from memory and
 * some from registers on each side. Every value is a small integer, every
 * sum and product exact, so the answer is checked exactly; and a - b,
 * which must not be swapped, runs beside them. */
// expect-exit: 42
static volatile int salt;

__attribute__((noinline)) float f1(float x) { return x + (float)salt; }
__attribute__((noinline)) double d1(double x) { return x + (double)salt; }

__attribute__((noinline)) float fmix(int k)
{
    float a = f1((float)k), b = f1(2.0f), c = f1(3.0f), d = f1(5.0f);
    float e = f1(7.0f), g = f1(11.0f), h = f1(13.0f), i = f1(17.0f);
    float j = f1(19.0f), m = f1(23.0f), n = f1(29.0f), o = f1(31.0f);
    float r = a * b + c;
    r = f1(r) - d * e;
    r = g + r * h;
    r = f1(r) + i * (j - m);
    r = (n - o) * r + a;
    return r + b + c + d + e + g + h + i + j + m + n + o;
}

__attribute__((noinline)) double dmix(int k)
{
    double a = d1((double)k), b = d1(2.0), c = d1(3.0), d = d1(5.0);
    double e = d1(7.0), g = d1(11.0), h = d1(13.0), i = d1(17.0);
    double r = a * b + c;
    r = d1(r) - d * e;
    r = g + r * h;
    r = d1(r) + i * (a - b);
    r = (c - d) * r + e;
    return r + a + b + c + d + e + g + h + i;
}

static long fref(long k)
{
    long a = k, b = 2, c = 3, d = 5, e = 7, g = 11, h = 13, i = 17;
    long j = 19, m = 23, n = 29, o = 31;
    long r = a * b + c;
    r = r - d * e;
    r = g + r * h;
    r = r + i * (j - m);
    r = (n - o) * r + a;
    return r + b + c + d + e + g + h + i + j + m + n + o;
}

static long dref(long k)
{
    long a = k, b = 2, c = 3, d = 5, e = 7, g = 11, h = 13, i = 17;
    long r = a * b + c;
    r = r - d * e;
    r = g + r * h;
    r = r + i * (a - b);
    r = (c - d) * r + e;
    return r + a + b + c + d + e + g + h + i;
}

int main(void)
{
    for (int k = -3; k <= 9; k++) {
        if (fmix(k) != (float)fref(k)) return 1;
        if (dmix(k) != (double)dref(k)) return 2;
    }
    return 42;
}
