/* The callee half of tests/golden/riscv-hf-abi.sh: each function prints
 * the bits it received, and the r_ ones return values built from their
 * arguments. See riscv-hf-abi.h. */
#include <stdarg.h>
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

void s_scalars(float a, int b, float c, double d, int e, double f)
{ pf(a); putn(b); pf(c); pd(d); putn(e); pd(f); nl(); }

void s_tenf(float a, float b, float c, float d, float e, float f, float g,
            float h, float i, float j, int k)
{ pf(a); pf(b); pf(c); pf(d); pf(e); pf(f); pf(g); pf(h); pf(i); pf(j);
  putn(k); nl(); }

void s_tend(double a, double b, double c, double d, double e, double f,
            double g, double h, double i, double j, int k)
{ pd(a); pd(b); pd(c); pd(d); pd(e); pd(f); pd(g); pd(h); pd(i); pd(j);
  putn(k); nl(); }

void s_structs(ff a, fi b, iff c, cd d, fa2 e, nest f, f1 g)
{ pf(a.x); pf(a.y); pf(b.f); putn(b.i); putn(c.i); pf(c.f);
  putn(d.c); pd(d.d); pf(e.a[0]); pf(e.a[1]); pf(f.in.x); pf(f.y);
  pf(g.f); nl(); }

void s_noflat(f3 a, uf b, fp_ c, df d, dl e, sd f)
{ pf(a.x); pf(a.y); pf(a.z); putn(b.i); pf(c.f); putn(*c.p);
  pd(d.d); pf(d.f); pd(e.d); putn((long)e.l); putn((long)(e.l >> 32));
  putn(f.s); pd(f.d); nl(); }

void s_wide(dd a, int k, dd b)
{ pd(a.x); pd(a.y); putn(k); pd(b.x); pd(b.y); nl(); }

void s_bits(fz a, fzf b, fbf c)
{ pf(a.f); pf(b.f); pf(b.g); pf(c.f); putn(c.bf); nl(); }

void s_cplx(float _Complex a, double _Complex b, int k)
{ pf(__real__ a); pf(__imag__ a); pd(__real__ b); pd(__imag__ b);
  putn(k); nl(); }

void s_lastf(float a, float b, float c, float d, float e, float f, float g,
             ff h, float i)
{ pf(a); pf(b); pf(c); pf(d); pf(e); pf(f); pf(g); pf(h.x); pf(h.y);
  pf(i); nl(); }

void s_lastx(int a, int b, int c, int d, int e, int f, int g, int h,
             fi s, float x)
{ putn(a + b + c + d + e + f + g + h); pf(s.f); putn(s.i); pf(x); nl(); }

void s_var(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int k = 0; k < n; k++) {
        if (k & 1) putn(va_arg(ap, int));
        else       pd(va_arg(ap, double));
    }
    va_end(ap);
    nl();
}

float r_float(float a, float b) { return a * 2.0f + b; }
double r_double(double a, int b) { return a * (double)b; }
ff r_ff(float a) { ff r; r.x = a; r.y = -a; return r; }
dd r_dd(double a) { dd r; r.x = a / 4.0; r.y = a * 3.0; return r; }
fi r_fi(int k) { fi r; r.f = (float)k + 0.5f; r.i = -k; return r; }
iff r_iff(int k) { iff r; r.i = k * 3; r.f = (float)k * 0.25f; return r; }
f3 r_f3(int k) { f3 r; r.x = (float)k; r.y = r.x + 1; r.z = r.y + 1; return r; }
float _Complex r_cf(float a)
{ float _Complex r; __real__ r = a; __imag__ r = a + 1.0f; return r; }
double _Complex r_cd(double a)
{ double _Complex r; __real__ r = -a; __imag__ r = a * 0.5; return r; }
cd r_cd2(int k) { cd r; r.c = (char)k; r.d = (double)k / 8.0; return r; }

double r_d3(double a, double b, double c) { return a * 100.0 + b * 10.0 + c; }

void s_fvar(float f, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    pf(f);
    while (n--)
        pd(va_arg(ap, double));
    va_end(ap);
    nl();
}

static float twice_plus(float a, float b) { return a + a + b; }
fn_ff r_ptr(void) { return twice_plus; }
