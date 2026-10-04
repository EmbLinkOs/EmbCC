/* Doubles whose bits are taken apart and put back -- fdlibm's way, through
 * a union -- and doubles that integer and floating-point operations both
 * touch, computed at -O2 where a value has a register home:
 *
 *   - the high word read out of a parameter, cleared, written back
 *     (fabs), and a whole double rebuilt from two words (INSERT_WORDS);
 *   - a value that is multiplied AND has its bits examined, so it lives in
 *     one register file and crosses to the other;
 *   - a polynomial of many distinct constants, which a backend may load
 *     from a literal pool, and +0.0 / -0.0, which differ in one bit.
 *
 * Each is noinline, so its double arrives as a PARAMETER (in d0 on
 * AArch64) and leaves as a return value, which is where the register
 * files are crossed. Plain portable C (union punning is
 * implementation-defined, and every
 * target here is IEEE 754 little-endian), so x86-64 referees the values.
 */
// expect-exit: 42
int printf(const char *fmt, ...);

typedef union { double value; struct { unsigned lsw, msw; } parts; } shape;

__attribute__((noinline)) static double my_fabs(double x)
{
    unsigned hi;
    { shape u; u.value = x; hi = u.parts.msw; }
    { shape u; u.value = x; u.parts.msw = hi & 0x7fffffffu; x = u.value; }
    return x;
}

__attribute__((noinline)) static double insert_words(unsigned hi, unsigned lo)
{
    shape u;
    u.parts.msw = hi;
    u.parts.lsw = lo;
    return u.value;
}

__attribute__((noinline)) static int ilogb_bits(double x)
{
    int hx, lx;
    { shape u; u.value = x; hx = (int)u.parts.msw; lx = (int)u.parts.lsw; }
    return ((hx >> 20) & 0x7ff) - 1023 + (lx != 0);
}

/* y is a product and also a bit pattern */
__attribute__((noinline)) static double mix(double x)
{
    double y = x * 3.0;
    unsigned hi;
    { shape u; u.value = y; hi = u.parts.msw; }
    return y + (double)(hi & 1);
}

__attribute__((noinline)) static double poly(double z)
{
    return 1.66666666666666019037e-01 + z * (-2.77777777770155933842e-03 +
           z * (6.61375632143793436117e-05 + z * (-1.65339022054652515390e-06 +
           z * 4.13813679705723846039e-08)));
}

__attribute__((noinline)) static double zero(void) { return 0.0; }
__attribute__((noinline)) static double negzero(void) { return -0.0; }

static unsigned long long bits(double d)
{
    shape u;
    u.value = d;
    return (unsigned long long)u.parts.msw << 32 | u.parts.lsw;
}

int main(void)
{
    int bad = 0;
    volatile double a = -2.5, b = 1.0e300, c = 7.0, h = 0.5;
    if (my_fabs(a) != 2.5) { printf("fabs(-2.5)\n"); bad++; }
    if (my_fabs(2.5) != 2.5) { printf("fabs(2.5)\n"); bad++; }
    if (insert_words(0x40090000u, 0) != 3.125) { printf("insert_words\n"); bad++; }
    if (bits(insert_words(0x3ff00000u, 1)) != 0x3ff0000000000001ULL) {
        printf("insert_words low\n"); bad++;
    }
    if (ilogb_bits(b) != 996 + 1) { printf("ilogb %d\n", ilogb_bits(b)); bad++; }
    if (ilogb_bits(c) != 2) { printf("ilogb 7: %d\n", ilogb_bits(c)); bad++; }
    /* 7*3 = 21: high word 0x40350000, bit 0 clear; 0.5*3 = 1.5: 0x3ff80000 */
    if (mix(c) != 21.0) { printf("mix(7)\n"); bad++; }
    if (mix(h) != 1.5) { printf("mix(0.5)\n"); bad++; }
    /* the exact bits, unfused, as clang -ffp-contract=off computes them:
     * a wrong constant anywhere in the pool changes them */
    if (bits(poly(h)) != 0x3fc5285b7a370ef1ULL) {
        printf("poly %llx\n", bits(poly(h))); bad++;
    }
    if (bits(zero()) != 0) { printf("+0.0\n"); bad++; }
    if (bits(negzero()) != 0x8000000000000000ULL) { printf("-0.0\n"); bad++; }
    return bad ? 1 : 42;
}
