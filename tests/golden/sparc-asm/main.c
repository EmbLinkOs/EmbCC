/* SPARC assembly with C, run on QEMU's leon3_generic by
 * tests/golden/sparc-asm.sh: forms.S's functions called from C and
 * calling C; inline templates that read and write %psr and %y, loop on
 * numeric labels, annul a slot, take every operand kind ("r", "m", "I",
 * a tied "0", "+r", register variables), call C through a register, and
 * keep values live across templates that clobber -- or only name --
 * registers; a file-scope asm function with %hi/%lo of a C global, and
 * a naked function that calls C. Every value printed is fixed. */
void puts_(const char *s);
void putn(long v);
extern int sp_add(int a, int b);
extern int sp_call(int x);
extern int sp_sum(int n);
extern int sp_annul(int x);
extern int sp_ba_annul(int x);
extern int *sp_table(void);
extern int sp_load_tab(int i);
extern unsigned sp_umulhi(unsigned a, unsigned b);
extern unsigned sp_div(unsigned a, unsigned b);
extern int sp_set(void);
extern int sp_pil(void);
extern int sp_tail(int x);
extern int sp_indirect(int (*f)(int), int x);
extern int sp_casa(int *p, int old, int new_);
extern int sp_swap(int *p, int v);
extern int sp_ldd(int *p);
extern int sp_bytes(const void *p);
extern void sp_stores(void *p, int v);
extern int table[];
extern short halves[];
int counter = 40;
int pair[2] __attribute__((aligned(8))) = { 50, 8 };
int st4[4] __attribute__((aligned(8)));
int c_twice(int x) { return 2 * x + counter; }
static int c_plus7(int x) { return x + 7; }

/* a file-scope function: counter + x, through %hi/%lo of a C global */
__asm__(".global blk_inc\n"
        ".type blk_inc, #function\n"
        ".align 4\n"
        "blk_inc:\n"
        "  sethi %hi(counter), %o1\n"
        "  ld [%o1 + %lo(counter)], %o1\n"
        "  retl\n"
        "   add %o1, %o0, %o0\n"
        ".size blk_inc, .-blk_inc\n");
int blk_inc(int);

__attribute__((naked)) int naked_sub(int a, int b)
{
    __asm__("retl");
    __asm__(" sub %o0, %o1, %o0");
}

int bump_count;
void bump(void) { bump_count += 3; }
/* naked, and calling C: its own window, the call (and its slot) written
 * by EmbCC */
__attribute__((naked)) void naked_bump(void)
{
    __asm__("save %sp, -96, %sp");
    bump();
    __asm__("ret");
    __asm__(" restore");
}

__attribute__((noinline)) int seed(int k) { return k + counter - 40; }

/* ldd writes the odd register of its pair, which the template never names:
 * only the clobber list keeps the values live across it (they cross calls,
 * so they want the window's own registers) out of %l1, %l3 and %l5 */
int minus2[2] __attribute__((aligned(8))) = { -1, -1 };
__attribute__((noinline)) int pair_clobbers(int k)
{
    int a = seed(k + 1), b = seed(k + 2), c = seed(k + 3), d = seed(k + 4);
    int e = seed(k + 5), f = seed(k + 6), g = seed(k + 7), h = seed(k + 8);
    __asm__ volatile("ldd [%0], %%o0\n ldd [%0], %%o2\n ldd [%0], %%o4\n"
                     "ldd [%0], %%l0\n ldd [%0], %%l2\n ldd [%0], %%l4"
                     :: "r"(minus2)
                     : "o1", "o3", "o5", "l1", "l3", "l5", "memory");
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f +
           1000000 * g + 10000000 * h;
}

int main(void)
{
    int n = seed(10), s = 0, v, w, mem = 1234, buf[4] = { 0, 0, 0, 0 };
    unsigned psr, y;
    unsigned char bytes[4] = { 0xc8, 0xfd, 0x80, 0x01 };

    /* the PSR's interrupt level (the harness runs at PIL 15), and %y */
    __asm__ volatile("rd %%psr, %0" : "=r"(psr));
    putn((psr >> 8) & 15);
    __asm__ volatile("wr %1, %%y\n nop\n nop\n nop\n rd %%y, %0"
                     : "=r"(y) : "r"(seed(77)));
    putn(y);
    /* a counted loop on numeric labels */
    __asm__("1: add %0, %1, %0\n"
            "   subcc %1, 1, %1\n"
            "   bne 1b\n"
            "    nop" : "+r"(s), "+r"(n) : : "cc");
    putn(s);
    /* an annulled slot, taken and not */
    v = seed(7);
    __asm__("cmp %0, 5\n bg,a 1f\n  add %0, 100, %0\n1:" : "+r"(v) : : "cc");
    putn(v);
    v = seed(3);
    __asm__("cmp %0, 5\n bg,a 1f\n  add %0, 100, %0\n1:" : "+r"(v) : : "cc");
    putn(v);
    /* every operand kind */
    __asm__("ld %1, %0" : "=r"(v) : "m"(mem));
    putn(v);
    __asm__("st %1, %0" : "=m"(buf[1]) : "r"(seed(55)));
    putn(buf[1]);
    __asm__("add %0, %2, %0" : "=r"(v) : "0"(seed(20)), "I"(-4096));
    putn(v);
    __asm__("sll %1, %2, %0" : "=r"(w) : "r"(seed(3)), "n"(4));
    putn(w);
    {
        register int a __asm__("o3") = seed(6), b __asm__("l2") = seed(9);
        __asm__("smul %1, %2, %0" : "=r"(v) : "r"(a), "r"(b));
        putn(v);
    }
    __asm__("set %1, %0" : "=r"(v) : "i"(0x7654321));
    putn(v);
    /* a call through a register, which changes the outs and %g1-%g4 */
    {
        int keep1 = seed(11), keep2 = seed(12), r;
        __asm__ volatile("call %1\n mov %2, %%o0\n mov %%o0, %0"
                         : "=r"(r) : "r"(c_plus7), "r"(seed(30))
                         : "o0", "o1", "o2", "o3", "o4", "o5", "o7", "g1",
                           "g2", "g3", "g4", "cc", "memory");
        putn(r);
        putn(keep1 + keep2);
    }
    /* values live across templates that clobber the locals, or only name
     * them */
    {
        int a = seed(1), b = seed(2), c = seed(3), d = seed(4), e = seed(5);
        int f = seed(6), g = seed(7), h = seed(8);
        __asm__ volatile("mov -1, %%l0\n mov -1, %%l1\n mov -1, %%l2\n"
                         "mov -1, %%l3\n mov -1, %%l4\n mov -1, %%l5\n"
                         "mov -1, %%i0\n mov -1, %%i1\n mov -1, %%i2\n"
                         "mov -1, %%i3\n mov -1, %%o0\n mov -1, %%o1"
                         ::: "l0", "l1", "l2", "l3", "l4", "l5", "i0", "i1",
                             "i2", "i3", "o0", "o1");
        __asm__ volatile("mov -2, %o2\n mov -2, %o3\n mov -2, %o4\n"
                         "mov -2, %o5\n mov -2, %i4\n mov -2, %i5");
        putn(a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f +
             1000000 * g + 10000000 * h);
    }
    putn(pair_clobbers(0));
    puts_("\n");
    putn(sp_add(40, 2));
    putn(sp_call(1));
    putn(sp_sum(10));
    putn(sp_annul(10));
    putn(sp_annul(2));
    putn(sp_ba_annul(9));
    putn(sp_table() == table);
    putn(sp_load_tab(1));
    putn(sp_load_tab(0) == (int)&counter);
    putn(((int (*)(int, int))sp_load_tab(2))(5, 6));
    putn(sp_load_tab(3));
    putn(sp_umulhi(0x80000000u, 6));
    putn(sp_div(1000, 7));
    putn(sp_set());
    putn(sp_pil());
    putn(sp_tail(5));
    putn(sp_indirect(c_plus7, 35));
    buf[0] = 5;
    putn(sp_casa(&buf[0], 5, 77));
    putn(buf[0]);
    putn(sp_casa(&buf[0], 5, 88));
    putn(buf[0]);
    putn(sp_swap(&buf[0], 99));
    putn(buf[0]);
    putn(sp_ldd(pair));
    putn(sp_bytes(bytes));
    putn(halves[0] + halves[1]);
    sp_stores(st4, 0x1234abcd);
    putn(((unsigned char *)st4)[0] + ((unsigned char *)st4)[1] +
         ((unsigned char *)st4)[2]);
    putn(st4[2] + st4[3]);
    putn(blk_inc(2));
    putn(naked_sub(50, 8));
    naked_bump();
    naked_bump();
    putn(bump_count);
    puts_("\n==END==\n");
    return 0;
}
