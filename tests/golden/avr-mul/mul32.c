/* __mulsi3 (lib/rt/avr.c) against a product by shifts and adds, over
 * 2000 pairs: edges, pseudo-random words, and words masked to 16 and 8
 * bits. The operands are xorshift, which multiplies nothing. */
typedef unsigned long u32;
extern void writec(int);
static u32 st = 2463534242UL;
static u32 xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }
/* the product by shifts and adds, no multiply anywhere */
static u32 ref(u32 x, u32 y)
{
    u32 r = 0;
    while (y) { if (y & 1) r += x; x += x; y >>= 1; }
    return r;
}
volatile u32 va, vb;
static void say(const char *m) { for (; *m; m++) writec(*m); }
int main(void)
{
    int bad = 0;
    static const u32 edge[] = { 0, 1, 0xffffffffUL, 0x80000000UL, 0x10000UL, 0xffffUL, 0xffUL, 0x7fffffffUL };
    for (int i = 0; i < 2000; i++) {
        u32 x = i < 64 ? edge[i & 7] : xs(), y = i < 64 ? edge[(i >> 3) & 7] : xs();
        if (i >= 64 && (i & 3) == 1) y &= 0xffffUL;
        if (i >= 64 && (i & 3) == 2) x &= 0xffUL;
        va = x; vb = y;
        if ((u32)(va * vb) != ref(x, y)) bad++;
    }
    say(bad ? "MUL BAD\n" : "MUL OK\n");
    say("==END==\n");
    for (;;) ;
}
