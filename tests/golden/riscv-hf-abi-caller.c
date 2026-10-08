/* The caller half of tests/golden/riscv-hf-abi.sh. See riscv-hf-abi.h. */
#include "riscv-hf-abi.h"
extern void writec(int c); extern void puts_(const char *s);
extern void putn(long v);

static void hx(unsigned long long v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 15)]);
    writec(' ');
}
static void pf(float f)
{ union { float f; unsigned u; } x; x.f = f; hx(x.u); }
static void pd(double d)
{ union { double d; unsigned long long u; } x; x.d = d; hx(x.u); }
static void nl(void) { writec('\n'); }

int main(void)
{
    static int seven = 7;
    s_scalars(1.5f, 2, -3.25f, 4.125, 5, -6.0625);
    s_tenf(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    s_tend(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5, 12);
    {
        ff a = { 1.25f, -2.5f }; fi b = { 3.75f, -4 }; iff c = { 5, 6.5f };
        cd d = { 'x', 7.125 }; fa2 e = { { 8.5f, 9.25f } };
        nest f = { { 10.75f }, 11.5f }; f1 g = { 12.25f };
        s_structs(a, b, c, d, e, f, g);
    }
    {
        f3 a = { 1, 2, 3 }; uf b; fp_ c = { 4.5f, &seven };
        df d = { 5.25, 6.75f }; dl e = { 7.5, 0x123456789LL };
        sd f = { -9, 10.5 };
        b.i = 0x40490fdb;
        s_noflat(a, b, c, d, e, f);
    }
    {
        dd a = { 1.0 / 3.0, -2.0 }, b = { 1e300, -1e-300 };
        s_wide(a, 42, b);
    }
    {
        fz a = { 1.5f }; fzf b = { 2.5f, 3.5f }; fbf c = { 4.5f, -3 };
        s_bits(a, b, c);
    }
    {
        float _Complex a; double _Complex b;
        __real__ a = 1.5f; __imag__ a = -2.5f;
        __real__ b = 3.25; __imag__ b = -4.75;
        s_cplx(a, b, 99);
    }
    {
        ff h = { 8.5f, 9.5f };
        s_lastf(1, 2, 3, 4, 5, 6, 7, h, 10.5f);
    }
    {
        fi s = { 11.25f, 12 };
        s_lastx(1, 2, 3, 4, 5, 6, 7, 8, s, 13.5f);
    }
    s_var(5, 1.5, 2, -3.25, 4, (double)5.5f);

    pf(r_float(1.25f, -0.5f)); pd(r_double(2.5, 3)); nl();
    { ff r = r_ff(3.5f); pf(r.x); pf(r.y); }
    { dd r = r_dd(10.0); pd(r.x); pd(r.y); }
    { fi r = r_fi(7); pf(r.f); putn(r.i); }
    { iff r = r_iff(9); putn(r.i); pf(r.f); }
    nl();
    { f3 r = r_f3(4); pf(r.x); pf(r.y); pf(r.z); }
    { float _Complex r = r_cf(2.5f); pf(__real__ r); pf(__imag__ r); }
    { double _Complex r = r_cd(6.0); pd(__real__ r); pd(__imag__ r); }
    { cd r = r_cd2(17); putn(r.c); pd(r.d); }
    pf(r_ptr()(1.5f, 0.25f));
    nl();
    puts_("==END==\n");
    return 0;
}
