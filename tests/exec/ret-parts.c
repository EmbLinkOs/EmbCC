/* A small struct returned as its register units (pass_sroa's ret parts,
 * IR_RET's nrp): built field by field and returned without going through
 * its slot. Each shape a convention returns in registers -- two words,
 * two floats, two doubles, two longs, an int beside a float -- and the
 * one that needs care: parts that arrive in each other's return
 * registers, `return (pair){ b, a }` with a in a0 and b in a1. */
// expect-exit: 42
struct ii { int a, b; };
struct ff { float x, y; };
struct dd { double re, im; };
struct ll { long a, b; };
struct mx { int n; float f; };

__attribute__((noinline)) struct ii mk_ii(int a, int b)
{
    struct ii r;
    r.a = a * 3 + b;
    r.b = b - a;
    return r;
}

/* each part already in the other's register */
__attribute__((noinline)) struct ii swap_ii(int a, int b)
{
    struct ii r = { b, a };
    return r;
}

__attribute__((noinline)) struct ff swap_ff(float a, float b)
{
    struct ff r = { b, a };
    return r;
}

__attribute__((noinline)) struct ff mk_ff(float x, float y)
{
    struct ff r;
    r.x = x * 2.0f + y;
    r.y = y - x;
    return r;
}

__attribute__((noinline)) struct dd swap_dd(double a, double b)
{
    struct dd r = { b, a };
    return r;
}

__attribute__((noinline)) struct ll swap_ll(long a, long b)
{
    struct ll r = { b, a };
    return r;
}

__attribute__((noinline)) struct mx mk_mx(int n, float f)
{
    struct mx r;
    r.n = n + 1;
    r.f = f * 0.5f;
    return r;
}

static volatile int vi[2] = { 7, 11 };
static volatile float vf[2] = { 1.5f, -4.25f };
static volatile double vd[2] = { 3.0, -0.125 };
static volatile long vl[2] = { 100000, -2 };

int main(void)
{
    struct ii a = mk_ii(vi[0], vi[1]);
    if (a.a != 32 || a.b != 4) return 1;
    struct ii s = swap_ii(vi[0], vi[1]);
    if (s.a != 11 || s.b != 7) return 2;
    struct ff f = swap_ff(vf[0], vf[1]);
    if (f.x != -4.25f || f.y != 1.5f) return 3;
    struct ff g = mk_ff(vf[0], vf[1]);
    if (g.x != -1.25f || g.y != -5.75f) return 4;
    struct dd d = swap_dd(vd[0], vd[1]);
    if (d.re != -0.125 || d.im != 3.0) return 5;
    struct ll l = swap_ll(vl[0], vl[1]);
    if (l.a != -2 || l.b != 100000) return 6;
    struct mx m = mk_mx(vi[0], vf[1]);
    if (m.n != 8 || m.f != -2.125f) return 7;
    return 42;
}
