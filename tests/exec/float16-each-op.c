// expect-exit: 42
// no-gcc-reference: GCC on x86-64 evaluates _Float16 in float and rounds once, at the end (FLT_EVAL_METHOD 16); this checks the rounding of each operation that EmbCC documents
/* Each _Float16 operation is rounded to binary16, as on a machine with
 * binary16 arithmetic (and as clang computes it on one, an Apple M-series):
 * 2048 + 1 is a tie, and rounds to the even 2048, before anything else
 * sees it -- in an expression, in a constant folded at compile time, and
 * in a conversion of the result. EmbCC documents FLT_EVAL_METHOD 0. */

#include <string.h>

/* and no contraction: t * 10 - 1 fused is exact, -2^-12 */
#pragma STDC FP_CONTRACT OFF

typedef _Float16 h16;

static unsigned short bits(h16 h)
{
    unsigned short u;
    memcpy(&u, &h, 2);
    return u;
}

static h16 folded = 2048.0f16 + 1.0f16 + 1.0f16;   /* 2048, then 2048 */
static h16 folded2 = (2048.0f16 + 1.0f16) - 2048.0f16;

int main(void)
{
    volatile h16 a = 2048, b = 1, c = 1, t = 0.1f16;
    h16 r = a + b + c;              /* (2048 + 1) = 2048, + 1 = 2048 */
    if (bits(r) != 0x6800)
        return 1;
    h16 d = (a + b) - a;            /* 0, not 1 */
    if (bits(d) != 0x0000)
        return 2;
    float f = a + b;                /* the half sum, converted: 2048 */
    if (f != 2048.0f)
        return 3;
    if (a + b != a)                 /* compared as the half it is */
        return 4;
    if (bits(folded) != 0x6800 || bits(folded2) != 0x0000)
        return 5;
    /* t * 10 is 0.999755859375, a tie in binary16 that rounds to 1, so
     * m is 0; in float it is exact, and m would be -2^-12 (0x8c00) */
    h16 m = t * 10 - 1;
    if (bits(m) != 0x0000)
        return 6;
    return 42;
}
