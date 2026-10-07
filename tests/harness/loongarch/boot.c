/* The LoongArch64 (QEMU virt) test harness: a startup and a way to stop,
 * in C.
 *
 * QEMU loads the image (-kernel) into RAM at its link address and starts
 * it in direct address mode at the ELF entry. That entry is `embld
 * -Tstack`'s stub -- li sp, then a jump to _start -- because C cannot set
 * the stack pointer. Then this copies .data (which moves nothing in a RAM
 * image), zeroes .bss, installs an exception reporter, runs the static
 * constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL on
 * the UART -- `==EXIT n ==` with main's result -- which the runner stops at
 * (tests/harness/qrun.sh --until). io.c's _exit prints it and then powers
 * the board off through the ACPI GED, which ends QEMU at once.
 *
 * Built with -DHARNESS_LIBC for a program linked with lib/libc: then main's
 * result goes through exit(), so atexit handlers run and stdio is flushed
 * before the sentinel. */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
/* The static constructors (`__attribute__((constructor))`), which embld
 * gathers into .init_array between these two symbols. */
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);
void _exit(int status);
void puts_(const char *s);
void putn(long v);
void writec(int c);
void harness_poweroff(void);

/* An exception -- an unaligned atomic, a reserved instruction, a `break`,
 * a floating-point instruction in soft-float code (the FPU is disabled at
 * reset) -- reports itself instead of hanging. EENTRY's low twelve bits
 * are zero, so the entry is a `b harness_fault` written into a page of
 * RAM aligned to 4 KiB (a block in .text has no such alignment); the CSRs
 * are read and written with inline asm.
 * The report is followed by the power-off and never by `==EXIT`, so a
 * run that faults cannot pass. */
void harness_fault(void);
static unsigned int vector_page[1024] __attribute__((aligned(4096)));

static void puthex(unsigned long v)
{
    puts_("0x");
    for (int k = 60; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

void harness_fault(void)
{
    unsigned long estat, era, badv;
    __asm__ volatile("csrrd %0, 0x5" : "=r"(estat));     /* ESTAT */
    __asm__ volatile("csrrd %0, 0x6" : "=r"(era));       /* ERA */
    __asm__ volatile("csrrd %0, 0x7" : "=r"(badv));      /* BADV */
    puts_("\n==FAULT ecode ");
    putn((long)((estat >> 16) & 63));
    puts_("subcode ");
    putn((long)((estat >> 22) & 511));
    puts_("era ");
    puthex(era);
    puts_("badv ");
    puthex(badv);
    puts_("==\n");
    harness_poweroff();
    for (;;)
        ;
}

static void install_vectors(void)
{
    unsigned long e = (unsigned long)vector_page;
    long off = (long)((unsigned long)harness_fault - e);
    /* b off: the word offset's low 16 bits at 25:10, its high 10 at 9:0 */
    vector_page[0] = 0x50000000u | (unsigned)(((off >> 2) & 0xffff) << 10) |
                     (unsigned)((off >> 18) & 0x3ff);
    __asm__ volatile("ibar 0" : : : "memory");
    __asm__ volatile("csrwr %0, 0xc" : "+r"(e) : : "memory");  /* EENTRY */
}

#ifdef HARNESS_LIBC
void exit(int status);
#endif

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    int r;
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
