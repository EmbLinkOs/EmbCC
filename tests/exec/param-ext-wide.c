/* A char or short parameter widened to 64 bits. On RISC-V and Xtensa the
 * caller extends such an argument to a register (target_args_extended), so
 * extending it again to 32 bits is a copy -- but only to 32: as a long long
 * on a 32-bit target it still needs a high word. `(u64)(short)s` -- the
 * cast to the parameter's own type, then the widening -- was folded into a
 * copy as well, and the call took whatever the register beside the
 * parameter held as its high word (fuzz seed 1064066492, RV32 at -O1 and
 * up). Here that register holds the second argument, so a missing high
 * word is a wrong one. */
// expect-exit: 42
typedef unsigned long long u64;
typedef long long i64;

__attribute__((noinline)) static u64 keep(u64 v) { return v; }

__attribute__((noinline)) u64 from_short(short s, int junk)
{
    (void)junk;
    return keep((u64)(short)s);
}
__attribute__((noinline)) u64 from_schar(signed char c, int junk)
{
    (void)junk;
    return keep((u64)(signed char)c);
}
__attribute__((noinline)) u64 from_uchar(unsigned char c, int junk)
{
    (void)junk;
    return keep((u64)(unsigned char)c);
}
__attribute__((noinline)) u64 from_ushort(unsigned short s, int junk)
{
    (void)junk;
    return keep((u64)(unsigned short)s);
}
/* the same parameter at 32 bits, where the copy is right, and at 64 */
__attribute__((noinline)) i64 both(short s, int junk)
{
    int narrow = (short)s;
    (void)junk;
    return (i64)keep((u64)(i64)(short)s) + narrow;
}

int main(void)
{
    int junk = 0x5a5a5a5a;
    if (from_short(-5, junk) != (u64)-5LL)
        return 1;
    if (from_short(32767, junk) != 32767ULL)
        return 2;
    if (from_schar(-128, junk) != (u64)-128LL)
        return 3;
    if (from_uchar(200, junk) != 200ULL)
        return 4;
    if (from_ushort(65535, junk) != 65535ULL)
        return 5;
    if (both(-3, junk) != -6LL)
        return 6;
    return 42;
}
