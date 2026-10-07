/* The Renesas RX (QEMU gdbsim-r5f562n8) test harness: a startup and a way
 * to stop, in C.
 *
 * QEMU's -kernel copies a RAW image to the second half of the gdbsim's
 * SDRAM, 0x01800000, and starts the CPU there in supervisor mode with the
 * interrupts masked. The image is linked at that address and begins with
 * `embld -Tstack`'s stub -- mov.l #top, r0; mov.l #_start, r14; jmp r14 --
 * because C cannot set the stack pointer. Then this copies .data (which
 * moves nothing in a RAM image), zeroes .bss, runs the static
 * constructors, then main, and reports its result.
 *
 * The same loader points every fixed vector (undefined instruction,
 * privileged instruction, ...) at 0x10 + 4n in the internal RAM; each of
 * those words is made a `bra.a` to harness_fault, so a fault reports
 * itself instead of running whatever is there. A run that faults never
 * prints ==EXIT, so it cannot pass.
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
void harness_io_init(void);

static void harness_fault(void)
{
    puts_("\n==FAULT==\n");
    for (;;)
        ;
}

static void install_vectors(void)
{
    for (unsigned n = 0; n < 32; n++) {
        volatile unsigned char *p = (volatile unsigned char *)(0x10u + 4u * n);
        long d = (long)(unsigned long)harness_fault - (long)(0x10u + 4u * n);
        p[0] = 0x04;                              /* bra.a */
        p[1] = (unsigned char)d;
        p[2] = (unsigned char)(d >> 8);
        p[3] = (unsigned char)(d >> 16);
    }
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
    harness_io_init();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
