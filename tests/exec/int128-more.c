/* __int128 beyond arithmetic: va_arg (x86-64: two save-area slots while
 * two registers are left, else a 16-aligned overflow slot; aarch64: an even
 * register pair, else a 16-aligned stack slot), switch on one, bit-fields
 * of it (read, written, compound-assigned, statically initialized — its
 * 16-byte storage unit shared with neighbours), increments and pointer
 * arithmetic, && ?: ! on one, stack arguments past the registers, and the
 * compound assignments. Expected values from gcc and Python (the same). */
// expect-exit: 42
#include <stdarg.h>
typedef __int128 i128;
typedef unsigned __int128 u128;
static i128 va(int n, ...)
{
    va_list ap; va_start(ap, n);
    i128 s = 0;
    for (int k = 0; k < n; k++) {
        int w = va_arg(ap, int);
        s += va_arg(ap, i128) * w;
    }
    va_end(ap);
    return s;
}
static int sw(i128 v) { switch (v) { case 3: return 1; case -7: return 2; default: return 3; } }
struct B { int a; i128 x : 3; unsigned __int128 y : 70; };
static struct B sb = { 9, -3, ((u128)3 << 68) | 1 };
struct C { i128 lo : 60; i128 mid : 60; unsigned char tail; i128 full : 128; };
static struct C sc = { -1, 0x123456789abcdefL, 7, -((i128)1 << 120) };
static i128 many(int a, int b, int c, int d, int e, i128 f, i128 g) { return f * 10 + g + a + b + c + d + e; }
int main(void)
{
    i128 big = (i128)1 << 100;
    if (va(3, 1, big, 2, (i128)-5, 3, (i128)7) != big - 10 + 21) return 1;
    if (va(5, 1, (i128)1, 1, (i128)2, 1, (i128)3, 1, (i128)4, 1, big) != big + 10) return 2;
    if (sw(3) != 1 || sw(-7) != 2 || sw(big + 3) != 3) return 3;
    struct B b = { 1, -2, ((u128)1 << 69) | 5 };
    if (b.x != -2 || b.y != (((u128)1 << 69) | 5)) return 4;
    b.x += 1; b.y >>= 1;
    if (b.x != -1 || b.y != (((u128)1 << 68) | 2)) return 5;
    if (sb.a != 9 || sb.x != -3 || sb.y != (((u128)3 << 68) | 1)) return 20;
    if (sc.lo != -1 || sc.mid != 0x123456789abcdefL || sc.tail != 7 ||
        sc.full != -((i128)1 << 120)) return 21;
    sc.mid = -5; sc.full += 1;
    if (sc.lo != -1 || sc.mid != -5 || sc.tail != 7 ||
        sc.full != -((i128)1 << 120) + 1 || sizeof sc != 32) return 22;
    i128 arr[3] = { 5, big, -1 };
    i128 *p = arr;
    if (++*p != 6 || p[1] - p[0] != big - 6 || (int)(&arr[2] - p) != 2) return 6;
    if (!(big && arr[2]) || !big || (big ? 0 : 1) || (u128)arr[2] >> 127 != 1) return 7;
    if (many(1, 2, 3, 4, 5, big, -big) != big * 9 + 15) return 8;
    u128 x = 0; x--; x /= 3; x %= 1000003;
    i128 y = big; y *= -3; y /= 7; y <<= 2; y >>= 1; y |= 1; y &= ~(i128)2; y ^= 8;
    if ((unsigned long)x != 0xa301d || (unsigned long)((u128)y >> 64) !=
        0xfffffff249249249UL || (unsigned long)y != 0x249249249249249dUL)
        return 9;
    return 42;
}
