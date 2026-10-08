/* TriCore assembly with C, run on QEMU's tricore_testboard by
 * tests/golden/tricore-gas.sh: forms.S's functions called from C and
 * calling C, a file-scope asm function with an address from hi:/lo:, a
 * naked function, and inline templates with numeric labels and branches.
 * Every value printed is fixed. */
void puts_(const char *s);
void putn(long v);
extern int tc_add(int a, int b);
extern int tc_call(int x);
extern int tc_sum(int n);
extern int tc_classify(int x);
extern int *tc_table(void);
extern int tc_jl(void);
extern int tc_tail(int x);
extern int table[];
int counter = 40;
int c_twice(int x) { return 2 * x + counter; }

__asm__(".global blk_inc\n"
        ".type blk_inc, @function\n"
        ".align 2\n"
        "blk_inc:\n"
        "  movh.a a2, hi:counter\n"
        "  lea a2, [a2]lo:counter\n"
        "  ld.w d2, [a2]0\n"
        "  add d2, d2, d4\n"
        "  ret\n");
int blk_inc(int);

__attribute__((naked)) int naked_sub(int a, int b)
{
    __asm__("sub d2, d4, d5");
    __asm__("ret");
}

__attribute__((noinline)) int seed(int k) { return k + counter - 40; }

int main(void)
{
    int n = seed(10), s = 0, v;
    __asm__("1: add %0, %0, %1\n"
            "   addi %1, %1, -1\n"
            "   jnz %1, 1b" : "+d"(s), "+d"(n));
    putn(s);
    v = seed(7);
    __asm__("jge %0, 5, 1f\n"
            "   mov %0, 0\n"
            "1: addi %0, %0, 100" : "+d"(v));
    putn(v);
    v = seed(3);
    __asm__("jlt %0, 5, .+8\n"
            "   mov %0, 0\n"
            "   addi %0, %0, 100" : "+d"(v));
    putn(v);
    puts_("\n");
    putn(tc_add(40, 2));
    putn(tc_call(1));
    putn(tc_sum(10));
    putn(tc_classify(10));
    putn(tc_classify(0));
    putn(tc_table() == table);
    putn(tc_jl());
    putn(tc_tail(5));
    putn(table[0] == (int)&counter);
    putn(table[1]);
    putn(((int (*)(int, int))table[2])(1, 2));
    putn(blk_inc(2));
    putn(naked_sub(50, 8));
    puts_("\n==END==\n");
    return 0;
}
