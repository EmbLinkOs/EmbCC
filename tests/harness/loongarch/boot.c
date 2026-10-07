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
 * reset) -- reports itself instead of hanging. EmbCC has no LoongArch
 * inline assembler, so the few privileged instructions this needs are
 * written as words into a page of RAM and called there: `csrwr a0,
 * EENTRY` and `csrrd a0, ESTAT/ERA/BADV`, each followed by `ret`, and at
 * the page's start the exception entry itself, a jump to harness_fault.
 * EENTRY's low twelve bits are zero, hence the page alignment. The
 * report is followed by the power-off and never by `==EXIT`, so a run
 * that faults cannot pass. */
static unsigned int page[1024] __attribute__((aligned(4096)));

typedef unsigned long (*csrfn)(unsigned long);

static void puthex(unsigned long v)
{
    puts_("0x");
    for (int k = 60; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

static void harness_fault(void)
{
    unsigned long estat = ((csrfn)(void *)&page[6])(0);
    unsigned long era = ((csrfn)(void *)&page[8])(0);
    unsigned long badv = ((csrfn)(void *)&page[10])(0);
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
    unsigned long h = (unsigned long)harness_fault;
    volatile unsigned int *v = page;
    /* the entry: lu12i.w t0, h >> 12; ori t0, t0, h & 0xfff; jr t0 --
     * the image sits below 2 GiB, so two instructions build h */
    v[0] = 0x14000000u | (unsigned)(((h >> 12) & 0xfffff) << 5) | 12u;
    v[1] = 0x03800000u | (unsigned)((h & 0xfff) << 10) | (12u << 5) | 12u;
    v[2] = 0x4c000000u | (12u << 5);
    v[4] = 0x04003024u;  v[5] = 0x4c000020u;      /* csrwr a0, EENTRY; ret */
    v[6] = 0x04001404u;  v[7] = 0x4c000020u;      /* csrrd a0, ESTAT; ret */
    v[8] = 0x04001804u;  v[9] = 0x4c000020u;      /* csrrd a0, ERA; ret */
    v[10] = 0x04001c04u; v[11] = 0x4c000020u;     /* csrrd a0, BADV; ret */
    ((csrfn)(void *)&page[4])((unsigned long)page);
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
