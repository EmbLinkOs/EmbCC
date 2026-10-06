/* 64-bit integers on a 32-bit machine (D-015): every operation the
 * Thumb backend lowers into a register pair, and the two it calls
 * lib/rt/int64.c for.
 *
 * The expected answers are not written down here. The same source is
 * compiled for the host and run, and its output is the reference —
 * `long long` arithmetic gives one answer whatever the register width,
 * so a disagreement is the backend's.
 */
extern void writec(int c);
extern void puts_(const char *s);
extern void putn(long v);

static void hx(unsigned long long v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfULL)]);
    writec(' ');
}

static void nl(void) { writec('\n'); }

static long long gs;
static unsigned long long gu;

static long long calls(long long a, long long b, int c, long long d)
{
    /* Four arguments over the register file: a in r0:r1, b in r2:r3,
     * c on the stack, d on the stack eight-aligned. */
    return a * 3 + b - (long long)c + d;
}

int main(void)
{
    unsigned long long a = 0x1234567890abcdefULL, b = 1000000007ULL;
    long long s = -1311768467294899695LL, t = 1000000007LL;

    hx(a); hx(b); nl();
    hx(a + b); hx(a - b); hx(b - a); nl();
    hx(a & b); hx(a | b); hx(a ^ b); hx(~a); hx(0ULL - a); nl();
    hx(a * b); hx(a * 3ULL); hx(b * b); nl();

    /* Shifts by a constant, on both sides of 32, and by a variable. */
    hx(a >> 1); hx(a >> 31); hx(a >> 32); hx(a >> 33); hx(a >> 63); nl();
    hx(a << 1); hx(a << 31); hx(a << 32); hx(a << 33); nl();
    hx((unsigned long long)(s >> 1)); hx((unsigned long long)(s >> 32));
    hx((unsigned long long)(s >> 33)); hx((unsigned long long)(s >> 63)); nl();
    /* Under 64: a shift by the width or more is undefined in C, and
     * the two machines answer it differently (x86 masks the count to
     * six bits; ARM's register shifts saturate to zero). A test that
     * asked would be testing which undefined answer, not the code. */
    for (int k = 0; k < 64; k += 13) {
        hx(a >> k); hx(a << k); hx((unsigned long long)(s >> k));
    }
    nl();

    /* Divide and remainder, signed and unsigned, through the runtime. */
    hx(a / b); hx(a % b);
    hx((unsigned long long)(s / t)); hx((unsigned long long)(s % t));
    hx((unsigned long long)(s / -t)); hx((unsigned long long)(-s % t)); nl();
    /* The identity that has to hold whatever the implementation. */
    putn((a / b) * b + a % b == a);
    putn((s / t) * t + s % t == s);
    nl();

    /* Comparisons: both halves, and the cases that differ only above
     * the low word. */
    putn(a > b); putn(a < b); putn(a >= a); putn(a <= b); putn(a == a);
    putn(a != b); nl();
    putn(s < t); putn(s > t); putn(s <= s); putn(-1LL < 1LL);
    putn(0xffffffffULL < 0x100000000ULL);
    putn((unsigned long long)-1 > 1ULL); nl();

    /* Widening and truncation. */
    { int n = -5; long long w = n; unsigned u = 4000000000u;
      hx((unsigned long long)w); hx((unsigned long long)u);
      putn((int)a); putn((int)(a >> 32)); nl(); }

    /* Through memory, and across a call boundary. */
    gs = s; gu = a;
    gs += 1; gu -= 1;
    hx((unsigned long long)gs); hx(gu); nl();
    hx((unsigned long long)calls(a, 7LL, -3, -9LL)); nl();

    { unsigned long long arr[4]; 
      for (int i = 0; i < 4; i++) arr[i] = a >> (i * 8);
      for (int i = 0; i < 4; i++) hx(arr[i]);
      nl(); }

    /* __builtin_{add,sub,mul}_overflow on 64-bit values. These are the
     * cases where the naive lowering traps rather than answers -- a
     * multiply by zero (the divisor guard) and LLONG_MIN * -1, whose
     * wrapped product is LLONG_MIN and whose division by -1 traps on
     * the very case being detected. On a 32-bit target the 64-bit
     * divide goes through lib/rt/int64.c, which is why they belong in
     * THIS corpus and not only in the host-side test. */
    {
        long long lmin = (long long)((unsigned long long)1 << 63);
        long long lmax = (long long)~(unsigned long long)((unsigned long long)1 << 63);
        long long r;
        unsigned long long ur;
        putn(__builtin_add_overflow(lmax, 1LL, &r));        hx((unsigned long long)r);
        putn(__builtin_add_overflow(lmin, -1LL, &r));       hx((unsigned long long)r);
        putn(__builtin_add_overflow(2LL, 3LL, &r));         hx((unsigned long long)r);
        nl();
        putn(__builtin_sub_overflow(lmin, 1LL, &r));        hx((unsigned long long)r);
        putn(__builtin_sub_overflow(lmax, -1LL, &r));       hx((unsigned long long)r);
        putn(__builtin_sub_overflow(9LL, 4LL, &r));         hx((unsigned long long)r);
        nl();
        putn(__builtin_mul_overflow(lmin, -1LL, &r));       hx((unsigned long long)r);
        putn(__builtin_mul_overflow(-1LL, lmin, &r));       hx((unsigned long long)r);
        putn(__builtin_mul_overflow(0LL, lmin, &r));        hx((unsigned long long)r);
        putn(__builtin_mul_overflow(lmin, 0LL, &r));        hx((unsigned long long)r);
        nl();
        putn(__builtin_mul_overflow(4000000000LL, 4000000000LL, &r));
        hx((unsigned long long)r);
        putn(__builtin_mul_overflow(123456LL, 654321LL, &r));
        hx((unsigned long long)r);
        putn(__builtin_mul_overflow(lmax, 2LL, &r));        hx((unsigned long long)r);
        nl();
        putn(__builtin_mul_overflow(0ULL, ~0ULL, &ur));     hx(ur);
        putn(__builtin_mul_overflow(~0ULL, 2ULL, &ur));     hx(ur);
        putn(__builtin_mul_overflow(1000ULL, 1000ULL, &ur)); hx(ur);
        putn(__builtin_add_overflow(~0ULL, 1ULL, &ur));     hx(ur);
        putn(__builtin_sub_overflow(0ULL, 1ULL, &ur));      hx(ur);
        nl();
    }

    /* x++ and x-- are worth x's OLD value, all eight bytes of it: through
     * a local, a volatile one, a pointer and an array element. */
    {
        volatile long long vq = 0x123456789LL;
        long long q0 = vq--;
        unsigned long long arr2[2] = { 0xffffffffULL, 0x1ffffffffULL };
        unsigned long long *pp = &arr2[1];
        unsigned long long u0 = arr2[0]++;
        unsigned long long u1 = (*pp)--;
        long long l = -1;
        long long l0 = l++;
        hx((unsigned long long)q0); hx((unsigned long long)vq); nl();
        hx(u0); hx(arr2[0]); hx(u1); hx(arr2[1]); nl();
        hx((unsigned long long)l0); hx((unsigned long long)l); nl();
    }

    puts_("==END==\n");
    return 0;
}
