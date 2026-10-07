#include "rx-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
int main(void)
{
    struct c6 c = { { 7, 1, 1, 1, 1, 3 } };
    struct s12 t = { 1, 2, 3 };
    putn(after_c6(c, 5));
    { long long r = after3(1, 2, 3, 4000000000LL, 5);
      putn((long)(unsigned)r); putn((long)(unsigned)(r >> 32)); }
    putn(s12_regs(9, t)); putn(s12_stk(1, 2, t, 4));
    putn(narrow(-3, 200, -300, 60000, -100, -20000, 250));
    putn(ret_sc(127)); putn(ret_us(30000)); writec('\n');
    { struct s16 r = ret16(5); putn(r.a + r.b * 10 + r.c * 100 + r.d * 1000); }
    { struct c6 r = ret6(11); putn(r.v[0] * 10 + r.v[5]); }
    { struct s4c r = ret4c(6); putn(r.a + r.b * 10 + r.c * 100 + r.d * 1000); }
    { _Complex float z = retcf(1.5f, -2.25f); putn((long)(__real__ z * 100)); putn((long)(__imag__ z * 100)); }
    { long long r = ret64(5000000000LL, 7);
      putn((long)(unsigned)r); putn((long)(unsigned)(r >> 32)); }
    putn((long)(dmul(1.5, 2.0f, 0.25) * 1000)); writec('\n');
    putn(vlast(1, 2, 3, 4));
    putn((long)(vdbl(3, 0.5, 1.25, 2.0) * 100));
    { struct c6 d = { { 2, 0, 0, 0, 0, 9 } }; putn(vstruct(2, c, 40, d, 50)); }
    { struct al16 s = { 6 }; putn(stk_al16(1, 2, 3, 4, s, 5)); }
    { struct msbf s = { 3, -2 }; putn(bf_byval(4, s)); }
    puts_("\n==END==\n");
    return 0;
}
