/* setjmp and longjmp, as each target's lib/libc implements them: what
 * comes back, which registers survive, and that nothing is left behind.
 *
 * - setjmp returns 0, then the value longjmp passes, and 1 for 0.
 * - Locals not changed since the setjmp keep their values: twelve ints,
 *   four floats and four doubles live across it, so they are in
 *   callee-saved registers -- the integer ones and, on a part with an
 *   FPU, the floating-point ones (a float where the FPU is single-
 *   precision) -- and the jump comes from a function that uses every
 *   register it can.
 * - A volatile local changed after the setjmp keeps the new value.
 * - longjmp from 60 calls down: SPARC's windows and TriCore's context
 *   areas between there and the setjmp are given back.
 * - 3,000 jumps in a row: a stack pointer or context area not restored
 *   shows up as a stack that only grows.
 * - A jump out of a nested setjmp to the outer one.
 *
 * setjmp is called only where C allows it (7.13.1.1p4): as a switch's or
 * an if's controlling expression or one side of `==`. */
// expect-exit: 42
#include <setjmp.h>
#include <stddef.h>

static jmp_buf outer, inner;
static volatile int sink;

/* As many live values as the machine has registers, and then a jump. */
__attribute__((noinline)) static void clobber_and_jump(jmp_buf env, int v,
                                                       int seed)
{
    volatile int s = seed;
    int a = s + 1, b = s + 2, c = s + 3, d = s + 4, e = s + 5, f = s + 6;
    int g = s + 7, h = s + 8, i = s + 9, j = s + 10, k = s + 11, l = s + 12;
    double x = s * 0.5, y = s * 0.25, z = s * 0.125, w = s * 2.0;
    float fa = s * 0.5f, fb = s * 0.25f, fc = s * 0.125f, fd = s * 2.0f;
    float fe = s * 3.0f, ff = s * 5.0f, fg = s * 7.0f, fh = s * 9.0f;
    for (int n = 0; n < 3; n++) {
        a += b * c; b += c * d; c += d * e; d += e * f; e += f * g;
        f += g * h; g += h * i; h += i * j; i += j * k; j += k * l;
        k += l * a; l += a * b;
        x = x * y + z; y = y * z + w; z = z * w + x; w = w * x + y;
        fa = fa * fb + fc; fb = fb * fc + fd; fc = fc * fd + fe;
        fd = fd * fe + ff; fe = fe * ff + fg; ff = ff * fg + fh;
        fg = fg * fh + fa; fh = fh * fa + fb;
    }
    sink = a + b + c + d + e + f + g + h + i + j + k + l +
           (int)(x + y + z + w) +
           (int)(fa + fb + fc + fd + fe + ff + fg + fh);
    longjmp(env, v);
}

__attribute__((noinline)) static void deep(jmp_buf env, int n)
{
    volatile int pad[4];
    pad[0] = n;
    if (n == 0)
        longjmp(env, 60);
    deep(env, n - 1);
    sink = pad[0];
}

static int values_survive(int p)
{
    int a = p + 1, b = p + 2, c = p + 3, d = p + 4, e = p + 5, f = p + 6;
    int g = p + 7, h = p + 8, i = p + 9, j = p + 10, k = p + 11, l = p + 12;
    double x = p + 0.5, y = p + 1.5, z = p + 2.5, w = p + 3.5;
    float fx = p + 0.25f, fy = p + 0.5f, fz = p + 0.75f, fw = p + 1.0f;
    switch (setjmp(outer)) {
    case 0:
        clobber_and_jump(outer, 7, p * 3);
        return 1;                       /* longjmp returned */
    case 7:
        break;
    default:
        return 2;
    }
    if (a + b + c + d + e + f + g + h + i + j + k + l != 12 * p + 78)
        return 3;
    if (x + y + z + w != 4 * p + 8.0)
        return 4;
    if (fx + fy + fz + fw != 4 * p + 2.5f)
        return 12;
    return 0;
}

/* longjmp(env, 0) comes back as 1 -- asked of lib/libc's longjmp, found
 * by a weak reference to a function each of its backends defines: the
 * QEMU harness for x86-64 links newlib, whose longjmp returns 0. (Apple's
 * linker refuses a weak reference nothing defines; its libSystem returns
 * 1, so there it is always asked.) */
#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
#endif

static int zero_is_one(void)
{
#if !defined(__APPLE__)
    if (!&__os_write)
        return 0;
#endif
    static volatile int jumped;
    jumped = 0;
    switch (setjmp(outer)) {
    case 0:
        if (jumped)
            return 13;                  /* came back as 0 */
        jumped = 1;
        longjmp(outer, 0);
    case 1:
        return 0;
    default:
        return 5;
    }
}

static int volatile_kept(void)
{
    volatile int v = 1;
    if (setjmp(outer) == 0) {
        v = 42;
        clobber_and_jump(outer, 1, 9);
    }
    return v == 42 ? 0 : 6;
}

static int from_deep(void)
{
    switch (setjmp(outer)) {
    case 0:
        deep(outer, 60);
        return 7;
    case 60:
        return 0;
    default:
        return 8;
    }
}

static int many(void)
{
    static volatile int count;
    count = 0;
    if (setjmp(outer) != 0) {
        if (++count == 3000)
            return 0;
    }
    if (count < 3000)
        deep(outer, count % 5);
    return 9;
}

__attribute__((noinline)) static int nested_inner(void)
{
    if (setjmp(inner) == 0)
        longjmp(outer, 3);              /* past the inner one */
    return 10;
}

static int nested(void)
{
    switch (setjmp(outer)) {
    case 0:
        return nested_inner() + 100;
    case 3:
        return 0;
    default:
        return 11;
    }
}

int main(void)
{
    int r;
    if ((r = values_survive(5)) != 0) return r;
    if ((r = values_survive(-20)) != 0) return r;
    if ((r = zero_is_one()) != 0) return r;
    if ((r = volatile_kept()) != 0) return r;
    if ((r = from_deep()) != 0) return r;
    if ((r = many()) != 0) return r;
    if ((r = nested()) != 0) return r;
    return 42;
}
