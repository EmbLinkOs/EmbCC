/* LoongArch64 assembly in a C program, run on the virt board by
 * tests/golden/loongarch-asm.sh with forms.S assembled by EmbCC and by
 * clang: inline asm with operands by number, "+r", "m", an "I" constant,
 * register variables, a barrier, a privileged read (csrrd of CRMD: DA,
 * direct addressing, is how QEMU starts the image), the stable counter; a
 * file-scope asm function, a naked function, and calls both ways between
 * C and forms.S. Every value printed is fixed. */
void puts_(const char *s);
void putn(long v);
extern long asm_add(long a, long b);
extern long asm_call(long x);
extern long asm_glob(void);
extern long table[];
long counter = 40;
long c_twice(long x) { return 2 * x + counter; }
__asm__(".globl blk_inc\n"
        ".type blk_inc, @function\n"
        "blk_inc:\n"
        "  addi.d $a0, $a0, 1\n"
        "  ret\n");
long blk_inc(long);
__attribute__((naked)) long naked_sub(long a, long b)
{
    __asm__("sub.d $a0, $a0, $a1");
    __asm__("ret");
}
static int crmd_da(void)
{
    long crmd;
    __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
    return (crmd >> 3) & 1;
}
int main(void)
{
    long v, x = 77, y = 5;
    int w = 1000;
    putn(crmd_da());
    __asm__("add.d %0, %1, %2" : "=r"(v) : "r"(x), "r"(y));
    putn(v);
    __asm__("ld.d %0, %1" : "=r"(v) : "m"(x));
    putn(v);
    __asm__("addi.w %0, %0, %1" : "+r"(w) : "I"(-24));
    putn(w);
    __asm__("sub.d %[d], %[a], %[b]" : [d] "=r"(v) : [a] "r"(x), [b] "r"(y));
    putn(v);
    __asm__ volatile("dbar 0" ::: "memory");
    {
        register long a0 __asm__("a0") = 11;
        register long a1 __asm__("a1") = 31;
        __asm__("add.d %0, %0, %1" : "+r"(a0) : "r"(a1));
        putn(a0);
    }
    {
        unsigned long t1, t2;
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(t1));
        __asm__ volatile("rdtime.d %0, $zero" : "=r"(t2));
        putn(t2 >= t1);
    }
    putn(asm_add(40, 2));
    putn(asm_call(1));
    putn(asm_glob());
    putn(table[0] == (long)&counter);
    putn(table[1]);
    putn(blk_inc(41));
    putn(naked_sub(50, 8));
    puts_("\n==END==\n");
    return 0;
}
