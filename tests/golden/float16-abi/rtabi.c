/* The ARM run-time ABI's half-precision helpers -- __aeabi_h2f,
 * __aeabi_f2h and __aeabi_d2h -- which clang and GCC call for a
 * conversion the FPU cannot make, in core registers whatever the float
 * ABI. Built by the reference compiler and linked against EmbCC's runtime
 * (float16-abi.sh): called by name, and by clang's own lowering of the
 * conversions where there is no FPU to make them. */

__attribute__((pcs("aapcs"))) float __aeabi_h2f(unsigned short);
__attribute__((pcs("aapcs"))) unsigned short __aeabi_f2h(float);
__attribute__((pcs("aapcs"))) unsigned short __aeabi_d2h(double);

static const struct { float f; unsigned short h; } fh[] = {
    { 1.5f, 0x3e00 }, { -65504.0f, 0xfbff },
    { 65520.0f, 0x7c00 },               /* the tie past the largest: infinity */
    { 0x1p-24f, 0x0001 },               /* the least subnormal */
    { 0x1p-25f, 0x0000 },               /* the tie below it, to even */
    { 0x1.8p-25f, 0x0001 },
};
static const struct { double d; unsigned short h; } dh[] = {
    { -0.0, 0x8000 },
    { 1.0 + 0x1p-11, 0x3c00 },          /* a tie, to even */
    /* just past the tie: through float first, 0x3c00 */
    { 1.0 + 0x1p-11 + 0x1p-40, 0x3c01 },
};

static volatile float vf;
static volatile double vd;
static volatile _Float16 vh;

static unsigned short bits(_Float16 h)
{
    union { _Float16 h; unsigned short u; } p;
    p.h = h;
    return p.u;
}

int main(void)
{
    int bad = 0;
    for (unsigned i = 0; i < sizeof fh / sizeof fh[0]; i++) {
        bad |= (__aeabi_f2h(fh[i].f) != fh[i].h) << 0;
        vf = fh[i].f;
        bad |= (bits((_Float16)vf) != fh[i].h) << 1;
    }
    for (unsigned i = 0; i < sizeof dh / sizeof dh[0]; i++) {
        bad |= (__aeabi_d2h(dh[i].d) != dh[i].h) << 2;
        vd = dh[i].d;
        bad |= (bits((_Float16)vd) != dh[i].h) << 3;
    }
    bad |= (__aeabi_h2f(0x3c00) != 1.0f || __aeabi_h2f(0x0001) != 0x1p-24f ||
            __aeabi_h2f(0xfc00) != -__builtin_inff()) << 4;
    {
        float n = __aeabi_h2f(0x7e00);
        bad |= (n == n) << 4;
    }
    vh = (_Float16)0.75f;
    bad |= ((float)vh != 0.75f) << 5;
    return bad ? bad : 42;
}
