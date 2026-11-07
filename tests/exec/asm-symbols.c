/* Inline asm that names symbols, RUN: a template's relocations reach the
 * object (src/as/gas.c's inline mode hands them to the writer).
 *
 *   - addr(): the address of the global `g`, loaded by the template the
 *     way the target's own code does it (adrp/add, movw/movt, la, lui/ori,
 *     lea), must be &g;
 *   - call(): a call from the template (bl, call, jal) to `answer`, a
 *     routine in a file-scope block that returns 42 and changes nothing
 *     else, with the link register declared clobbered;
 *   - local(): a template that addresses a label of its own (RISC-V's la
 *     pairs the auipc's label; elsewhere a branch over a word) and reads
 *     the word there.
 *
 * Elsewhere the three are written in C, so every board runs the file. */
// expect-exit: 0

long g = 7;

#if defined(__aarch64__)
__asm__(".text\n.global answer\n.type answer, %function\nanswer:\n"
        " mov w0, #42\n ret\n");
static long addr(void)
{
    long r;
    __asm__("adrp %x0, g\n add %x0, %x0, :lo12:g" : "=r"(r));
    return r;
}
__attribute__((noinline)) static int call(void)
{
    register int r __asm__("w0");
    __asm__ volatile("bl answer" : "=r"(r) : : "x30");
    return r;
}
static long local(void)
{
    long r;
    __asm__("adr %x0, 1f\n ldr %w0, [%x0]\n b 2f\n"
            "1: .word 99\n2:" : "=r"(r));
    return r;
}
#elif defined(__thumb__)
__asm__(".text\n.global answer\n.type answer, %function\n.thumb_func\n"
        "answer:\n movs r0, #42\n bx lr\n");
#if __ARM_ARCH_ISA_THUMB >= 2
static long addr(void)
{
    long r;
    __asm__("movw %0, #:lower16:g\n movt %0, #:upper16:g" : "=r"(r));
    return r;
}
#else
/* (ARMv6-M has no movw/movt, and a literal pool is refused: the call) */
static long addr(void) { return (long)&g; }
#endif
__attribute__((noinline)) static int call(void)
{
    register int r __asm__("r0");
    __asm__ volatile("bl answer" : "=r"(r) : : "lr");
    return r;
}
static long local(void) { return 99; }
#elif defined(__arm__)
__asm__(".text\n.global answer\n.type answer, %function\n"
        "answer:\n mov r0, #42\n bx lr\n");
static long addr(void)
{
    long r;
    __asm__("movw %0, #:lower16:g\n movt %0, #:upper16:g" : "=r"(r));
    return r;
}
__attribute__((noinline)) static int call(void)
{
    register int r __asm__("r0");
    __asm__ volatile("bl answer" : "=r"(r) : : "lr");
    return r;
}
static long local(void) { return 99; }
#elif defined(__riscv)
__asm__(".text\n.global answer\n.type answer, @function\nanswer:\n"
        " li a0, 42\n ret\n");
static long addr(void)
{
    long r;
    __asm__("la %0, g" : "=r"(r));
    return r;
}
__attribute__((noinline)) static int call(void)
{
    register int r __asm__("a0");
    __asm__ volatile("call answer" : "=r"(r) : : "ra");
    return r;
}
static long local(void)
{
    long r;
    __asm__("la %0, 1f\n lw %0, 0(%0)\n j 2f\n .p2align 2\n"
            "1: .word 99\n2:" : "=r"(r));
    return r;
}
#elif defined(__x86_64__)
/* (no call: a template's call would write below a leaf's red zone) */
static long addr(void)
{
    long r;
    __asm__("leaq g(%%rip), %0" : "=r"(r));
    return r;
}
static int call(void) { return 42; }
static long local(void) { return 99; }
#elif defined(__mips__)
__asm__(".text\n.set noreorder\n.global answer\n.type answer, @function\n"
        "answer:\n jr $31\n li $2, 42\n.set reorder\n");
static long addr(void)
{
    long r;
#if defined(__mips64)
    __asm__("lui %0, %%highest(g)\n daddiu %0, %0, %%higher(g)\n"
            " dsll %0, %0, 16\n daddiu %0, %0, %%hi(g)\n"
            " dsll %0, %0, 16\n daddiu %0, %0, %%lo(g)" : "=r"(r));
#else
    __asm__("la %0, g" : "=r"(r));
#endif
    return r;
}
__attribute__((noinline)) static int call(void)
{
    register int r __asm__("$2");
    __asm__ volatile(".set noreorder\n jal answer\n nop\n .set reorder"
                     : "=r"(r) : : "$31");
    return r;
}
static long local(void) { return 99; }
#elif defined(__loongarch__)
__asm__(".text\n.global answer\n.type answer, @function\nanswer:\n"
        " ori $a0, $zero, 42\n jirl $zero, $ra, 0\n");
static long addr(void)
{
    long r;
    __asm__("pcalau12i %0, %%pc_hi20(g)\n addi.d %0, %0, %%pc_lo12(g)"
            : "=r"(r));
    return r;
}
__attribute__((noinline)) static int call(void)
{
    register int r __asm__("$a0");
    __asm__ volatile("bl answer" : "=r"(r) : : "$ra");
    return r;
}
static long local(void) { return 99; }
#else
static long addr(void) { return (long)&g; }
static int call(void) { return 42; }
static long local(void) { return 99; }
#endif

int main(void)
{
    if (addr() != (long)&g)
        return 1;
    if (*(long *)addr() != 7)
        return 2;
    if (call() != 42)
        return 3;
    if (local() != 99)
        return 4;
    return 0;
}
