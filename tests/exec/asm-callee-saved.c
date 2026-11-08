/* An inline asm that clobbers callee-saved registers, which the function
 * holding it must save and restore -- as GCC does. EmbCC refused the
 * clobber on ARM, RISC-V, AArch64, MIPS, LoongArch and AVR ("EmbCC does
 * not save around an asm"), and on x86-64 accepted it and saved nothing,
 * so the caller's rbx and r12 came back destroyed. Here the callee
 * clobbers every callee-saved register the target lets an asm take
 * (not the frame pointer, which GCC keeps at -O0), by the clobber list
 * and by naming one in the template; the caller keeps twelve values
 * alive across the calls, which an allocator puts in exactly those
 * registers, and checks them after. */
// expect-exit: 42

#if defined(__x86_64__)
#define CLOBBER() __asm__ volatile("xorl %%ebx, %%ebx\n\txorl %%r12d, %%r12d\n\t" \
    "xorl %%r13d, %%r13d\n\txorl %%r14d, %%r14d\n\txorl %%r15d, %%r15d" \
    ::: "rbx", "r12", "r13", "r14", "r15")
#elif defined(__aarch64__)
#define CLOBBER() __asm__ volatile("mov x19, #0\n\tmov x20, #0\n\tmov x21, #0\n\t" \
    "mov x22, #0\n\tmov x23, #0\n\tmov x24, #0\n\tmov x25, #0\n\tmov x26, #0\n\t" \
    "mov x27, #0\n\tmov x28, #0" ::: "x19", "x20", "x21", "x22", "x23", \
    "x24", "x25", "x26", "x27", "x28")
#elif defined(__arm__) && (defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__))
#define CLOBBER() __asm__ volatile("movs r4, #0\n\tmovs r5, #0\n\tmovs r6, #0" \
    ::: "r4", "r5", "r6")
#elif defined(__arm__)
#define CLOBBER() __asm__ volatile("mov r4, #0\n\tmov r5, #0\n\tmov r6, #0\n\t" \
    "mov r8, #0\n\tmov r9, #0\n\tmov r10, #0" \
    ::: "r4", "r5", "r6", "r8", "r9", "r10")
#elif defined(__riscv)
#define CLOBBER() __asm__ volatile("li s1, 0\n\tli s2, 0\n\tli s3, 0\n\tli s4, 0\n\t" \
    "li s5, 0\n\tli s6, 0\n\tli s7, 0\n\tli s8, 0\n\tli s9, 0\n\tli s10, 0\n\t" \
    "li s11, 0" ::: "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", \
    "s10", "s11")
#elif defined(__mips__)
#define CLOBBER() __asm__ volatile("move $16, $0\n\tmove $17, $0\n\tmove $18, $0\n\t" \
    "move $19, $0\n\tmove $20, $0\n\tmove $21, $0\n\tmove $22, $0\n\t" \
    "move $23, $0" ::: "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23")
#elif defined(__loongarch__)
#define CLOBBER() __asm__ volatile("move $s0, $zero\n\tmove $s1, $zero\n\t" \
    "move $s2, $zero\n\tmove $s3, $zero\n\tmove $s4, $zero\n\tmove $s5, $zero\n\t" \
    "move $s6, $zero\n\tmove $s7, $zero\n\tmove $s8, $zero" ::: "$s0", "$s1", \
    "$s2", "$s3", "$s4", "$s5", "$s6", "$s7", "$s8")
#else
#define CLOBBER() ((void)0)
#endif

/* the template names a register too, which must be saved as a clobber is */
#if defined(__riscv)
#define NAMED(x) __asm__ volatile("mv s3, %0\n\tmv s3, zero" :: "r"(x))
#elif defined(__arm__) && !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
#define NAMED(x) __asm__ volatile("mov r5, %0\n\tmov r5, #0" :: "r"(x))
#else
#define NAMED(x) ((void)(x))
#endif

__attribute__((noinline)) static int victim(int x)
{
    CLOBBER();
    NAMED(x);
    return x + 1;
}

__attribute__((noinline)) static int seed(int k) { return k * 3 + 1; }

/* The FPU's callee-saved registers too: s16-s31 on an ARM with an FPU,
 * which EmbCC's vpush covered only for the values it allocated there --
 * an asm that changed s17 or d9 left the caller's value destroyed. The
 * caller sets s16-s19 by asm (and so saves them itself), calls the
 * victim, and reads them back. */
#if defined(__arm__) && defined(__ARM_FP) && !defined(__SOFTFP__)
#define FP_TEST 1
__attribute__((noinline)) static void fp_victim(void)
{
    __asm__ volatile("vmov s17, %0" :: "r"(0) : "s17");
    __asm__ volatile("vsub.f32 s18, s18, s18\n\tvsub.f32 s19, s19, s19" ::: "d9");
}

__attribute__((noinline)) static int fp_keep(unsigned v)
{
    unsigned a, b, c;
    __asm__ volatile("vmov s17, %0\n\tvmov s18, %0\n\tvmov s19, %0"
                     :: "r"(v) : "s17", "s18", "s19");
    fp_victim();
    __asm__ volatile("vmov %0, s17\n\tvmov %1, s18\n\tvmov %2, s19"
                     : "=r"(a), "=r"(b), "=r"(c) :: "s17", "s18", "s19");
    return a == v && b == v && c == v;
}
#endif

int main(void)
{
    int a = seed(1), b = seed(2), c = seed(3), d = seed(4), e = seed(5),
        f = seed(6), g = seed(7), h = seed(8), i = seed(9), j = seed(10),
        k = seed(11), l = seed(12);
    int s = 0;
    for (int n = 0; n < 3; n++)
        s += victim(n);                      /* 1 + 2 + 3 */
    s += victim(a) + victim(b);
    if (a != 4 || b != 7 || c != 10 || d != 13 || e != 16 || f != 19 ||
        g != 22 || h != 25 || i != 28 || j != 31 || k != 34 || l != 37)
        return 1;
    if (s != 6 + 5 + 8)
        return 2;
#ifdef FP_TEST
    if (!fp_keep(0x3fc00000u))
        return 3;
#endif
    return 42 + (a + b + c + d + e + f + g + h + i + j + k + l) - 246;
}
