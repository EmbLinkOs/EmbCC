/* PowerPC assembly with C, run on QEMU's ppce500 by
 * tests/golden/ppc-asm.sh: forms.S's functions called from C and calling
 * C; inline templates that read the MSR, write and read an SPRG, count
 * with CTR on numeric labels, branch on a record form, take every
 * operand kind ("r", "b", "m" with %U/%X, "I", "K", a tied "0", "+r",
 * register variables), call C through CTR, and keep values live across
 * templates that clobber callee-saved registers -- or only name them, as
 * bare numbers; a file-scope asm function with @ha/@l of a C global, and
 * naked functions, one of which calls C. Every value printed is fixed. */
void puts_(const char *s);
void putn(long v);
extern int pp_add(int a, int b);
extern int pp_call(int x);
extern int pp_sum(int n);
extern int pp_classify(int x);
extern int pp_rec(int x);
extern int *pp_table(void);
extern int pp_load_tab(int i);
extern unsigned pp_mulhi(unsigned a, unsigned b);
extern unsigned pp_div(unsigned a, unsigned b);
extern unsigned pp_rlw(unsigned a, unsigned b);
extern int pp_min(int a, int b);
extern int pp_ee(void);
extern int pp_sprg(int v);
extern int pp_lr(void);
extern int pp_tail(int x);
extern int pp_indirect(int (*f)(int), int x);
extern int pp_cas(int *p, int old, int new_);
extern int pp_bytes(const void *p);
extern unsigned pp_brx(const unsigned *p);
extern int pp_update(const int *p);
extern int pp_notrap(int x, int y);
extern int table[];
extern short halves[];
int counter = 40;
int c_twice(int x) { return 2 * x + counter; }
static int c_plus7(int x) { return x + 7; }

/* a file-scope function: counter + x, through @ha/@l of a C global */
__asm__(".globl blk_inc\n"
        ".type blk_inc, @function\n"
        ".align 2\n"
        "blk_inc:\n"
        "  lis 4, counter@ha\n"
        "  lwz 4, counter@l(4)\n"
        "  add 3, 3, 4\n"
        "  blr\n"
        ".size blk_inc, .-blk_inc\n");
int blk_inc(int);

__attribute__((naked)) int naked_sub(int a, int b)
{
    __asm__("subf 3, 4, 3");
    __asm__("blr");
}

int bump_count;
void bump(void) { bump_count += 3; }
/* naked, and calling C: its own frame and LR save, the call written by
 * EmbCC */
__attribute__((naked)) void naked_bump(void)
{
    __asm__("stwu 1, -16(1)\n mflr 0\n stw 0, 20(1)");
    bump();
    __asm__("lwz 0, 20(1)\n mtlr 0\n addi 1, 1, 16\n blr");
}

__attribute__((noinline)) int seed(int k) { return k + counter - 40; }

/* lmw 29 writes r29, r30 and r31 and names only r29: the clobber list alone
 * keeps the values live across it -- eighteen, crossing calls, so they
 * want r14-r31 -- out of r30 and r31, and has the prologue save them */
int minus3[3] = { -1, -1, -1 };
#define V(n) int v##n = seed(k + n);
#define W(n) + n * v##n
__attribute__((noinline)) int lmw_clobbers(int k)
{
    V(1) V(2) V(3) V(4) V(5) V(6) V(7) V(8) V(9) V(10) V(11) V(12) V(13)
    V(14) V(15) V(16) V(17) V(18)
    __asm__ volatile("lmw 29, 0(%0)" :: "r"(minus3) : "r30", "r31", "memory");
    return 0 W(1) W(2) W(3) W(4) W(5) W(6) W(7) W(8) W(9) W(10) W(11) W(12)
             W(13) W(14) W(15) W(16) W(17) W(18);
}

int main(void)
{
    int n = seed(10), s = 0, v, w, mem = 1234, buf[4] = { 0, 0, 0, 0 };
    unsigned msr, x;
    unsigned char bytes[4] = { 0xc8, 0xfd, 0x80, 0x01 };
    unsigned word = 0x11223344u;
    int arr[3] = { 5, 30, 12 };

    /* MSR[EE] with interrupts off, and an SPRG */
    __asm__ volatile("wrteei 0\n mfmsr %0" : "=r"(msr));
    putn((msr >> 15) & 1);
    __asm__ volatile("mtsprg 1, %1\n mfsprg %0, 1" : "=r"(x) : "r"(seed(77)));
    putn(x);
    /* SPRG5 by its names: written at 277, read through its copy at 261 */
    __asm__ volatile("mtsprg5 %1\n mfsprg5 %0" : "=r"(x) : "r"(seed(78)));
    putn(x);
    __asm__ volatile("mtspr sprg6, %1\n mfspr %0, sprg6" : "=r"(x)
                     : "r"(seed(79)));
    putn(x);
    /* a CTR loop on numeric labels */
    __asm__("mtctr %1\n"
            "1: add %0, %0, %1\n"
            "   addi %1, %1, -1\n"
            "   bdnz 1b" : "+r"(s), "+r"(n) : : "ctr");
    putn(s);
    /* a record form deciding a branch, both ways */
    v = seed(7);
    __asm__("addic. %0, %0, -5\n bge 1f\n li %0, 0\n1:" : "+r"(v) : : "cr0", "xer");
    putn(v);
    v = seed(3);
    __asm__("addic. %0, %0, -5\n bge 1f\n li %0, 0\n1:" : "+r"(v) : : "cr0", "xer");
    putn(v);
    /* every operand kind */
    __asm__("lwz%U1%X1 %0, %1" : "=r"(v) : "m"(mem));
    putn(v);
    __asm__("stw%U0%X0 %1, %0" : "=m"(buf[1]) : "r"(seed(55)));
    putn(buf[1]);
    __asm__("addi %0, %1, %2" : "=r"(v) : "b"(seed(20)), "I"(-32768));
    putn(v);
    __asm__("ori %0, %1, %2" : "=r"(v) : "r"(seed(1)), "K"(0x8000));
    putn(v);
    __asm__("slwi %0, %0, 4" : "=r"(w) : "0"(seed(3)));
    putn(w);
    {
        register int a __asm__("r5") = seed(6), b __asm__("r20") = seed(9);
        __asm__("mullw %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
        putn(v);
    }
    __asm__("lis %0, %1@h\n ori %0, %0, %1@l" : "=r"(v) : "i"(0x7654321));
    putn(v);
    /* a call through CTR, which changes LR, r0 and r3-r12 */
    {
        int keep1 = seed(11), keep2 = seed(12), r;
        __asm__ volatile("mtctr %1\n mr 3, %2\n bctrl\n mr %0, 3"
                         : "=r"(r) : "r"(c_plus7), "r"(seed(30))
                         : "r0", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
                           "r10", "r11", "r12", "ctr", "lr", "cc", "memory");
        putn(r);
        putn(keep1 + keep2);
    }
    /* values live across a template that clobbers callee-saved registers,
     * and one that only names others, as bare numbers */
    {
        int a = seed(1), b = seed(2), c = seed(3), d = seed(4), e = seed(5);
        int f = seed(6), g = seed(7), h = seed(8);
        __asm__ volatile("li 14, -1\n li 15, -1\n li 16, -1\n li 17, -1\n"
                         "li 18, -1\n li 19, -1\n li 20, -1\n li 3, -1\n"
                         "li 4, -1"
                         ::: "r14", "r15", "r16", "r17", "r18", "r19", "r20",
                             "r3", "r4");
        __asm__ volatile("li 21, -2\n li 22, -2\n li 23, -2\n li 5, -2\n"
                         "li 6, -2\n li 7, -2\n li 8, -2");
        putn(a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f +
             1000000 * g + 10000000 * h);
    }
    putn(lmw_clobbers(0));
    puts_("\n");
    putn(pp_add(40, 2));
    putn(pp_call(1));
    putn(pp_sum(10));
    putn(pp_classify(10));
    putn(pp_classify(2));
    putn(pp_rec(52));
    putn(pp_rec(3));
    putn(pp_table() == table);
    putn(pp_load_tab(1));
    putn(pp_load_tab(0) == (int)&counter);
    putn(((int (*)(int, int))pp_load_tab(2))(5, 6));
    putn(pp_load_tab(3));
    putn(pp_mulhi(0x80000000u, 6));
    putn(pp_div(1000, 7));
    putn(pp_rlw(0x11223344u, 0xaabbccddu) == 0xaabb22ddu);
    putn(pp_min(-5, 3));
    putn(pp_min(9, 4));
    putn(pp_ee());
    putn(pp_sprg(41));
    putn(pp_lr());
    putn(pp_tail(5));
    putn(pp_indirect(c_plus7, 35));
    buf[0] = 5;
    putn(pp_cas(&buf[0], 5, 77));
    putn(buf[0]);
    putn(pp_cas(&buf[0], 5, 88));
    putn(buf[0]);
    putn(pp_bytes(bytes));
    putn(pp_brx(&word) == 0x44332211u);
    putn(pp_update(arr));
    putn(pp_notrap(3, 4));
    putn(halves[0] + halves[1]);
    putn(blk_inc(2));
    putn(naked_sub(50, 8));
    naked_bump();
    naked_bump();
    putn(bump_count);
    puts_("\n==END==\n");
    return 0;
}
