/* Floating point on a machine with no FPU (D-015): every operation the
 * Thumb backend turns into a call, and the lib/rt/softfp.c routines
 * behind them.
 *
 * Results are printed as BIT PATTERNS, because that is what "correct"
 * means for IEEE arithmetic — a value that prints the same to fifteen
 * digits can still be a rounding off. The reference is the host, whose
 * hardware does these in one instruction.
 *
 * Nothing here uses `long`: it is eight bytes on the host and four on
 * the target, so a conversion into one would differ between the two
 * for reasons that have nothing to do with the arithmetic.
 */
extern void writec(int c);
extern void puts_(const char *s);
extern void putn(long v);

static void hx(unsigned long long v, int n)
{
    for (int i = (n - 1) * 4; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfULL)]);
    writec(' ');
}
static unsigned long long db(double d)
{ union { double d; unsigned long long u; } x; x.d = d; return x.u; }
static unsigned fb(float f)
{ union { float f; unsigned u; } x; x.f = f; return x.u; }
static void hd(double d) { hx(db(d), 16); }
static void hf(float f) { hx((unsigned long long)fb(f), 8); }
static void nl(void) { writec('\n'); }
static void pi(int v) { putn((long)v); }

/* A float→int conversion is UNDEFINED in C when the value does not fit,
 * and the implementations differ: one saturates, another wraps, a third
 * gives the integer minimum. So the out-of-range cases are answered
 * here rather than asked of the compiler — the test is about the
 * conversions that have an answer. */
static int fits(double d) { return d > -2147483000.0 && d < 2147483000.0; }
static void pid(double d) { pi(fits(d) ? (int)d : 0); }

/* Zeros of both signs, one, a half, pi, the extremes of the exponent
 * range, the smallest normal and the smallest denormal, a value that
 * needs every significand bit, and the powers of two where a double
 * stops being able to count. */
static const double dv[] = {
    0.0, -0.0, 1.0, -1.0, 0.5, 3.14159265358979,
    1e-300, 1e300, 2.2250738585072014e-308, 5e-324, 123456789.0,
    -0.1, 1.0 / 3.0, 1e16, 4503599627370496.0, 7.0, -2.5, 1e-310
};
static const float fv[] = {
    0.0f, -1.5f, 3.14159f, 1e-40f, 1e38f, 0.1f, 16777217.0f, -7.25f, 1.0f
};

int main(void)
{
    int nd = (int)(sizeof dv / sizeof dv[0]);
    int nf = (int)(sizeof fv / sizeof fv[0]);

    for (int i = 0; i < nd; i++)
        for (int j = 0; j < nd; j++) {
            hd(dv[i] + dv[j]); hd(dv[i] - dv[j]);
            hd(dv[i] * dv[j]); hd(dv[i] / dv[j]);
            pi(dv[i] < dv[j]); pi(dv[i] <= dv[j]); pi(dv[i] == dv[j]);
            pi(dv[i] != dv[j]); pi(dv[i] > dv[j]); pi(dv[i] >= dv[j]);
            nl();
        }

    for (int i = 0; i < nf; i++)
        for (int j = 0; j < nf; j++) {
            hf(fv[i] + fv[j]); hf(fv[i] - fv[j]);
            hf(fv[i] * fv[j]); hf(fv[i] / fv[j]);
            pi(fv[i] < fv[j]); pi(fv[i] == fv[j]); pi(fv[i] >= fv[j]);
            nl();
        }

    /* Negation, the conversions between the two formats, and back and
     * forth through the integers. */
    for (int i = 0; i < nd; i++) {
        hd(-dv[i]); hf((float)dv[i]); hd((double)(float)dv[i]);
        hx((unsigned long long)(long long)dv[i], 16);
        pid(dv[i]);
        nl();
    }
    for (int i = 0; i < nf; i++) {
        hf(-fv[i]); hd((double)fv[i]);
        hx((unsigned long long)(long long)fv[i], 16);
        pid((double)fv[i]);
        nl();
    }
    for (int k = -70000; k <= 70000; k += 7777) {
        hd((double)k); hf((float)k);
        hd((double)(unsigned)k); hf((float)(unsigned)k);
        hd((double)(long long)k * 1.5); pid((double)k * 1.5);
        nl();
    }
    /* Accumulation, where a wrong rounding shows up as drift. */
    { double s = 0.0; float t = 0.0f;
      for (int k = 1; k <= 400; k++) { s = s + 1.0 / (double)k;
                                       t = t + 1.0f / (float)k; }
      hd(s); hf(t); nl(); }

    puts_("==END==\n");
    return 0;
}
