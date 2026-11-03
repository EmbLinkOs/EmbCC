/* A multiply of which only the low bytes are read, one operand an
 * eight-byte value truncated: on AVR the eight-byte value sat in
 * r18-r25 and the other operand was loaded into r18:r19 over it, so the
 * product was that operand squared. Only the bit-field store's five bits
 * were read of it, which is what made the multiply narrow (found by
 * verify/fuzz/run-avr.sh). Both orders, and a constant, against the same
 * products computed apart. */
// expect-exit: 42

typedef unsigned long u32;
typedef unsigned long long u64;

static float tof(signed char a) { return (float)a; }
static long f2i(double d) { return (d > -2147483648.0 && d < 2147483647.0) ? (long)d : 0; }
static u64 tab[8] = { 0, 0, 0x3C6EF362ULL, 0, 0x12345678ULL, 0, 0, 0xFFFFFFFF87654321ULL };
struct bits { unsigned b3 : 3; signed s5 : 5; };

__attribute__((noinline)) static int a_times_w(long i, signed char a)
{
    struct bits s;
    s.b3 = 0;
    s.s5 = (long)((u32)((u32)f2i(tof(a)) * (u32)tab[(u32)i & 7]) & 31U) - 16;
    return s.s5;
}

__attribute__((noinline)) static int w_times_a(long i, signed char a)
{
    struct bits s;
    s.b3 = 1;
    s.s5 = (long)((u32)((u32)tab[(u32)i & 7] * (u32)f2i(tof(a))) & 31U) - 16;
    return s.s5 * 8 + s.b3;
}

__attribute__((noinline)) static int w_times_k(long i)
{
    struct bits s;
    s.b3 = 0;
    s.s5 = (long)((u32)((u32)tab[(u32)i & 7] * 1013904226UL) & 31U) - 16;
    return s.s5;
}

static int want(u32 x) { return (int)(x & 31U) - 16; }

int main(void)
{
    static const long idx[] = { -1218582798, 4, 7, 2 };
    static const signed char av[] = { 127, -3, 5, 0 };
    for (int k = 0; k < 4; k++) {
        u32 w = (u32)tab[(u32)idx[k] & 7];
        u32 p = (u32)(long)av[k] * w;
        if (a_times_w(idx[k], av[k]) != want(p))
            return 1 + k;
        if (w_times_a(idx[k], av[k]) != want(p) * 8 + 1)
            return 10 + k;
        if (w_times_k(idx[k]) != want(w * 1013904226UL))
            return 20 + k;
    }
    return 42;
}
