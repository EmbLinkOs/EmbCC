/* Xtensa assembly in a C program, run on QEMU's de212 by
 * tests/golden/xtensa-asm.sh with forms.S assembled by EmbCC and by GNU
 * as: inline asm with "r", "m", "=m", "i", "n", "+r", "=&r" and named
 * operands, two outputs, a loop on numeric labels, rsr/rsil/wsr of PS,
 * CCOUNT, xsr, a callx8 to C from a template, values live across a
 * template that clobbers eleven registers, a "memory" clobber; a
 * file-scope asm function with a literal, a naked function, and calls
 * both ways between C and forms.S. Every value printed is fixed. */
void puts_(const char *s);
void putn(long v);
extern int asm_add(int a, int b);
extern int asm_call(int x);
extern int asm_sum(int n);
extern int asm_classify(int x);
extern int asm_ps(void);
extern unsigned asm_ccount(void);
extern int asm_xsr(void);
extern int asm_calls(int x);
extern int asm_lit(void);
extern int asm_other(int x);
extern int table[];
int counter = 40;
int c_twice(int x) { return 2 * x + counter; }

__asm__(".global blk_inc\n"
        ".type blk_inc, @function\n"
        ".align 4\n"
        "blk_inc:\n"
        "  entry a1, 32\n"
        "  movi a3, 100000\n"
        "  add a2, a2, a3\n"
        "  retw\n");
int blk_inc(int);

__attribute__((naked)) int naked_sub(int a, int b)
{
    __asm__("entry a1, 32");
    __asm__("sub a2, a2, a3");
    __asm__("retw");
}

/* Opaque to the optimizer, so the values below are computed at run time
 * and live in registers across the asm that clobbers them. */
__attribute__((noinline)) int seed(int k) { return k + counter - 40; }

int main(void)
{
    int v, x = seed(77), y = seed(5), w = 1000;
    __asm__("add %0, %1, %2" : "=r"(v) : "r"(x), "r"(y));
    putn(v);
    __asm__("l32i %0, %1" : "=r"(v) : "m"(x));
    putn(v);
    __asm__("s32i %1, %0" : "=m"(w) : "r"(y));
    putn(w);
    w = 1000;
    __asm__("addi %0, %0, %1" : "+r"(w) : "i"(-24));
    putn(w);
    __asm__("movi %0, %1" : "=r"(v) : "n"(-300));
    putn(v);
    __asm__("sub %[d], %[a], %[b]" : [d] "=r"(v) : [a] "r"(x), [b] "r"(y));
    putn(v);
    __asm__("movi %0, 1\n add %0, %0, %1\n add %0, %0, %2"
            : "=&r"(v) : "r"(x), "r"(y));
    putn(v);
    {
        int q, r;
        __asm__("quou %0, %2, %3; remu %1, %2, %3"
                : "=r"(q), "=r"(r) : "r"(x), "r"(y));
        putn(q * 100 + r);
    }
    {
        int n = seed(10), s = 0;
        __asm__("1: add %0, %0, %1\n"
                "   addi %1, %1, -1\n"
                "   bnez %1, 1b" : "+r"(s), "+r"(n));
        putn(s);
    }
    {
        unsigned old, lvl, back;
        __asm__ volatile("rsil %0, 4" : "=r"(old) :: "memory");
        __asm__ volatile("rsr %0, ps" : "=r"(lvl));
        __asm__ volatile("wsr %0, ps\n rsync" :: "r"(old) : "memory");
        __asm__ volatile("rsr.ps %0" : "=r"(back));
        putn((int)((lvl & 15) * 10 + (back & 15)));
    }
    {
        unsigned t1, t2;
        __asm__ volatile("rsr %0, ccount" : "=r"(t1));
        for (volatile int k = 0; k < 50; k++)
            ;
        __asm__ volatile("rsr %0, ccount" : "=r"(t2));
        putn(t2 != t1);
    }
    {
        int r;
        __asm__ volatile("mov a10, %1\n callx8 %2\n mov %0, a10"
                         : "=r"(r) : "r"(x), "r"(c_twice)
                         : "a8", "a9", "a10", "a11", "a12", "a13", "a14",
                           "a15", "memory");
        putn(r);
    }
    {
        /* EmbCC's own guarantee, beyond GCC's: a windowed call changes
         * the callee's window -- a8-a15 for callx8 -- whether or not the
         * clobber list says so, and nothing live is kept there */
        int r, k1 = x + 1, k2 = y * 3, k3 = x * y, k4 = x - y;
        __asm__ volatile("mov a10, %1\n callx8 %2\n mov %0, a10"
                         : "=r"(r) : "r"(x), "r"(c_twice));
        putn(r);
        putn(k1 + k2 + k3 + k4);
    }
    {
        int a = x * 3, b = y * 7, c = x - y, d = x + 11, e = y * y, f = x ^ y;
        __asm__ volatile("movi a2, 0; movi a3, 0; movi a4, 0; movi a5, 0\n"
                         "movi a6, 0; movi a8, 0; movi a9, 0; movi a10, 0\n"
                         "movi a11, 0; movi a12, 0; movi a13, 0"
                         ::: "a2", "a3", "a4", "a5", "a6", "a8", "a9", "a10",
                             "a11", "a12", "a13");
        putn(a + b + c + d + e + f);
    }
    {
        int m = 1;
        __asm__ volatile("movi a8, 99\n s32i a8, %0, 0"
                         :: "r"(&m) : "a8", "memory");
        putn(m);
    }
    {
        unsigned s1 = 77, s2 = 5, r1, r2;
        __asm__ volatile("wsr %2, excsave1\n mov %0, %3\n xsr %0, excsave1\n"
                         " rsr %1, excsave1"
                         : "=&r"(r1), "=&r"(r2) : "r"(s1), "r"(s2));
        putn((int)(r1 * 10 + r2));
    }
    puts_("\n");
    putn(asm_add(40, 2));
    putn(asm_call(1));
    putn(asm_sum(10));
    putn(asm_classify(10));
    putn(asm_ps());
    putn(asm_ccount() > 0);
    putn(asm_xsr());
    putn(asm_calls(1));
    putn(asm_lit());
    putn(asm_other(5));
    putn(table[0] == (int)&counter);
    putn(table[1]);
    putn(((int (*)(int, int))table[2])(1, 2));
    putn(blk_inc(41));
    putn(naked_sub(50, 8));
    puts_("\n==END==\n");
    return 0;
}
