/* A branch on a floating-point comparison, both ways round, with NaN.
 *
 * On a soft-float Cortex-M the compare is a helper call (__ltdf2 and
 * family) whose int answer the branch tests directly. `if (!(a < b))` is
 * NOT `if (a >= b)`: with a NaN the first is taken and the second is not,
 * so the branch on a false answer must be the inverse of the predicate's
 * condition on the helper's result, never the opposite predicate. Every
 * predicate, both senses, float and double, ordered and unordered. */
// expect-exit: 42
static volatile double vz, vone = 1.0, vtwo = 2.0;
static volatile float fz, fone = 1.0f, ftwo = 2.0f;

#define BR(T, name, op)                                                    \
    __attribute__((noinline)) int name##_t(T a, T b)                       \
    { if (a op b) return 1; return 0; }                                    \
    __attribute__((noinline)) int name##_f(T a, T b)                       \
    { if (!(a op b)) return 1; return 0; }

BR(double, dlt, <) BR(double, dle, <=) BR(double, dgt, >)
BR(double, dge, >=) BR(double, deq, ==) BR(double, dne, !=)
BR(float, flt, <) BR(float, fle, <=) BR(float, fgt, >)
BR(float, fge, >=) BR(float, feq, ==) BR(float, fne, !=)

/* The right operand just computed by a call, so it is in r0:r1 (r0) where
 * the helper wants the left one: the compare is made the other way round
 * (b > a for a < b), which must give the same answer, NaN included. */
static volatile double vgot;
static volatile float fgot;
__attribute__((noinline)) double dget(double x) { return x + vgot; }
__attribute__((noinline)) float fget(float x) { return x + fgot; }
__attribute__((noinline)) int dmirror(double a, double b)
{
    int r = 0;
    if (a < dget(b)) r |= 1;
    if (a <= dget(b)) r |= 2;
    if (a > dget(b)) r |= 4;
    if (a >= dget(b)) r |= 8;
    if (a == dget(b)) r |= 16;
    if (a != dget(b)) r |= 32;
    return r;
}
__attribute__((noinline)) int fmirror(float a, float b)
{
    int r = 0;
    if (a < fget(b)) r |= 1;
    if (a <= fget(b)) r |= 2;
    if (a > fget(b)) r |= 4;
    if (a >= fget(b)) r |= 8;
    if (a == fget(b)) r |= 16;
    if (a != fget(b)) r |= 32;
    return r;
}

/* the expected answer of `a op b` for ordered a, b, and for a NaN */
static int want(int op, double a, double b, int nan)
{
    if (nan) return op == 5;            /* only != holds */
    switch (op) {
    case 0: return a < b;  case 1: return a <= b; case 2: return a > b;
    case 3: return a >= b; case 4: return a == b; default: return a != b;
    }
}

int main(void)
{
    double dn = vz / vz;                /* NaN, made at run time */
    float fn = fz / fz;
    int (*dt[6])(double, double) = { dlt_t, dle_t, dgt_t, dge_t, deq_t, dne_t };
    int (*df[6])(double, double) = { dlt_f, dle_f, dgt_f, dge_f, deq_f, dne_f };
    int (*ft[6])(float, float) = { flt_t, fle_t, fgt_t, fge_t, feq_t, fne_t };
    int (*ff[6])(float, float) = { flt_f, fle_f, fgt_f, fge_f, feq_f, fne_f };
    double da[5] = { 0, 0, 0, 0, 0 }, db[5] = { 0, 0, 0, 0, 0 };
    float fa[5], fb[5];
    int nan[5] = { 0, 0, 0, 1, 1 };
    da[0] = vone; db[0] = vtwo;         /* less */
    da[1] = vtwo; db[1] = vone;         /* greater */
    da[2] = vone; db[2] = vone;         /* equal */
    da[3] = dn;   db[3] = vone;         /* unordered, both sides */
    da[4] = vone; db[4] = dn;
    fa[0] = fone; fb[0] = ftwo; fa[1] = ftwo; fb[1] = fone;
    fa[2] = fone; fb[2] = fone; fa[3] = fn; fb[3] = fone;
    fa[4] = fone; fb[4] = fn;
    int bad = 0;
    for (int op = 0; op < 6; op++)
        for (int k = 0; k < 5; k++) {
            int w = want(op, da[k], db[k], nan[k]);
            if (dt[op](da[k], db[k]) != w) bad |= 1;
            if (df[op](da[k], db[k]) != !w) bad |= 2;
            if (ft[op](fa[k], fb[k]) != w) bad |= 4;
            if (ff[op](fa[k], fb[k]) != !w) bad |= 8;
        }
    for (int k = 0; k < 5; k++) {
        int dw = 0, fw = 0;
        for (int op = 0; op < 6; op++) {
            dw |= want(op, da[k], db[k], nan[k]) << op;
            fw |= want(op, fa[k], fb[k], nan[k]) << op;
        }
        if (dmirror(da[k], db[k]) != dw) bad |= 16;
        if (fmirror(fa[k], fb[k]) != fw) bad |= 32;
    }
    return bad ? bad : 42;
}
