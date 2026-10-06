/* The MIPS (mipsel, QEMU malta) test harness: a startup and a way to stop,
 * in C.
 *
 * QEMU loads the image (-kernel) into RAM at its link address in KSEG0 and
 * starts it from its own reset-vector code at 0xbfc00000, which jumps to
 * the ELF entry. That entry is `embld -Tstack`'s stub -- li sp, then a jr
 * to _start through t9 -- because C cannot set the stack pointer. Then
 * this copies .data (which moves nothing in a RAM image), zeroes .bss,
 * runs the static constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL on
 * the UART -- `==EXIT n==` with main's result -- which the runner stops at
 * (tests/harness/qrun.sh --until). io.c's _exit prints it and then asks
 * the board to reset, which with -no-reboot ends QEMU at once.
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

/* An exception -- an address error from a misaligned access, a reserved
 * instruction, a `break` -- reports itself instead of hanging. The
 * processor comes out of reset with Status.BEV set, which sends
 * exceptions to the boot flash at 0xbfc00380; clearing it sends them to
 * EBase + 0x180, 0x80000180, where four words jump here through k0 (the
 * register the ABI keeps for exactly this). QEMU models no caches, so the
 * words need no flush. The report is followed by the board's reset and
 * never by `==EXIT`, so a run that faults cannot pass. */
#define SOFTRES (*(volatile unsigned *)0xbf000500u)

void writec(int c);

static void puthex(unsigned v)
{
    puts_("0x");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

static void harness_fault(void)
{
    unsigned cause, epc, bad;
    __asm__ volatile("mfc0 %0, $13" : "=r"(cause));
    __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
    __asm__ volatile("mfc0 %0, $8" : "=r"(bad));
    puts_("\n==FAULT cause ");
    putn((long)((cause >> 2) & 31));
    puts_("epc ");
    puthex(epc);
    puts_("badvaddr ");
    puthex(bad);
    puts_("==\n");
    SOFTRES = 0x42;
    for (;;)
        ;
}

static void install_vectors(void)
{
    volatile unsigned *v = (volatile unsigned *)0x80000180u;
    unsigned h = (unsigned)harness_fault, status;
    v[0] = 0x3c1a0000u | (h >> 16);           /* lui  k0, %hi */
    v[1] = 0x375a0000u | (h & 0xffffu);       /* ori  k0, k0, %lo */
    v[2] = 0x03400008u;                       /* jr   k0 */
    v[3] = 0;                                 /* nop */
    __asm__ volatile("mfc0 %0, $12" : "=r"(status));
    status &= ~((1u << 22) | (1u << 2) | (1u << 1));    /* BEV, ERL, EXL */
    __asm__ volatile("mtc0 %0, $12; ehb" : : "r"(status));
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
