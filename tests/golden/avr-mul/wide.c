/* Full products of values that fit 16 bits -- the four partial products of
 * src/arch/avr/codegen.c (avr_mul_wide16) -- against a product by shifts
 * and adds: int by int, int by char, a square, and by constants above
 * the shifts-and-adds bound, each read as all four bytes. */
typedef unsigned long u32;
extern void writec(int);
static u32 st = 521288629UL;
static u32 xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }
static u32 ref(u32 x, u32 y)
{
    u32 r = 0;
    while (y) { if (y & 1) r += x; x += x; y >>= 1; }
    return r;
}
__attribute__((noinline)) u32 wide(unsigned a, unsigned b) { return (u32)a * b; }
__attribute__((noinline)) u32 wc(unsigned a, unsigned char c) { return (u32)a * c; }
__attribute__((noinline)) u32 wsq(unsigned a) { return (u32)a * a; }
__attribute__((noinline)) u32 wk1(unsigned a) { return (u32)a * 52429u; }
__attribute__((noinline)) u32 wk2(unsigned a) { return (u32)a * 0xffffu; }
__attribute__((noinline)) u32 wk3(unsigned char a) { return (u32)a * 40503u; }
__attribute__((noinline)) u32 wacc(unsigned a, unsigned b, u32 c) { return c + (u32)a * b; }
static void say(const char *m) { for (; *m; m++) writec(*m); }
int main(void)
{
    int bad = 0;
    for (int i = 0; i < 1500; i++) {
        u32 r1 = xs(), r2 = xs();
        unsigned a = (unsigned)r1, b = (unsigned)(r1 >> 16);
        if (i < 16) { a = (unsigned)(i & 3) * 0x7fff + (i >> 2); b = 0xffffu - (unsigned)i; }
        if (wide(a, b) != ref(a, b)) bad |= 1;
        if (wc(a, (unsigned char)b) != ref(a, b & 0xff)) bad |= 2;
        if (wsq(a) != ref(a, a)) bad |= 4;
        if (wk1(a) != ref(a, 52429u)) bad |= 8;
        if (wk2(a) != ref(a, 0xffffu)) bad |= 16;
        if (wk3((unsigned char)a) != ref(a & 0xff, 40503u)) bad |= 32;
        if (wacc(a, b, r2) != r2 + ref(a, b)) bad |= 64;
    }
    if (bad) { say("WIDE BAD "); for (int k = 6; k >= 0; k--) writec(bad >> k & 1 ? '1' : '0'); say("\n"); }
    else say("WIDE OK\n");
    say("==END==\n");
    for (;;) ;
}
