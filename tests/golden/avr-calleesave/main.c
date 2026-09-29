#include <stdint.h>
void writec(int c);
void puts_(const char *s);
typedef uint16_t u16;
typedef u16 (*fn_t)(u16);
u16 probe(fn_t f, u16 a);
extern volatile u16 probe_bad;
extern volatile unsigned char probe_ybad;
u16 leaf(u16), across(u16), loop(u16), pressure(u16), across32(u16),
    loop32(u16), manyargs(u16), relabel(u16);
static void hx(u16 v)
{
    for (int i = 12; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static fn_t fns[] = { leaf, across, loop, pressure, across32, loop32,
                      manyargs, relabel };
int main(void)
{
    for (unsigned k = 0; k < sizeof fns / sizeof fns[0]; k++)
        for (u16 a = 1; a < 3000; a += 1111) {
            u16 r = probe(fns[k], a);
            hx(r);
            if (probe_bad || probe_ybad) {
                puts_("CLOBBERED ");
                hx((u16)k); hx(probe_bad); hx(probe_ybad);
            }
        }
    puts_("\n==END==\n");
    return 0;
}
