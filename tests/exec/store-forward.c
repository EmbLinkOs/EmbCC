// expect-exit: 42
/* A load that reads back what a store just wrote is that value -- unless
 * something in between could have changed the memory. Each function is
 * one of the ways forwarding the value would be wrong: a store through a
 * pointer that may alias, a union zeroed and then partly written, the
 * high word of a double read through a union, a volatile access. */
typedef unsigned int u32;
typedef unsigned long long u64;

__attribute__((noinline)) static int roundtrip(int *p, int x)
{
    *p = x;
    return *p;                      /* x */
}

__attribute__((noinline)) static int aliased(int *p, int *q, int x)
{
    *p = x;
    *q = 7;                         /* p and q may be the same */
    return *p;
}

__attribute__((noinline)) static double droundtrip(double *p, double x)
{
    *p = x;
    return *p;
}

__attribute__((noinline)) static u64 bits(double x)
{
    union { double d; u64 u; } v = { x };
    return v.u;
}

__attribute__((noinline)) static u32 high_word(double x)
{
    union { double d; u32 w[2]; } v = { x };
    return v.w[1];                  /* little-endian: the sign and exponent */
}

__attribute__((noinline)) static u32 zeroed_half(u32 lo)
{
    /* the initializer zeroes all eight bytes; only the first four are
     * written after it, so the second four must still read 0 */
    union { u64 q; u32 w[2]; } v = { 0 };
    v.w[0] = lo;
    return v.w[1] + (v.w[0] == lo ? 0 : 100);
}

__attribute__((noinline)) static u32 init_half(u32 lo)
{
    /* `= { lo }` zeroes the union and then writes only the first word */
    union { u32 w[2]; u64 q; } v = { lo };
    return v.w[1] + (v.w[0] == lo ? 0 : 100);
}

__attribute__((noinline)) static u32 init_member(u32 lo)
{
    struct { u32 a, b; } s = { lo };    /* b is zero */
    return s.b + (s.a == lo ? 0 : 100);
}

/* Nonzero bytes where the next call's frame will be, so a zeroing the
 * compiler wrongly dropped reads garbage rather than a lucky zero. */
__attribute__((noinline)) static void scribble(void)
{
    volatile unsigned char junk[256];
    for (int i = 0; i < 256; i++)
        junk[i] = (unsigned char)(0xa5 ^ i);
}

__attribute__((noinline)) static int vol(volatile int *p, int x)
{
    *p = x;
    return *p;
}

int main(void)
{
    int a = 0, b = 0;
    volatile int c = 0;
    double d = 0;
    if (roundtrip(&a, 5) != 5) return 1;
    if (aliased(&a, &b, 3) != 3) return 2;
    if (aliased(&a, &a, 3) != 7) return 3;     /* the same object */
    if (droundtrip(&d, 2.5) != 2.5) return 4;
    if (bits(1.0) != 0x3ff0000000000000ULL) return 5;
    if (bits(-0.0) != 0x8000000000000000ULL) return 6;
    if (high_word(-2.0) != 0xc0000000u) return 7;
    scribble();
    if (zeroed_half(0xdeadbeefu) != 0) return 8;
    if (vol(&c, 9) != 9) return 9;
    scribble();
    if (init_half(0x12345678u) != 0) return 10;
    scribble();
    if (init_member(0x12345678u) != 0) return 11;
    return 42;
}
