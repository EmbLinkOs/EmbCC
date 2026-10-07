/* The ColdFire (m68k-none-elf, QEMU mcf5208evb) test harness: a startup
 * and a way to stop, in C.
 *
 * QEMU loads the image (-kernel) at its link address and starts at the
 * ELF entry, which is `embld -Tstack`'s stub: the stack pointer set and a
 * jmp to _start, because C cannot set the stack pointer. This copies
 * .data (which moves nothing in a RAM image), zeroes .bss, points VBR at
 * a vector table whose every entry reports the exception, runs the
 * static constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL on
 * the UART -- `==EXIT n ==` with main's result -- which the runner stops
 * at (tests/harness/qrun.sh --until).
 *
 * The one privileged instruction this needs -- movec to VBR -- is written
 * as instruction WORDS into a small buffer and called, because EmbCC has
 * no ColdFire assembler: move.l 4(%sp),%d0 (0x202f 0004, refereed by
 * tests/golden/coldfire-encoding.sh), movec %d0,%vbr (0x4e7b 0801) and
 * rts (0x4e75). QEMU notices code written to memory.
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
void writec(int c);

static unsigned short vbr_code[5] = {
    0x202f, 0x0004, 0x4e7b, 0x0801, 0x4e75
};

/* The vector table, at the start of SDRAM: the image is linked 1 MiB
 * above it, and a ColdFire VBR names a 1 MiB boundary. */
#define VECTORS ((unsigned *)0x40000000u)

static void puthex(unsigned v)
{
    puts_("0x");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

/* Every exception lands here, with the exception frame where a call's
 * return address and first argument would be: the format word (format,
 * vector, sr) at 4(%fp) -- __builtin_return_address(0) -- and the
 * faulting pc at 8(%fp), the first parameter. The report is never
 * followed by `==EXIT`, so a run that faults cannot pass. */
void harness_fault(unsigned pc)
{
    unsigned fv = (unsigned)__builtin_return_address(0);
    unsigned vec = (fv >> 18) & 0xff;
    puts_("\n==FAULT vector ");
    writec('0' + (int)(vec / 100));
    writec('0' + (int)(vec / 10 % 10));
    writec('0' + (int)(vec % 10));
    puts_(" pc ");
    puthex(pc);
    puts_("sr ");
    puthex(fv & 0xffff);
    puts_("==\n");
    for (;;)
        ;
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
    for (int k = 2; k < 256; k++)
        VECTORS[k] = (unsigned)harness_fault;
    ((void (*)(unsigned))(void *)vbr_code)((unsigned)VECTORS);
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
