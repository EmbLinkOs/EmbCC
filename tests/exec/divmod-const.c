/* A remainder and a quotient by the same constant, in both orders, signed
 * and unsigned, with negative dividends and divisors and the extremes of
 * the type. On Thumb the pair shares one divide and the remainder becomes
 * a - q*k in a single mls; C's truncating division makes that exact for
 * every value the division itself is defined for. Plus a multiply by a
 * constant feeding an add or subtract, which keeps the constant in a
 * register so it can be one mla or mls. */
// expect-exit: 42
static volatile int vz;

static unsigned long udigits(unsigned long v, unsigned char *out)
{
    unsigned long sum = 0;
    int n = 0;
    do {
        out[n++] = (unsigned char)(v % 10);
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        sum = sum * 10 + out[n - 1 - i];
    return sum * 100 + (unsigned long)n;
}

static long spair(long a, long *r)
{
    long q = a / -7;
    *r = a % -7;
    return q;
}

static long spair2(long a, long *q)
{
    long r = a % 1000;        /* remainder first */
    *q = a / 1000;
    return r;
}

static unsigned long upair3(unsigned long a)
{
    return (a / 3) * 7 + a % 3;
}

/* an unsigned quotient and a signed remainder of the same operands: not a
 * pair, whatever the names say */
static long mixed(long a, long *r)
{
    unsigned long q = (unsigned long)a / 10;
    *r = a % 10;
    return (long)q;
}

static long macc(const long *x, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++)
        s += x[i] * 10;
    for (int i = 0; i < n; i++)
        s -= x[i] * 3;
    return s;
}

int main(void)
{
    unsigned char buf[12];
    if (udigits(4294967295UL + vz, buf) != 4294967295UL * 100 + 10)
        return 1;
    if (udigits(0UL + vz, buf) != 1 || udigits(7051UL + vz, buf) != 705104)
        return 2;
    long r;
    if (spair(100L + vz, &r) != -14 || r != 2)
        return 3;
    if (spair(-100L + vz, &r) != 14 || r != -2)
        return 4;
    if (spair(-2147483647L - 1 + vz, &r) != 306783378L || r != -2)
        return 5;
    long q;
    if (spair2(-123456L + vz, &q) != -456 || q != -123)
        return 6;
    if (spair2(2147483647L + vz, &q) != 647 || q != 2147483L)
        return 7;
    if (upair3(4294967295UL + vz) != 1431655765UL * 7 + 0)
        return 8;
    if (upair3(100UL + vz) != 33UL * 7 + 1)
        return 9;
    {
        volatile unsigned long m15 = (unsigned long)(-15L + vz);
        if (mixed(-15L + vz, &r) != (long)(m15 / 10) || r != -5)
            return 11;
    }
    long x[5] = { 1, -2, 3, 40000, -5 };
    x[0] += vz;
    if (macc(x, 5) != (1 - 2 + 3 + 40000 - 5) * 7L)
        return 10;
    return 42;
}
