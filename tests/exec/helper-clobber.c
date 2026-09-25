/* Values that must survive a call the IR never spells out.
 *
 * Some arithmetic is not an instruction. Long double add, multiply,
 * divide and compare, and __int128 multiply, divide, remainder and
 * shift, are calls into libgcc that the BACKEND emits -- there is no
 * IR_CALL anywhere for anything to see. The register allocator builds
 * its "does this value cross a call" mask by looking for IR_CALL, so on
 * a machine whose allocatable pool holds caller-saved registers it will
 * happily leave a value in one across `bl __divtf3`, and the helper
 * will return having overwritten it.
 *
 * This is what that looks like when it goes wrong: aarch64 with x15 in
 * the pool computed the address of a local, divided two long doubles,
 * and dereferenced what was left -- a data abort at 0x10, because the
 * helper had put a small integer where the address had been.
 *
 * So every check below computes something with a helper in the middle
 * of a long live range, and then uses the values that were live across
 * it. What matters is not the arithmetic -- the answers are ordinary --
 * but that each `keep` is still what it was. The pointers matter most:
 * a clobbered integer gives a wrong answer, a clobbered pointer gives a
 * fault, and the fault is the one that was actually found.
 */
// expect-exit: 42

static long arr[8] = { 100, 200, 300, 400, 500, 600, 700, 800 };

/* long double: the operands go to helpers, the longs and the pointer
 * must not move. */
static long ld_across(long double a, long double b, long *p)
{
    long *q0 = p + 1, *q1 = p + 3;       /* addresses, live across it all */
    long k0 = p[0], k1 = p[2], k2 = p[4], k3 = p[5], k4 = p[6];
    long double s = a + b;
    long double d = a / b;               /* __divtf3 */
    long double m = s * d;               /* __multf3 */
    int gt = m > a;                      /* a comparison helper */
    return *q0 + *q1 + k0 + k1 + k2 + k3 + k4 + gt + (long)(d * 0.0L);
}

/* __int128: multiply, divide, remainder and shift are all calls. */
static long i128_across(__int128 a, __int128 b, long *p)
{
    long *q0 = p + 1, *q1 = p + 3;
    long k0 = p[0], k1 = p[2], k2 = p[4], k3 = p[5], k4 = p[6];
    __int128 m = a * b;                  /* __multi3 */
    __int128 d = m / b;                  /* __divti3 */
    __int128 r = m % b;                  /* __modti3 */
    __int128 s = d << 3;                 /* __ashlti3 */
    return *q0 + *q1 + k0 + k1 + k2 + k3 + k4
         + (long)(r == 0) + (long)(s >> 3 == a);
}

/* Both kinds in one function, with the live values interleaved so the
 * allocator has every reason to keep them in registers. */
static long both(long double x, __int128 y, long *p)
{
    long *q = p + 2;
    long k0 = p[0], k1 = p[1], k2 = p[3], k3 = p[4];
    long double a = x * x;               /* helper */
    __int128 b = y * y;                  /* helper */
    long double c = a / (x + 1.0L);      /* helper */
    __int128 e = b / (y + 1);            /* helper */
    return *q + k0 + k1 + k2 + k3 + (long)(c > 0.0L) + (long)(e > 0);
}

int main(void)
{
    long r = 0;

    r = ld_across(6.0L, 3.0L, arr);
    /* 200 + 400 + 100 + 300 + 500 + 600 + 700, +1 for m > a, +0 */
    if (r != 200 + 400 + 100 + 300 + 500 + 600 + 700 + 1) return 1;

    r = i128_across((__int128)1000003, (__int128)97, arr);
    if (r != 200 + 400 + 100 + 300 + 500 + 600 + 700 + 1 + 1) return 2;

    r = both(2.0L, (__int128)5, arr);
    if (r != 300 + 100 + 200 + 400 + 500 + 1 + 1) return 3;

    /* and again with the pointer deliberately not the array base, so a
     * clobbered pointer cannot accidentally still be in range */
    r = ld_across(1.5L, 0.25L, arr + 1);
    if (r != 300 + 500 + 200 + 400 + 600 + 700 + 800 + 1) return 4;

    return 42;
}
