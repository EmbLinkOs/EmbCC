/* Inline asm whose one output is a value: "=r" and "+r" on locals and
 * arguments (in registers, as the asm's result -- not through an address),
 * an earlyclobber, a register pair, a value live across an asm that
 * clobbers registers, and "+r" on a global (through its address), each
 * against the same computation in C over many inputs. On AVR the
 * templates are AVR's; elsewhere the program is the C alone. */
// expect-exit: 42

static unsigned char g;

#ifdef __AVR__
static unsigned char sw(unsigned char x) { __asm__("swap %0" : "+r"(x)); return x; }
static unsigned char inc(unsigned char x)
{
    unsigned char y;
    __asm__("mov %0, %1\n\tinc %0" : "=&r"(y) : "r"(x));
    return y;
}
static unsigned add1(unsigned x) { __asm__("adiw %0, 1" : "+w"(x)); return x; }
static unsigned char keep(unsigned char a, unsigned char b)
{
    unsigned char t = (unsigned char)(a * 3);     /* live across the asm */
    __asm__ volatile("ldi r18, 0x55\n\tmov r19, r18\n\tmov r20, r18"
                     ::: "r18", "r19", "r20");
    __asm__("eor %0, %1" : "+r"(b) : "r"(t));
    return (unsigned char)(b + t);
}
static void gswap(void) { __asm__("swap %0" : "+r"(g)); }
#else
static unsigned char sw(unsigned char x) { return (unsigned char)(x << 4 | x >> 4); }
static unsigned char inc(unsigned char x) { return (unsigned char)(x + 1); }
static unsigned add1(unsigned x) { return x + 1; }
static unsigned char keep(unsigned char a, unsigned char b)
{
    unsigned char t = (unsigned char)(a * 3);
    b ^= t;
    return (unsigned char)(b + t);
}
static void gswap(void) { g = (unsigned char)(g << 4 | g >> 4); }
#endif

static volatile unsigned char seed = 7;

int main(void)
{
    int bad = 0;
    unsigned char s = seed;
    for (int k = 0; k < 300; k++) {
        unsigned char x = (unsigned char)(s * 37u + (unsigned)k);
        unsigned char want_sw = (unsigned char)(x << 4 | x >> 4);
        bad |= (sw(x) != want_sw) << 0;
        bad |= (inc(x) != (unsigned char)(x + 1)) << 1;
        unsigned w16 = (unsigned)x * 257u + (unsigned)k;
        bad |= (add1(w16) != (unsigned)(w16 + 1)) << 2;
        unsigned char t = (unsigned char)(x * 3);
        bad |= (keep(x, (unsigned char)k) != (unsigned char)(((unsigned char)k ^ t) + t)) << 3;
        g = x;
        gswap();
        bad |= (g != want_sw) << 4;
        s = (unsigned char)(s ^ x);
    }
    return bad ? bad : 42;
}
