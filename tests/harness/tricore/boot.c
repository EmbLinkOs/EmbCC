/* The TriCore (QEMU tricore_testboard) test harness: a startup and a way
 * to stop, in C.
 *
 * QEMU loads the image (-kernel) into the board's code RAM at its link
 * address and starts it at the ELF entry, which is `embld -Tstack`'s
 * stub: it sets A10, links the context-save areas (--csa) into the free
 * list every CALL draws from, turns call-depth counting off, and jumps
 * here -- with JI, not CALL, so this function must never return. Then
 * this copies .data (which moves nothing in a RAM image), zeroes .bss,
 * runs the static constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL --
 * `==EXIT n==` with main's result -- which the runner stops at
 * (tests/harness/qrun.sh --until); io.c's _exit prints it and then ends
 * QEMU through the board's test device. A trap reports itself
 * (install_traps).
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

/* A trap -- an illegal instruction, a bus or alignment error, a CSA list
 * run dry, a SYSCALL -- reports itself instead of running on: the board
 * starts with BTV 0, where there is no memory, and execution there reads
 * zeros, which are NOPs, until the timeout. So BTV is pointed at a table
 * of eight 32-byte vectors, one per trap class, each loading its class,
 * the trap number (D15 on entry) and the trapping address (A11) into the
 * argument registers and jumping to harness_trap. The report is followed
 * by the end of the run and never by `==EXIT`, so a run that traps cannot
 * pass. The vectors are written as words because they are written once,
 * here, and executed from RAM. */
static unsigned trap_tab[64] __attribute__((aligned(256)));

static void puthex(unsigned v)
{
    puts_("0x");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

void harness_trap(int cls, int tin, unsigned pc);
void harness_trap(int cls, int tin, unsigned pc)
{
    puts_("\n==TRAP class ");
    putn(cls);
    puts_("tin ");
    putn(tin);
    puts_("at ");
    puthex(pc);
    puts_("==\n");
    *(volatile unsigned *)0xf0000000u = 0xee;   /* the test device */
    for (;;)
        ;
}

static void install_traps(void)
{
    unsigned h = (unsigned)harness_trap;
    unsigned hi = ((h + 0x8000u) >> 16) & 0xffffu, lo = h & 0xffffu;
    for (unsigned c = 0; c < 8; c++) {
        unsigned *v = trap_tab + 8 * c;
        v[0] = 0x4000003bu | (c << 12);                 /* mov d4, class */
        v[1] = 0x51f0f00bu;                             /* mov d5, d15 */
        v[2] = 0x64c0b001u;                             /* mov.d d6, a11 */
        v[3] = 0xf0000091u | (hi << 12);                /* movh.a a15, hi */
        v[4] = 0x0000ffd9u | ((lo & 0x3fu) << 16) |     /* lea a15, [a15]lo */
               (((lo >> 10) & 0x3fu) << 22) | (((lo >> 6) & 0xfu) << 28);
        v[5] = 0x00300f2du;                             /* ji a15 */
    }
    __asm__ volatile("mtcr btv, %0\n isync" : : "d"(trap_tab));
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
    install_traps();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
