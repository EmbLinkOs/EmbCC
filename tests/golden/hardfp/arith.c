/* Floating-point ARITHMETIC across real call boundaries under the
 * hard-float convention: the values arrive in s/d registers, a float is
 * computed on the FPU, a double goes to the runtime -- which takes it in
 * CORE registers, because the runtime keeps the base convention -- and
 * the result goes home in s0/d0. Every step of that is a register-file
 * crossing, and the volatile pointers keep the inliner from removing it.
 * Compared bit for bit with the host. */
void writec(int c);
void puts_(const char *s);
typedef unsigned u32;
union F { float f; u32 u; };
union D { double d; u32 w[2]; };
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static void pf(float f) { union F x; x.f = f; hx(x.u); }
static void pd(double d) { union D x; x.d = d; hx(x.w[0]); hx(x.w[1]); }

struct v3 { float x, y, z; };
static float fma3(float a, double b, float c) { return a * c + (float)b; }
static double dmix(float a, double b, int k, double c)
{ return (double)a * b - c / (double)k; }
static struct v3 cross(struct v3 a, struct v3 b)
{
    struct v3 r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}
static float sum12(float a, float b, float c, float d, float e, float f,
                   float g, float h, float i, float j, float k, float l)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8
         + i * 9 + j * 10 + k * 11 + l * 12; }
static float  (*volatile p_fma3)(float, double, float) = fma3;
static double (*volatile p_dmix)(float, double, int, double) = dmix;
static struct v3 (*volatile p_cross)(struct v3, struct v3) = cross;
static float (*volatile p_sum12)(float, float, float, float, float, float,
                                 float, float, float, float, float, float)
    = sum12;

/* A float at an ODD address, which only a packed struct makes legal.
 * ldr takes it on a Cortex-M4; vldr faults. So these must stay core
 * loads and stores, however much the value wants an S register. */
struct __attribute__((packed)) pk { char c; float f; float g; };
static struct pk pks[2] = { { 1, 1.5f, -2.25f }, { 2, 3.75f, 0.5f } };
static struct pk *volatile ppk = &pks[1];
static float packed_work(struct pk *p, struct pk *q)
{
    float x = p->f * q->g + p->g;
    q->f = x * 2.0f;
    p->g = x - q->f;
    pks[0].f = pks[1].g + x;
    return x + pks[0].f;
}
static float (*volatile p_packed)(struct pk *, struct pk *) = packed_work;

int main(void)
{
    struct v3 a = { 1.5f, -2.25f, 3.125f }, b = { -0.5f, 4.75f, 2.0f }, r;
    pf(p_fma3(1.25f, 0.1, 3.5f));
    pd(p_dmix(0.3f, 1.0 / 3.0, 7, 2.5));
    r = p_cross(a, b);
    pf(r.x); pf(r.y); pf(r.z);
    pf(p_sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12.5f));
    pf(p_packed(&pks[0], ppk));
    pf(pks[0].f); pf(pks[0].g); pf(pks[1].f); pf(pks[1].g);
    puts_("\n==END==\n");
    return 0;
}
