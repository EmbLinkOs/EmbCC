// A 32-bit value widened after it was narrowed from a 64-bit one, or after
// it came back from a call: (unsigned __int128)(unsigned)v is v's low 32
// bits. AArch64 kept the register's upper half -- the narrowing copy was a
// 64-bit move, and a call's 32-bit result has an upper half the ABI leaves
// unspecified -- and a 4-byte value read at 8 bytes was taken as already
// zero-extended. Fuzz seed 30003 (gen 3) found it through a 128-bit compare.
// expect-exit: 42
typedef unsigned long long u64;

static volatile long long big = 0x123456789LL;          /* upper bits set */
static volatile long long neg = -111;

__attribute__((noinline)) static unsigned low(long long v)
{
    return (unsigned)v;
}

__attribute__((noinline)) static int to_int(double d)
{
    return (int)d;                  /* through a 64-bit conversion */
}

__attribute__((noinline)) static u64 wide64(long long v)
{
    unsigned u = (unsigned)v;
    return u;
}

#ifdef __SIZEOF_INT128__
typedef unsigned __int128 u128;

__attribute__((noinline)) static u128 wide128(long long v)
{
    unsigned u = (unsigned)v;
    return u;
}

__attribute__((noinline)) static u128 via_call(long long v)
{
    return low(v);
}

__attribute__((noinline)) static int fits(double d)
{
    unsigned l0 = (unsigned)to_int(d);
    return (u128)l0 == 0xFFFFFF91u;
}
#endif

int main(void)
{
    int ok = 0;
    ok += wide64(big) == 0x23456789u;
    ok += wide64(neg) == 0xFFFFFF91u;
#ifdef __SIZEOF_INT128__
    ok += wide128(big) == 0x23456789u;
    ok += wide128(neg) == 0xFFFFFF91u;
    ok += via_call(big) == 0x23456789u;
    ok += via_call(neg) == 0xFFFFFF91u;
    ok += fits(-111.0);
#else
    ok += 5;
#endif
    return ok == 7 ? 42 : ok;
}
