// expect-exit: 42
/* An inlined function's double result keeps all eight bytes: the
 * inliner's move of a returned value had no width, and AArch64 copied
 * it between stack slots as four (msun's lgamma of a negative argument
 * returned infinity at -O1). */
static double scale(double x, int *sign)
{
    if (x < 0) {
        *sign = -1;
        return -x * 3.5 + 1e-300;
    }
    *sign = 1;
    return x * 0.25 + 1e300;
}

static float scalef(float x, int *sign)
{
    if (x < 0) {
        *sign = -1;
        return -x * 3.5f;
    }
    *sign = 1;
    return x * 0.25f;
}

volatile double in[4] = { -2.75, 1.5, -1e-10, 4e10 };
volatile float in_f[2] = { -2.75f, 1.5f };

int main(void)
{
    double r[4];
    int s[4], sf[2];
    float rf[2];
    for (int i = 0; i < 4; i++)
        r[i] = scale(in[i], &s[i]);
    for (int i = 0; i < 2; i++)
        rf[i] = scalef(in_f[i], &sf[i]);
    if (r[0] != 2.75 * 3.5 + 1e-300 || s[0] != -1) return 1;
    if (r[1] != 1.5 * 0.25 + 1e300 || s[1] != 1) return 2;
    if (r[2] != 1e-10 * 3.5 + 1e-300 || s[2] != -1) return 3;
    if (r[3] != 4e10 * 0.25 + 1e300 || s[3] != 1) return 4;
    if (rf[0] != 2.75f * 3.5f || sf[0] != -1) return 5;
    if (rf[1] != 1.5f * 0.25f || sf[1] != 1) return 6;
    return 42;
}
