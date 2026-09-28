/* The CALLER half of the hard-float cross test: calls each function in
 * callee.c with distinct values and prints what comes back, as hex. The
 * host computes the same line natively, so any argument the target put
 * somewhere its callee did not look shows up as a different hash. No
 * floating-point arithmetic here either -- see callee.c. */
typedef unsigned u32;
void writec(int c);
void puts_(const char *s);

struct f1 { float a; };
struct f2 { float a, b; };
struct f3 { float a, b, c; };
struct f4 { float a, b, c, d; };
struct d2 { double a, b; };
struct d4 { double a, b, c, d; };
struct f5 { float a, b, c, d, e; };
struct mix { float a; int b; };
struct i3 { int x[3]; };
union CF { float _Complex z; float p[2]; };
union CD { double _Complex z; double p[2]; };
union F { float f; u32 u; };
union D { double d; u32 w[2]; };

u32 c_backfill(float, double, float, float, double, float);
u32 c_many_f(float, float, float, float, float, float, float, float, float,
             float, float, float, float, float, float, float, float, float,
             float, float);
u32 c_many_d(double, double, double, double, double, double, double, double,
             double, double, int, float);
u32 c_mixed(int, float, int, double, int, float, long long, int);
u32 c_hfa(struct f1, struct f2, struct f3, struct d2, float);
u32 c_hfa_over(struct d4, struct d4, struct f3, float);
u32 c_hfa_backfill(float, struct d2, float, struct f2);
u32 c_close(float, struct d4, struct d4, float);
u32 c_nonhfa(struct f5, struct mix, float);
u32 c_nosplit(double, double, double, double, double, double, double, double,
              float, int, int, int, struct i3);
u32 c_cplx(float _Complex, double _Complex, float);
u32 c_va(int, ...);
u32 c_va_named(float, double, ...);
float r_f(float, float);
double r_d(double, double);
struct f1 r_f1(float);
struct f2 r_f2(float, float);
struct f3 r_f3(float, float, float);
struct f4 r_f4(float, float, float, float);
struct d2 r_d2(double, double);
struct d4 r_d4(double, double, double, double);
float _Complex r_cf(float, float);
double _Complex r_cd(double, double);
struct f5 r_f5(float, float);
struct mix r_mix(float, int);
double r_vad(int, ...);

static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static void pf(float f) { union F x; x.f = f; hx(x.u); }
static void pd(double d) { union D x; x.d = d; hx(x.w[0]); hx(x.w[1]); }

int main(void)
{
    struct f1 a1 = { 1.5f };
    struct f2 a2 = { 2.5f, -3.25f };
    struct f3 a3 = { 4.0f, 5.5f, -6.75f };
    struct d2 b2 = { 7.125, -8.0625 };
    struct d4 b4 = { 9.5, 10.25, -11.0, 12.75 };
    struct d4 c4 = { 13.5, -14.25, 15.0, 16.125 };
    struct f5 e5 = { 17.0f, 18.5f, 19.25f, -20.0f, 21.75f };
    struct mix m = { 22.5f, 23 };
    struct i3 s3 = { { 24, -25, 26 } };
    union CF cf;
    union CD cd;
    cf.p[0] = 27.5f; cf.p[1] = -28.25f;
    cd.p[0] = 29.125; cd.p[1] = 30.0625;

    hx(c_backfill(1.25f, 2.5, 3.75f, 4.125f, -5.5, 6.25f));
    hx(c_many_f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                17, 18, 19, 20));
    hx(c_many_d(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5, 77,
                -1.75f));
    hx(c_mixed(-1, 2.5f, 3, -4.75, 5, 6.125f, 0x123456789abcdefLL, -9));
    hx(c_hfa(a1, a2, a3, b2, 99.5f));
    hx(c_hfa_over(b4, c4, a3, -42.5f));
    hx(c_hfa_backfill(0.5f, b2, -0.25f, a2));
    hx(c_close(33.5f, b4, c4, -34.25f));
    hx(c_nonhfa(e5, m, 31.5f));
    hx(c_nosplit(1, 2, 3, 4, 5, 6, 7, 8, 9.75f, 10, 11, 12, s3));
    hx(c_cplx(cf.z, cd.z, 32.5f));
    hx(c_va(5, 1.5, 2, -3.25, 4, 5.125));
    hx(c_va_named(6.5f, 7.75, 8.875));
    puts_("\n");

    pf(r_f(1.0f, -2.5f));
    pd(r_d(3.0, -4.5));
    { struct f1 r = r_f1(5.25f); pf(r.a); }
    { struct f2 r = r_f2(6.5f, 7.75f); pf(r.a); pf(r.b); }
    { struct f3 r = r_f3(8.0f, 9.5f, 10.25f); pf(r.a); pf(r.b); pf(r.c); }
    { struct f4 r = r_f4(11.0f, 12.5f, 13.25f, 14.125f);
      pf(r.a); pf(r.b); pf(r.c); pf(r.d); }
    { struct d2 r = r_d2(15.5, 16.25); pd(r.a); pd(r.b); }
    { struct d4 r = r_d4(17.5, 18.25, 19.125, 20.0625);
      pd(r.a); pd(r.b); pd(r.c); pd(r.d); }
    { union CF r; r.z = r_cf(21.5f, 22.25f); pf(r.p[0]); pf(r.p[1]); }
    { union CD r; r.z = r_cd(23.5, 24.25); pd(r.p[0]); pd(r.p[1]); }
    { struct f5 r = r_f5(25.5f, 26.25f);
      pf(r.a); pf(r.b); pf(r.c); pf(r.d); pf(r.e); }
    { struct mix r = r_mix(27.5f, 28); pf(r.a); hx((u32)r.b); }
    pd(r_vad(3, 29.5, 30.25, 31.125));
    puts_("\n==END==\n");
    return 0;
}
