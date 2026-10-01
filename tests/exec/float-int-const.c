/* One constant, two classes. 0.0f and the integer 0 are the same bits,
 * so the optimizer gives `f * 0.0f` and `r->h = 0` one `const 0`, and
 * that value then has a floating-point use and an integer one. The
 * x86-64 and aarch64 backends put such a value in an FP register when
 * every use takes it as floating point -- and counted a STORE of it as
 * floating point at any width. A 2-byte store went out as movsd: eight
 * bytes, the six past the field written with the rest of the register
 * (aarch64: a 4-byte str at twice the offset). Neighbours of every
 * narrow store are checked, and the same constant returned as an int
 * must reach the integer return register. */
// expect-exit: 42
struct rec { short h; short g1; unsigned char b; unsigned char g2[3]; int g3; };
static short arr[8];
static int after[4];

__attribute__((noinline)) float st_fields(struct rec *r, float f)
{
    r->h = 0;
    r->b = 0;
    return f * 0.0f;
}

__attribute__((noinline)) float st_array(int i, float f)
{
    arr[i] = 0;
    return f * 0.0f;
}

__attribute__((noinline)) int ret_zero(float f, float *o)
{
    *o = f * 0.0f;
    return 0;
}

__attribute__((noinline)) int loop_store(float *o, int n)
{
    /* the shape the fuzzer found: the store and the float use in a loop */
    for (int i = 0; i < n; i++) {
        arr[(i & 3) * 2 + 1] = 0;
        o[i] = (float)arr[7] - 0.0f;
    }
    return after[0];
}

int main(void)
{
    struct rec r = { 0x1111, 0x2222, 0x33, { 0x44, 0x55, 0x66 }, 0x77777777 };
    float o[8];
    int bad = 0;

    for (int k = 0; k < 8; k++) arr[k] = (short)(0x100 + k);
    for (int k = 0; k < 4; k++) after[k] = 0x5a5a5a5a;

    if (st_fields(&r, 3.0f) != 0.0f) bad |= 1;
    if (r.h != 0 || r.g1 != 0x2222 || r.b != 0 || r.g2[0] != 0x44 ||
        r.g2[1] != 0x55 || r.g2[2] != 0x66 || r.g3 != 0x77777777) bad |= 2;

    if (st_array(6, 2.0f) != 0.0f) bad |= 4;
    if (arr[6] != 0 || arr[5] != 0x105 || arr[7] != 0x107) bad |= 8;

    if (st_array(7, 2.0f) != 0.0f) bad |= 16;
    if (arr[7] != 0 || after[0] != 0x5a5a5a5a || after[1] != 0x5a5a5a5a)
        bad |= 32;

    if (ret_zero(5.0f, o) != 0 || o[0] != 0.0f) bad |= 64;

    if (loop_store(o, 8) != 0x5a5a5a5a || o[7] != 0.0f) bad |= 128;
    if (arr[1] != 0 || arr[3] != 0 || arr[5] != 0 || arr[0] != 0x100 ||
        arr[2] != 0x102 || arr[4] != 0x104 || after[1] != 0x5a5a5a5a)
        bad |= 256;

    return bad ? 100 + (bad & 0x7f) + ((bad >> 7) ? 0 : 0) : 42;
}
