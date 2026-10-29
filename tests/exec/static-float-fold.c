// expect-exit: 42
/* A static initializer's float arithmetic is rounded as the operations
 * themselves round at run time: each sum, difference, product and
 * quotient to its own type. sema folded float operations in double and
 * rounded only the final value, so (16777216.0f + 1.0f) - 16777216.0f was
 * 1 -- in double 2^24 + 1 is exact -- where every float machine gives 0:
 * 2^24 + 1 is a tie in float, and rounds to 2^24. */

static float big = (16777216.0f + 1.0f) - 16777216.0f;
static float third = 1.0f / 3.0f * 3.0f - 1.0f;
static float prod = 4097.0f * 4097.0f - 16785408.0f;
static double dbig = (9007199254740992.0 + 1.0) - 9007199254740992.0;

int main(void)
{
    volatile float a = 16777216.0f, b = 1.0f, c = 3.0f, d = 4097.0f;
    float r1 = (a + b) - a;
    volatile float q = b / c, qc = q * c, p = d * d;   /* no fused forms */
    float r2 = qc - b;
    float r3 = p - 16785408.0f;
    if (big != r1 || third != r2 || prod != r3)
        return 1;
    if (big != 0.0f || dbig != 0.0)
        return 2;
    return 42;
}
