/* Floating-point arithmetic on constants is folded at -O1 and above;
 * the folded bits must be the machine's bits. Each case is computed
 * twice -- once from constants the optimizer folds, once from volatile
 * operands the machine computes -- and the two compared bit for bit:
 * rounding to nearest-even, subnormals, overflow to infinity, signed
 * zeros, division by zero, and the comparisons. A NaN result is never
 * folded (which NaN is the machine's choice), so those cases only check
 * that the program still agrees with itself. */
// expect-exit: 42
static int same(const void *p, const void *q, int n)
{
    const unsigned char *a = p, *b = q;
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static int bad;
#define CASE(T, op, x, y) do { \
        volatile T vx = (x), vy = (y); \
        T folded = (T)(x) op (T)(y), run = vx op vy; \
        if (!same(&folded, &run, sizeof folded)) bad++; \
    } while (0)
#define CMP(T, op, x, y) do { \
        volatile T vx = (x), vy = (y); \
        int folded = (T)(x) op (T)(y), run = vx op vy; \
        if (folded != run) bad++; \
    } while (0)
#define ALL(T, x, y) do { \
        CASE(T, +, x, y); CASE(T, -, x, y); CASE(T, *, x, y); \
        CASE(T, /, x, y); \
        CMP(T, ==, x, y); CMP(T, !=, x, y); CMP(T, <, x, y); \
        CMP(T, <=, x, y); CMP(T, >, x, y); CMP(T, >=, x, y); \
    } while (0)
int main(void)
{
    ALL(float, 251, 255.0f);
    ALL(float, 1.0f, 3.0f);
    ALL(float, 0.1f, 0.2f);
    ALL(float, 16777216.0f, 1.0f);          /* 2^24 + 1: ties to even */
    ALL(float, 1e-38f, 1e-7f);              /* into the subnormals */
    ALL(float, 1.4e-45f, 0.5f);             /* the smallest subnormal */
    ALL(float, 3.4e38f, 10.0f);             /* overflow to infinity */
    ALL(float, -0.0f, 0.0f);                /* signed zeros */
    ALL(float, 5.0f, 0.0f);                 /* division by zero */
    ALL(float, -7.125f, 1e10f);
    ALL(double, 1.0, 3.0);
    ALL(double, 0.1, 0.2);
    ALL(double, 9007199254740992.0, 1.0);   /* 2^53 + 1 */
    ALL(double, 2.2250738585072014e-308, 1e-10);
    ALL(double, 4.9e-324, 0.25);
    ALL(double, 1.7976931348623157e308, 2.0);
    ALL(double, -0.0, -0.0);
    ALL(double, -1.0, 0.0);
    ALL(double, 1e300, 1e-300);
    {   /* negation, and a folded value that feeds a branch */
        volatile float vf = 2.5f;
        float nf = -(2.5f), rf = -vf;
        if (!same(&nf, &rf, sizeof nf)) bad++;
        volatile double a = 0.1, b = 0.2, c = 0.3;
        if ((0.1 + 0.2 == 0.3) != (a + b == c))
            bad++;      /* false in binary64, true where double is binary32 */
    }
    return bad ? 1 + (bad & 0x3f) : 42;
}
