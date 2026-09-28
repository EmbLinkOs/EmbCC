/* long double where it has no format of its own: on ARM EABI it IS a
 * double, and on AVR it is a float (tests/golden/ldouble-same.sh). It must
 * compute exactly as that type -- so the host reference, whose long double
 * is wider, computes with the target's type in its place -- while staying
 * a distinct C type. */
void writec(int c);
void puts_(const char *s);
#include <stdint.h>
typedef uint32_t u32;                    /* 32 bits on host, ARM and AVR */
#if defined(__arm__) || defined(__AVR__)
typedef long double LD;                  /* the type under test */
#define LDSUF(x) x##L
#else
#if defined(EMBCC_HOST_AS_FLOAT)
typedef float LD;
#else
typedef double LD;
#endif
#define LDSUF(x) x
#endif
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
/* its bits, as the type it shares a format with */
static void pld(LD x)
{
    if (sizeof x == 4) {
        union { LD d; u32 u; } v; v.d = x; hx(v.u);
    } else {
        union { LD d; u32 u[2]; } v; v.d = x; hx(v.u[0]); hx(v.u[1]);
    }
}
__attribute__((noinline)) LD mul_add(LD a, LD b, LD c) { return a * b + c; }
__attribute__((noinline)) LD mix(float f, LD a, double d, int k)
{ return (LD)f * a - (LD)d / (LD)k; }
static LD (*volatile p_mul_add)(LD, LD, LD) = mul_add;
volatile LD va = LDSUF(1.5), vb = LDSUF(0.1);

int main(void)
{
    LD folded = LDSUF(1.0) / LDSUF(3.0) + LDSUF(0.2);    /* folded by sema */
    pld(folded);
    pld(va * vb + LDSUF(2.25));
    pld(p_mul_add(va, vb, LDSUF(-7.125)));
    pld(mix(0.3f, va, 2.5, 7));
    pld(-va);
    hx((u32)(va * LDSUF(100.0)));
    hx((u32)(int)(-vb * LDSUF(1000.0)));
    hx(va > vb);
    hx(sizeof(LD) == sizeof(double) || sizeof(LD) == sizeof(float));
#if defined(__arm__) || defined(__AVR__)
    /* still its own type */
    hx(_Generic(va, long double: 1, double: 2, float: 3, default: 4));
#else
    hx(1);
#endif
    puts_("\n==END==\n");
    return 0;
}
