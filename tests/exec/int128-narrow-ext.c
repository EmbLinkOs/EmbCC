// expect-exit: 42
/* A narrow value read out of an __int128 and widened again: `(int)x`
 * compared with an __int128, `(signed char)x` returned as one. Once the
 * optimizer propagates copies the narrowing is no instruction, and the
 * extension back to 128 bits reads the sixteen-byte value itself --
 * ext.16:4s of it. x86-64 and AArch64 took all eight low bytes there and
 * ignored the four and the sign, so (int)0x80000000 came back positive
 * and INT_MIN compared greater than 0. -O0 kept the copy and was right.
 * (Narrowing an out-of-range value is implementation-defined; GCC, Clang
 * and EmbCC all take the low bytes.) */
typedef __int128 i128;
typedef unsigned __int128 u128;

static u128 la[4];

__attribute__((noinline)) static int gt(int k, i128 g)
{
    return (int)la[k & 3] > g;
}
__attribute__((noinline)) static i128 sx(int k)
{
    return (signed char)la[k & 3];
}
__attribute__((noinline)) static u128 zx(int k)
{
    return (unsigned short)la[k & 3];
}
__attribute__((noinline)) static i128 sx32(int k)
{
    return (int)la[k & 3];
}

int main(void)
{
    la[0] = 0x80000000u;                       /* (int) is INT_MIN */
    la[1] = ((u128)1 << 64) | 0xfffff0ffu;     /* (signed char) -1, (ushort) 0xf0ff */
    if (gt(0, 0)) return 1;
    if (!gt(1, -4000)) return 2;               /* (int) is -3841 */
    if (sx(1) != -1) return 3;
    if (zx(1) != 0xf0ff) return 4;
    if (sx32(0) != -2147483647 - 1) return 5;
    return 42;
}
