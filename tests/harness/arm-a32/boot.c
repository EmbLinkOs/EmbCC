/* The ARMv7-A (armv7a-none-eabi, QEMU virt) test harness: a startup, the
 * exception vectors, and a way to stop, in C.
 *
 * QEMU loads the image (-kernel) into RAM at its link address and starts
 * it at the ELF entry in ARM state, SVC mode, MMU and caches off. That
 * entry is `embld -Tstack`'s stub -- movw/movt sp, then a bx to _start --
 * because C cannot set the stack pointer. _start first turns the MMU on
 * with a flat map (see mmu_on: that is what lets a word load or store be
 * unaligned, which the code assumes as clang's does), then gives the abort and
 * undefined-instruction modes stacks of their own and points VBAR at a
 * vector table that reports the exception, copies .data (which moves
 * nothing in a RAM image), zeroes .bss, runs the static constructors, then
 * main, and reports its result.
 *
 * Built with -DHARNESS_LIBC for a program linked with lib/libc: then main's
 * result goes through exit(), so atexit handlers run and stdio is flushed
 * before the sentinel. */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);
void _exit(int status);
void puts_(const char *s);
void putn(long v);
void writec(int c);

static void puthex(unsigned v)
{
    puts_("0x");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

/* What went wrong, and where: the exception, the address it was taken
 * at (lr, less what the architecture adds for the kind), and for an
 * abort the fault status and address registers. Never `==EXIT`, so a run
 * that faults cannot pass. */
static void report(const char *what, unsigned lr, int back)
{
    unsigned dfsr, dfar, ifsr, ifar;
    __asm__ volatile("mrc p15, #0, %0, c5, c0, #0" : "=r"(dfsr));
    __asm__ volatile("mrc p15, #0, %0, c6, c0, #0" : "=r"(dfar));
    __asm__ volatile("mrc p15, #0, %0, c5, c0, #1" : "=r"(ifsr));
    __asm__ volatile("mrc p15, #0, %0, c6, c0, #2" : "=r"(ifar));
    puts_("\n==FAULT ");
    puts_(what);
    puts_(" at ");
    puthex(lr - (unsigned)back);
    puts_("dfsr ");
    puthex(dfsr);
    puts_("dfar ");
    puthex(dfar);
    puts_("ifsr ");
    puthex(ifsr);
    puts_("ifar ");
    puthex(ifar);
    puts_("==\n");
    _exit(126);
}

/* Each handler is entered in its exception's mode, with that mode's own
 * lr: the faulting address plus 4 or 8. Its first statement reads it --
 * the prologue saves lr and nothing before the read writes it. */
static void h_undef(void)  { unsigned lr; __asm__ volatile("mov %0, lr" : "=r"(lr)); report("undefined instruction", lr, 4); }
static void h_svc(void)    { unsigned lr; __asm__ volatile("mov %0, lr" : "=r"(lr)); report("svc", lr, 4); }
static void h_pabort(void) { unsigned lr; __asm__ volatile("mov %0, lr" : "=r"(lr)); report("prefetch abort", lr, 4); }
static void h_dabort(void) { unsigned lr; __asm__ volatile("mov %0, lr" : "=r"(lr)); report("data abort", lr, 8); }
static void h_irq(void)    { unsigned lr; __asm__ volatile("mov %0, lr" : "=r"(lr)); report("irq", lr, 4); }
static void h_fiq(void)    { unsigned lr; __asm__ volatile("mov %0, lr" : "=r"(lr)); report("fiq", lr, 4); }

/* The vector table: eight `ldr pc, [pc, #24]`, each loading its handler
 * from the word 32 bytes on. VBAR wants it 32-aligned. */
static unsigned vectors[16] __attribute__((aligned(32)));

static unsigned mode_stack[2][256];

/* Switch to `mode` (IRQ and FIQ masked), set its sp, and come back. */
static void mode_sp(unsigned mode, unsigned *top)
{
    __asm__ volatile("mrs r2, cpsr\n\t"
                     "mov r3, %0\n\t"
                     "orr r3, r3, #0xc0\n\t"
                     "msr cpsr_c, r3\n\t"
                     "mov sp, %1\n\t"
                     "msr cpsr_c, r2"
                     : : "r"(mode), "r"(top) : "r2", "r3", "memory");
}

static void install_vectors(void)
{
    void (*h[8])(void);
    h[0] = h_undef;  h[1] = h_undef; h[2] = h_svc; h[3] = h_pabort;
    h[4] = h_dabort; h[5] = h_undef; h[6] = h_irq; h[7] = h_fiq;
    for (int k = 0; k < 8; k++) {
        vectors[k] = 0xe59ff018u;            /* ldr pc, [pc, #24] */
        vectors[k + 8] = (unsigned)h[k];
    }
    mode_sp(0x17, &mode_stack[0][256]);      /* abort */
    mode_sp(0x1b, &mode_stack[1][256]);      /* undefined */
    __asm__ volatile("mcr p15, #0, %0, c12, c0, #0\n\tisb"
                     : : "r"(vectors) : "memory");
}

/* With the MMU off every data access is Strongly-ordered, and an
 * unaligned LDR/STR/LDRH/STRH takes an alignment fault whatever SCTLR.A
 * says -- QEMU models it. Code for ARMv7-A assumes they work
 * (__ARM_FEATURE_UNALIGNED: a packed struct's int member is one ldr), as
 * on any A-profile system that runs with its MMU on, so this one does: a
 * flat map of 1 MB sections, RAM (0x40000000-0x7fffffff) Normal
 * write-back, everything else Device and never executed. The table sits
 * at the top of the 128 MB of RAM, outside the image, so the .bss loop
 * cannot clear it from under the walk. */
#define TTB ((volatile unsigned *)0x47ff0000u)       /* 16 KB aligned */

static void mmu_on(void)
{
    unsigned sctlr;
    for (unsigned i = 0; i < 4096; i++) {
        unsigned normal = 0x2u | 0x4u | 0x8u | 0xc00u | (1u << 12);
        unsigned device = 0x2u | 0x4u | 0x10u | 0xc00u;
        TTB[i] = (i << 20) | (i >= 0x400 && i < 0x800 ? normal : device);
    }
    __asm__ volatile("mov r0, #1\n\t"
                     "mcr p15, #0, r0, c3, c0, #0\n\t"   /* DACR: client */
                     "mov r0, #0\n\t"
                     "mcr p15, #0, r0, c2, c0, #2\n\t"   /* TTBCR */
                     "mcr p15, #0, %0, c2, c0, #0\n\t"   /* TTBR0 */
                     "mcr p15, #0, r0, c8, c7, #0\n\t"   /* TLBIALL */
                     "dsb\n\tisb"
                     : : "r"(TTB) : "r0", "memory");
    __asm__ volatile("mrc p15, #0, %0, c1, c0, #0" : "=r"(sctlr));
    sctlr |= 1u;                       /* M */
    sctlr &= ~2u;                      /* A: unaligned accesses permitted */
    __asm__ volatile("mcr p15, #0, %0, c1, c0, #0\n\tisb"
                     : : "r"(sctlr) : "memory");
}

/* The FPU is off at reset: CPACR gives CP10 and CP11 (the VFP's two
 * coprocessor numbers) full access, and FPEXC.EN turns the unit on. Done
 * whether or not the program uses it -- a hard-float build's first vmov
 * would otherwise be an undefined instruction -- and harmless when it
 * does not. `vmsr fpexc, r0` is written as its word: the inline-asm
 * vocabulary has no FPEXC, which only a startup ever touches. */
__asm__(".type harness_fpu_on, %function\n"
        "harness_fpu_on:\n"
        "  mrc p15, #0, r0, c1, c0, #2\n"
        "  orr r0, r0, #0xf00000\n"
        "  mcr p15, #0, r0, c1, c0, #2\n"
        "  isb\n"
        "  mov r0, #0x40000000\n"
        "  .inst 0xeee80a10\n"
        "  bx lr\n"
        ".size harness_fpu_on, .-harness_fpu_on\n");
void harness_fpu_on(void);

#ifdef HARNESS_LIBC
void exit(int status);
#endif

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    int r;
    harness_fpu_on();
    mmu_on();
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    install_vectors();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
