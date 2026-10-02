/* An AND whose constant reads only bits the other operand can never set
 * folds to zero: after a shift, a multiply by an even constant, a mask, a
 * zero-extension, an or or xor of such. Beside each foldable case sits
 * the nearest one that is NOT -- the bit just inside the shift, an
 * arithmetic shift, a sign-extension, a multiple of 4 tested at 4, an add
 * in the chain -- with the value that would show an over-eager fold. */
// expect-exit: 42
#define NI __attribute__((noinline))
NI long f1(long x) { return (x * 2) & 1; }
NI int f2(int x) { return (x << 3) & 7; }
NI int f3(int x) { return (x << 3) & 8; }                       /* not: 8 at x = 1 */
NI unsigned f4(unsigned x) { return (x >> 4) & 0xF0000000u; }
NI int f5(int x) { return (x >> 4) & (int)0xF0000000; }         /* not: arithmetic */
NI int f6(unsigned char c) { return c & 0x100; }
NI int f7(signed char c) { return c & 0x100; }                  /* not: sign-extended */
NI long f8(long x) { return ((x & 0xF0) | 0x0F) & 0x100; }
NI long f9(long x) { return (x * 12) & 3; }
NI long f10(long x) { return (x * 12) & 4; }                    /* not: 4 at x = 1 */
NI int f11(int x) { return ((x << 1) ^ (x << 2)) & 1; }
NI int f12(int x) { return (((x << 1) & 0xFF) + 1) & 1; }       /* not: the add */
NI unsigned f13(unsigned x) { return ((x >> 1) << 1) & 1; }
NI long long f14(long long x) { return (x << 32) & 0xFFFFFFFFLL; }   /* 64-bit on every target */
NI int f15(int x) { return (x << 31) & 0x7FFFFFFF; }
NI int f16(int x) { return (x << 30) & 0x40000000; }            /* not: 2^30 at x = 1 */
int main(void)
{
    int bad = 0;
    static const long xs[] = { 0, 1, 2, 3, 5, -1, -7, 255, 256, 0x7fffffff, -2147483647 - 1, 123456789 };
    for (int k = 0; k < 12; k++) {
        long x = xs[k]; int xi = (int)x; unsigned xu = (unsigned)x;
        if (f1(x) != 0 || f2(xi) != 0 || f4(xu) != 0 || f8(x) != 0 || f9(x) != 0 ||
            f11(xi) != 0 || f13(xu) != 0 || f14(x) != 0 || f15(xi) != 0) bad |= 1;
        if (f3(xi) != ((xi << 3) & 8)) bad |= 2;
        if (f5(xi) != ((xi >> 4) & (int)0xF0000000)) bad |= 4;
        if (f10(x) != ((x * 12) & 4)) bad |= 8;
        if (f12(xi) != 1) bad |= 16;
        if (f16(xi) != ((xi << 30) & 0x40000000)) bad |= 32;
    }
    if (f3(1) != 8 || f5(-1) != (int)0xF0000000 || f10(1) != 4 || f16(1) != 0x40000000) bad |= 64;
    if (f6(0xFF) != 0 || f6(0) != 0 || f7(-1) != 0x100 || f7(127) != 0 || f7(-128) != 0x100) bad |= 128;
    if (bad) return bad;
    return 42;
}
