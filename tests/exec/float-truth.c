/* A floating value used as a condition is compared with zero AS A FLOAT
 * (C11 6.8.4.1, 6.5.3.3, 6.5.13-14): -0.0 is false, a NaN is true (NaN !=
 * 0), and so is the smallest subnormal. Testing the bit pattern instead gets
 * -0.0 wrong — its sign bit is set — in every one of if / while / for /
 * do-while / ?: / ! / && / ||.
 */
// expect-exit: 42
volatile double dz = -0.0, dn, dt = 4.9406564584124654e-324, d1 = 1.0;
volatile float fz = -0.0f, fn, ft = 1.40129846e-45f;

static int tally_d(double x)
{
    int r = 0;
    if (x) r |= 1;
    if (!x) r |= 2;
    if (x && 1) r |= 4;
    if (0 || x) r |= 8;
    r |= (x ? 16 : 0);
    while (x) { r |= 32; break; }
    for (; x;) { r |= 64; break; }
    int once = 0;
    do { once++; } while (x && once < 2);
    if (once == 2) r |= 128;
    return r;
}

static int tally_f(float x)
{
    int r = 0;
    if (x) r |= 1;
    if (!x) r |= 2;
    if (x && 1) r |= 4;
    if (0 || x) r |= 8;
    r |= (x ? 16 : 0);
    while (x) { r |= 32; break; }
    return r;
}

int main(void)
{
    dn = 0.0 / (dz == 0.0 ? 0.0 : 1.0);   /* NaN, made at run time */
    fn = (float)dn;
    if (tally_d(dz) != 2) return 1;                   /* -0.0: false */
    if (tally_d(dn) != 255 - 2) return 2;             /* NaN: true */
    if (tally_d(dt) != 255 - 2) return 3;             /* subnormal: true */
    if (tally_d(d1) != 255 - 2) return 4;
    if (tally_f(fz) != 2) return 5;
    if (tally_f(fn) != 63 - 2) return 6;
    if (tally_f(ft) != 63 - 2) return 7;
    return 42;
}
