/* va_arg of a struct. Each ABI fetches one differently, and EmbCC
 * refused them all: SysV gathers INTEGER and SSE eightbytes from two
 * runs of the register save area (sixteen bytes apart for SSE), AAPCS64
 * an HFA from the v save area one member per register, a big struct is
 * MEMORY on x86-64 and a pointer on AAPCS64 and RISC-V, and a struct
 * aligned to sixteen takes an even register or an aligned stack slot.
 * Enough arguments are passed that the later ones overflow the save
 * areas onto the stack. agrees-with-gcc and regalloc-O2 run it against
 * gcc's whole program. */
// expect-exit: 42
#include <stdarg.h>
struct i2 { int a, b; };
struct mx { long l; double d; };
struct dd { double x, y; };
struct f3 { float x, y, z; };
struct c3 { char a, b, c; };
struct big { long a, b, c, d; };
struct al { long x; } __attribute__((aligned(16)));
static unsigned long hv(unsigned long h, long v) { return h * 31 + (unsigned long)v; }
__attribute__((noinline)) static unsigned long walk(int n, ...)
{
    va_list ap;
    unsigned long h = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        struct i2 a = va_arg(ap, struct i2);
        struct mx b = va_arg(ap, struct mx);
        struct dd c = va_arg(ap, struct dd);
        struct f3 d = va_arg(ap, struct f3);
        struct c3 e = va_arg(ap, struct c3);
        struct big f = va_arg(ap, struct big);
        struct al g = va_arg(ap, struct al);
        int k = va_arg(ap, int);
        h = hv(h, a.a); h = hv(h, a.b); h = hv(h, b.l); h = hv(h, (long)b.d);
        h = hv(h, (long)c.x); h = hv(h, (long)c.y);
        h = hv(h, (long)d.x); h = hv(h, (long)d.y); h = hv(h, (long)d.z);
        h = hv(h, e.a); h = hv(h, e.b); h = hv(h, e.c);
        h = hv(h, f.a); h = hv(h, f.b); h = hv(h, f.c); h = hv(h, f.d);
        h = hv(h, g.x); h = hv(h, k);
    }
    va_end(ap);
    return h;
}
int main(void)
{
    struct i2 a = { 1, 2 }; struct mx b = { 3, 4 }; struct dd c = { 5, 6 };
    struct f3 d = { 7, 8, 9 }; struct c3 e = { 10, 11, 12 };
    struct big f = { 13, 14, 15, 16 }; struct al g = { 17 };
    struct i2 a2 = { 21, 22 }; struct mx b2 = { 23, 24 }; struct dd c2 = { 25, 26 };
    struct f3 d2 = { 27, 28, 29 }; struct c3 e2 = { 30, 31, 32 };
    struct big f2 = { 33, 34, 35, 36 }; struct al g2 = { 37 };
    unsigned long want = 0;
    for (int v = 1; v <= 18; v++)
        want = want * 31 + (unsigned long)v;
    for (int v = 21; v <= 38; v++)
        want = want * 31 + (unsigned long)v;
    unsigned long got = walk(2, a, b, c, d, e, f, g, 18, a2, b2, c2, d2, e2, f2, g2, 38);
    return got == want ? 42 : 1;
}
