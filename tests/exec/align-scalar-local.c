/* Scalar locals aligned beyond what the stack guarantees -- `_Alignas(64)
 * int x;`, as DMA descriptors and cache-line counters are written. The
 * stack pointer is only 16-aligned (8 on AAPCS32), so the slot cannot be
 * rounded there; such a local gets storage of its own, carved and rounded
 * at the function's entry, and every read and write of the scalar goes to
 * it. This was refused by name ("supported for an array or a struct, not
 * yet for a scalar"). Each frame of a recursion must get its own copy, a
 * value must survive calls and loops, and the address must be aligned.
 */
// expect-exit: 42
#include <stdint.h>

static int aligned_to(const void *p, uintptr_t a) { return ((uintptr_t)p & (a - 1)) == 0; }

static void bump(int *p) { *p += 1; }

/* each frame its own x, holding its own value across the inner call */
static int depth(int n)
{
    _Alignas(64) int x = n * 10;
    if (!aligned_to(&x, 64))
        return -1000;
    if (n > 0) {
        int r = depth(n - 1);
        if (r < 0)
            return r;
        x += r;
    }
    return x;                       /* 30 + 20 + 10 + 0 = 60 for n = 3 */
}

int main(void)
{
    _Alignas(64) int a = 5;
    __attribute__((aligned(32))) long long b = 7;
    _Alignas(64) double d = 1.5;
    volatile _Alignas(32) unsigned char v = 200;
    _Alignas(64) char *p = 0;

    if (!aligned_to(&a, 64) || !aligned_to(&b, 32) || !aligned_to(&d, 64) ||
        !aligned_to((const void *)&v, 32) || !aligned_to(&p, 64))
        return 1;
    a += 3;                         /* 8 */
    a++;                            /* 9 */
    bump(&a);                       /* 10, through the address */
    if (a != 10) return 2;
    b <<= 4;                        /* 112 */
    if (b != 112) return 3;
    d = d * 4;                      /* 6.0 */
    if (d != 6.0) return 4;
    v += 100;                       /* 300 wraps to 44 */
    if (v != 44) return 5;
    p = (char *)&a;
    if (*(int *)p != 10) return 6;

    int sum = 0;
    for (int i = 0; i < 4; i++) {
        _Alignas(64) int t = i;     /* the same storage every trip */
        if (!aligned_to(&t, 64)) return 7;
        t *= 2;
        sum += t;                   /* 0 + 2 + 4 + 6 = 12 */
    }
    if (sum != 12) return 8;
    if (depth(3) != 60) return 9;
    return a + (int)(b - 112) + (int)d + sum + (v - 44) + 14;   /* 10+0+6+12+0+14 */
}
