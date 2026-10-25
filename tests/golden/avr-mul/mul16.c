/* Products of an int or a char -- the inline mul of src/arch/avr/codegen.c
 * (avr_mul_inline) -- against a product by shifts and adds: unsigned and
 * signed, by variables and by constants (a constant takes mul only where
 * the shifts and adds would be longer), a square, and a multiply-add. */
typedef unsigned long u32;
extern void writec(int);
static u32 st = 88172645UL;
static u32 xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }
static u32 ref(u32 x, u32 y)
{
    u32 r = 0;
    while (y) { if (y & 1) r += x; x += x; y >>= 1; }
    return r;
}
__attribute__((noinline)) unsigned mu(unsigned a, unsigned b) { return a * b; }
__attribute__((noinline)) int ms(int a, int b) { return a * b; }
__attribute__((noinline)) unsigned char mc(unsigned char a, unsigned char b) { return a * b; }
__attribute__((noinline)) signed char msc(signed char a, signed char b) { return a * b; }
__attribute__((noinline)) unsigned k1(unsigned a) { return a * 52429u; }
__attribute__((noinline)) unsigned k2(unsigned a) { return a * 1000u; }
__attribute__((noinline)) int k3(int a) { return a * -12345; }
__attribute__((noinline)) unsigned k4(unsigned a) { return a * 0x0100u; }
__attribute__((noinline)) unsigned k5(unsigned a) { return a * 0x00ffu; }
__attribute__((noinline)) unsigned char k6(unsigned char a) { return a * 205; }
__attribute__((noinline)) unsigned sq(unsigned a) { return a * a; }
__attribute__((noinline)) unsigned acc(unsigned a, unsigned b, unsigned c) { return c + a * b; }
static void say(const char *m) { for (; *m; m++) writec(*m); }
int main(void)
{
    int bad = 0;
    for (int i = 0; i < 1500; i++) {
        u32 r1 = xs(), r2 = xs();
        unsigned a = (unsigned)r1, b = (unsigned)(r1 >> 16), c = (unsigned)r2;
        if (i < 16) { a = (unsigned)(i & 3) * 0x7fff + (i >> 2); b = 0xffffu - (unsigned)i; }
        if (mu(a, b) != (unsigned)ref(a, b)) bad |= 1;
        if (ms((int)a, (int)b) != (int)(unsigned)ref(a, b)) bad |= 2;
        if (mc((unsigned char)a, (unsigned char)b) != (unsigned char)ref(a & 0xff, b & 0xff)) bad |= 4;
        if (msc((signed char)a, (signed char)b) != (signed char)(unsigned char)ref(a & 0xff, b & 0xff)) bad |= 8;
        if (k1(a) != (unsigned)ref(a, 52429u)) bad |= 16;
        if (k2(a) != (unsigned)ref(a, 1000u)) bad |= 32;
        if (k3((int)a) != (int)(unsigned)ref(a, (unsigned)-12345)) bad |= 64;
        if (k4(a) != (unsigned)ref(a, 0x100u) || k5(a) != (unsigned)ref(a, 0xffu)) bad |= 128;
        if (k6((unsigned char)a) != (unsigned char)ref(a & 0xff, 205)) bad |= 256;
        if (sq(a) != (unsigned)ref(a, a) || acc(a, b, c) != (unsigned)(c + ref(a, b))) bad |= 512;
    }
    if (bad) { say("MUL16 BAD "); for (int k = 9; k >= 0; k--) writec(bad >> k & 1 ? '1' : '0'); say("\n"); }
    else say("MUL16 OK\n");
    say("==END==\n");
    for (;;) ;
}
