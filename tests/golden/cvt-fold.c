/* Conversions of constants, folded by the optimizer, against the same
 * conversions done at run time and against the host (cvt-fold.sh).
 *
 * The optimizer computes a constant's conversion with the host's own
 * arithmetic; everything here is a case where that could go wrong: a
 * 64-bit integer to float, which must round ONCE (through double first
 * rounds twice); the edges of each integer type; truncation of negatives;
 * double to float at a tie, at a subnormal and past FLT_MAX. Each line is
 * computed from a literal -- folded -- and from a volatile copy of it --
 * converted at run time -- and the two must agree with each other and with
 * the host. Out-of-range float-to-int is undefined in C and is not here. */
#include <stdint.h>
void writec(int c);
void puts_(const char *s);
typedef uint32_t u32;
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static u32 fb(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }
static void db(double d)
{
    union { double d; u32 u[2]; } x;
    if (sizeof(double) == 4) { hx(fb((float)d)); return; }   /* AVR */
    x.d = d; hx(x.u[0]); hx(x.u[1]);
}
static u32 bad;
#define CHK(folded, runtime) do { if ((folded) != (runtime)) bad++; } while (0)

int main(void)
{
    /* int -> float / double */
    /* just above a halfway point between two floats: rounding to double
     * first lands exactly ON the halfway point and then goes to even --
     * the wrong way. 2^62 + 2^38 + 1 and 2^63 + 2^39 + 1. */
    volatile int64_t v64 = 0x4000004000000001LL;
    volatile uint64_t vu64 = 0x8000008000000001ULL;
    volatile int32_t vi32 = -2147483647 - 1;
    volatile uint32_t vu32 = 0xffffffffu;
    volatile int64_t vbig = 9007199254740993LL;      /* 2^53 + 1 */
    hx(fb((float)0x4000004000000001LL)); CHK(fb((float)0x4000004000000001LL), fb((float)v64));
    hx(fb((float)0x8000008000000001ULL)); CHK(fb((float)0x8000008000000001ULL), fb((float)vu64));
    hx(fb((float)(-2147483647 - 1)));   CHK(fb((float)(-2147483647 - 1)), fb((float)vi32));
    hx(fb((float)0xffffffffu));         CHK(fb((float)0xffffffffu), fb((float)vu32));
    hx(fb((float)16777217));            CHK(fb((float)16777217), fb((float)(vi32 * 0 + 16777217)));
    db((double)9007199254740993LL);
    db((double)vbig);
    db((double)0xffffffffu);
    db((double)vu32);
    puts_("\n");
    /* float / double -> int, truncating toward zero */
    volatile double vd = -2.9999, vd2 = 4294967295.75, vnz = -0.0;
    volatile float vf = -7.5f, vf2 = 16777215.5f;
    hx((u32)(int)-2.9999);              CHK((int)-2.9999, (int)vd);
    hx((u32)4294967295.75);             CHK((u32)4294967295.75, (u32)vd2);
    hx((u32)(int)-0.0);                 CHK((int)-0.0, (int)vnz);
    hx((u32)(int)-7.5f);                CHK((int)-7.5f, (int)vf);
    hx((u32)16777215.5f);               CHK((u32)16777215.5f, (u32)vf2);
    hx((u32)(signed char)-100.9);
    hx((u32)(unsigned short)65535.99);
    puts_("\n");
    /* double <-> float */
    volatile double vt = 1.0000000596046448;   /* a tie: 1 + 2^-24 */
    volatile double vs = 1.0e-40, vo = 1.0e39, vm = -3.4028235677973366e38;
    hx(fb((float)1.0000000596046448));  CHK(fb((float)1.0000000596046448), fb((float)vt));
    hx(fb((float)1.0e-40));             CHK(fb((float)1.0e-40), fb((float)vs));
    hx(fb((float)1.0e39));              CHK(fb((float)1.0e39), fb((float)vo));
    hx(fb((float)-3.4028235677973366e38)); CHK(fb((float)-3.4028235677973366e38), fb((float)vm));
    db((double)0.1f);
    db((double)-0.0f);
    hx(bad);
    puts_("\n==END==\n");
    return 0;
}
