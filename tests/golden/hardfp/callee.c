/* The CALLEE half of the hard-float cross test (tests/golden/thumb-hardfp.sh).
 *
 * Every function here hashes the BIT PATTERN of each argument it was given,
 * in order, or hands its arguments straight back in a different order. No
 * floating-point arithmetic, on purpose: this file is compiled by clang as
 * well as by EmbCC, and what is under test is only WHERE each value travels
 * -- s0-s15, d0-d7, r0-r3 or the stack -- not how anything computes. A value
 * read from the wrong register changes the hash; so does one read from the
 * right register of the wrong width. */
#include <stdarg.h>

typedef unsigned u32;
union F { float f; u32 u; };
union D { double d; u32 w[2]; };
static u32 H(u32 h, u32 v) { return (h ^ v) * 16777619u + 0x9e3779b9u; }
static u32 hf(u32 h, float f) { union F x; x.f = f; return H(h, x.u); }
static u32 hd(u32 h, double d)
{
    union D x;
    x.d = d;
    return H(H(h, x.w[0]), x.w[1]);
}

struct f1 { float a; };
struct f2 { float a, b; };
struct f3 { float a, b, c; };
struct f4 { float a, b, c, d; };
struct d2 { double a, b; };
struct d4 { double a, b, c, d; };
struct f5 { float a, b, c, d, e; };      /* five members: NOT homogeneous */
struct mix { float a; int b; };          /* two kinds: NOT homogeneous */
struct i3 { int x[3]; };
union CF { float _Complex z; float p[2]; };
union CD { double _Complex z; double p[2]; };

/* s0, d1, s1, s4, d3, s5: the double leaves s1 free and a float fills it */
u32 c_backfill(float a, double b, float c, float d, double e, float f)
{
    u32 h = 1;
    h = hf(h, a); h = hd(h, b); h = hf(h, c);
    h = hf(h, d); h = hd(h, e); h = hf(h, f);
    return h;
}

/* sixteen in s0-s15 and four on the stack */
u32 c_many_f(float a0, float a1, float a2, float a3, float a4, float a5,
             float a6, float a7, float a8, float a9, float a10, float a11,
             float a12, float a13, float a14, float a15, float a16,
             float a17, float a18, float a19)
{
    u32 h = 2;
    h = hf(h, a0); h = hf(h, a1); h = hf(h, a2); h = hf(h, a3);
    h = hf(h, a4); h = hf(h, a5); h = hf(h, a6); h = hf(h, a7);
    h = hf(h, a8); h = hf(h, a9); h = hf(h, a10); h = hf(h, a11);
    h = hf(h, a12); h = hf(h, a13); h = hf(h, a14); h = hf(h, a15);
    h = hf(h, a16); h = hf(h, a17); h = hf(h, a18); h = hf(h, a19);
    return h;
}

/* d0-d7 full, two doubles on the stack; the int still gets r0, and the
 * float cannot go back to a VFP register (they are closed) */
u32 c_many_d(double a0, double a1, double a2, double a3, double a4,
             double a5, double a6, double a7, double a8, double a9,
             int i, float f)
{
    u32 h = 3;
    h = hd(h, a0); h = hd(h, a1); h = hd(h, a2); h = hd(h, a3);
    h = hd(h, a4); h = hd(h, a5); h = hd(h, a6); h = hd(h, a7);
    h = hd(h, a8); h = hd(h, a9); h = H(h, (u32)i); h = hf(h, f);
    return h;
}

/* the two files interleaved: ints walk r0-r3 and the stack, floats walk
 * the VFP registers, independently */
u32 c_mixed(int a, float b, int c, double d, int e, float f, long long g,
            int k)
{
    u32 h = 4;
    h = H(h, (u32)a); h = hf(h, b); h = H(h, (u32)c); h = hd(h, d);
    h = H(h, (u32)e); h = hf(h, f);
    h = H(h, (u32)g); h = H(h, (u32)(g >> 32)); h = H(h, (u32)k);
    return h;
}

/* aggregates: s0 | s1-s2 | s3-s5 | d3-d4 | s10 */
u32 c_hfa(struct f1 p, struct f2 q, struct f3 r, struct d2 s, float t)
{
    u32 h = 5;
    h = hf(h, p.a);
    h = hf(h, q.a); h = hf(h, q.b);
    h = hf(h, r.a); h = hf(h, r.b); h = hf(h, r.c);
    h = hd(h, s.a); h = hd(h, s.b);
    h = hf(h, t);
    return h;
}

/* d0-d3, d4-d7, and the third does not fit: to the stack, and the float
 * after it may NOT back-fill -- every VFP register is now unavailable */
u32 c_hfa_over(struct d4 a, struct d4 b, struct f3 c, float d)
{
    u32 h = 6;
    h = hd(h, a.a); h = hd(h, a.b); h = hd(h, a.c); h = hd(h, a.d);
    h = hd(h, b.a); h = hd(h, b.b); h = hd(h, b.c); h = hd(h, b.d);
    h = hf(h, c.a); h = hf(h, c.b); h = hf(h, c.c);
    h = hf(h, d);
    return h;
}

/* s0 | d1-d4, then the second d4 needs d5-d8 and there is no d8: it goes
 * to the stack and CLOSES the VFP registers, so the last float goes to
 * the stack too although s1 is still free. Without that rule it would
 * back-fill s1, which is the one case where the difference shows. */
u32 c_close(float a, struct d4 b, struct d4 c, float e)
{
    u32 h = 13;
    h = hf(h, a);
    h = hd(h, b.a); h = hd(h, b.b); h = hd(h, b.c); h = hd(h, b.d);
    h = hd(h, c.a); h = hd(h, c.b); h = hd(h, c.c); h = hd(h, c.d);
    h = hf(h, e);
    return h;
}

/* s0 | d1-d2 | s1 | s6-s7 */
u32 c_hfa_backfill(float a, struct d2 b, float c, struct f2 d)
{
    u32 h = 7;
    h = hf(h, a); h = hd(h, b.a); h = hd(h, b.b); h = hf(h, c);
    h = hf(h, d.a); h = hf(h, d.b);
    return h;
}

/* not homogeneous, so core registers: f5 splits r0-r3 + stack, mix goes
 * to the stack, and the float is still s0 */
u32 c_nonhfa(struct f5 a, struct mix b, float c)
{
    u32 h = 8;
    h = hf(h, a.a); h = hf(h, a.b); h = hf(h, a.c); h = hf(h, a.d);
    h = hf(h, a.e);
    h = hf(h, b.a); h = H(h, (u32)b.b);
    h = hf(h, c);
    return h;
}

/* C.5: the float overflows to the stack first, so the struct may NOT be
 * split between r3 and the stack -- all of it goes to memory */
u32 c_nosplit(double a0, double a1, double a2, double a3, double a4,
              double a5, double a6, double a7, float f, int i, int j,
              int k, struct i3 s)
{
    u32 h = 9;
    h = hd(h, a0); h = hd(h, a1); h = hd(h, a2); h = hd(h, a3);
    h = hd(h, a4); h = hd(h, a5); h = hd(h, a6); h = hd(h, a7);
    h = hf(h, f); h = H(h, (u32)i); h = H(h, (u32)j); h = H(h, (u32)k);
    h = H(h, (u32)s.x[0]); h = H(h, (u32)s.x[1]); h = H(h, (u32)s.x[2]);
    return h;
}

/* a complex number is a two-element homogeneous aggregate: s0-s1 | d1-d2 | s6 */
u32 c_cplx(float _Complex a, double _Complex b, float c)
{
    union CF x;
    union CD y;
    u32 h = 10;
    x.z = a;
    y.z = b;
    h = hf(h, x.p[0]); h = hf(h, x.p[1]);
    h = hd(h, y.p[0]); h = hd(h, y.p[1]);
    h = hf(h, c);
    return h;
}

/* variadic: the BASE convention for everything, named arguments too */
u32 c_va(int n, ...)
{
    va_list ap;
    u32 h = 11;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        if (i & 1) h = H(h, (u32)va_arg(ap, int));
        else       h = hd(h, va_arg(ap, double));
    }
    va_end(ap);
    return h;
}
u32 c_va_named(float a, double b, ...)
{
    va_list ap;
    u32 h = 12;
    h = hf(h, a); h = hd(h, b);
    va_start(ap, b);
    h = hd(h, va_arg(ap, double));
    va_end(ap);
    return h;
}

/* results: s0, d0, s0-s3, d0-d3, and memory for what is not homogeneous */
float r_f(float a, float b) { (void)a; return b; }
double r_d(double a, double b) { (void)a; return b; }
struct f1 r_f1(float a) { struct f1 r; r.a = a; return r; }
struct f2 r_f2(float a, float b) { struct f2 r; r.a = b; r.b = a; return r; }
struct f3 r_f3(float a, float b, float c)
{ struct f3 r; r.a = c; r.b = a; r.c = b; return r; }
struct f4 r_f4(float a, float b, float c, float d)
{ struct f4 r; r.a = d; r.b = c; r.c = b; r.d = a; return r; }
struct d2 r_d2(double a, double b) { struct d2 r; r.a = b; r.b = a; return r; }
struct d4 r_d4(double a, double b, double c, double d)
{ struct d4 r; r.a = d; r.b = c; r.c = b; r.d = a; return r; }
float _Complex r_cf(float a, float b)
{ union CF x; x.p[0] = b; x.p[1] = a; return x.z; }
double _Complex r_cd(double a, double b)
{ union CD x; x.p[0] = b; x.p[1] = a; return x.z; }
struct f5 r_f5(float a, float b)
{ struct f5 r; r.a = b; r.b = a; r.c = b; r.d = a; r.e = b; return r; }
struct mix r_mix(float a, int b) { struct mix r; r.a = a; r.b = b; return r; }
double r_vad(int n, ...)
{
    va_list ap;
    double d;
    va_start(ap, n);
    d = va_arg(ap, double);
    while (--n > 0)
        d = va_arg(ap, double);
    va_end(ap);
    return d;
}
