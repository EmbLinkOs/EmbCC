/* Arguments and results that live far from the stack pointer, and an
 * argument split between registers and the stack in a frame whose base
 * is not the stack pointer.
 *   - A frame past 4 KB with arguments passed on the stack: they sit
 *     above the whole frame, out of the 12-bit offset an ARMv7-M load
 *     takes, and their loads were refused ("a 4-byte load at offset 6008
 *     from r13 is out of reach") -- at -O0 a large function's temps alone
 *     make such a frame.
 *   - The same frame returning a struct through the caller's buffer,
 *     whose address is saved in a slot at the top of it.
 *   - A seven-byte struct whose first word goes in r3 and whose last
 *     three bytes go on the stack, passed from a function with a VLA:
 *     the bytes past the last whole word were stored relative to the
 *     frame base, which after a VLA is not the stack pointer, so the
 *     callee read three bytes nothing had written.
 * Every value is checked against a closed form. */
// expect-exit: 42
struct big { int v[40]; };
struct s7 { unsigned char b[7]; };

__attribute__((noinline)) long far_args(int a, int b, int c, int d, int e, int f,
                                        long long g, int h)
{
    volatile int pad[1500];               /* 6000 bytes of frame */
    for (int i = 0; i < 1500; i++) pad[i] = i;
    long s = 0;
    for (int i = 0; i < 1500; i += 499) s += pad[i];
    return s + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * (long)(g >> 32) + 8 * h;
}

__attribute__((noinline)) struct big far_ret(int k)
{
    volatile int pad[1500];
    for (int i = 0; i < 1500; i++) pad[i] = i * k;
    struct big r;
    for (int i = 0; i < 40; i++) r.v[i] = pad[i * 37] + i;
    return r;
}

__attribute__((noinline)) int take7(int a, int b, int c, struct s7 x)
{
    int s = a + b + c;
    for (int i = 0; i < 7; i++) s = s * 3 + x.b[i];
    return s;
}

__attribute__((noinline)) int pass7(int n)
{
    char vla[n];                          /* the frame base is not sp now */
    for (int i = 0; i < n; i++) vla[i] = (char)i;
    struct s7 x;
    for (int i = 0; i < 7; i++) x.b[i] = (unsigned char)(0x41 + i);
    return take7(1, 2, 3, x) + vla[n - 1];
}

int main(void)
{
    long want = (0 + 499 + 998 + 1497) + 1 + 2 * 2 + 3 * 3 + 4 * 4 + 5 * 5 + 6 * 6 + 7 * 9 + 8 * 11;
    if (far_args(1, 2, 3, 4, 5, 6, 9LL << 32, 11) != want) return 1;
    struct big r = far_ret(3);
    for (int i = 0; i < 40; i++) if (r.v[i] != i * 37 * 3 + i) return 2;
    int s = 1 + 2 + 3;
    for (int i = 0; i < 7; i++) s = s * 3 + 0x41 + i;
    if (pass7(9) != s + 8) return 3;
    return 42;
}
