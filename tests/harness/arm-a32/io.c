/* Output for the ARMv7-A harness: QEMU virt's PL011 UART at 0x09000000,
 * which QEMU wires to stdout. Writing a byte to the data register prints
 * it; QEMU's model needs no initialisation, and the flag register's TXFF
 * bit (5) says when a real one is full.
 *
 * The end of a run is the sentinel `==EXIT n ==`, then the semihosting
 * exit: QEMU run with -semihosting takes `svc #0x123456` in ARM state as a
 * request to its host, and SYS_EXIT_EXTENDED ends QEMU with the status --
 * so a run costs what the program takes, not the runner's timeout. */
#define UART_DR (*(volatile unsigned *)0x09000000u)
#define UART_FR (*(volatile unsigned *)0x09000018u)

void writec(int c)
{
    while (UART_FR & (1u << 5))
        ;
    UART_DR = (unsigned)(unsigned char)c;
}

void puts_(const char *s)
{
    while (*s)
        writec(*s++);
}

/* The trailing space is part of the interface: the same programs run on
 * the host against hostio.c, whose putn is printf("%ld "). */
void putn(long v)
{
    char b[24];
    int n = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    if (v < 0)
        writec('-');
    do { b[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (n)
        writec(b[--n]);
    writec(' ');
}

/* lib/libc's output, when the program is linked with it: its write() is
 * weak, and this one puts stdout and stderr on the UART. Weak itself, so
 * a program that brings its own write() links with this harness too. */
__attribute__((weak)) long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    (void)fd;
    for (unsigned long k = 0; k < n; k++)
        writec(p[k]);
    return (long)n;
}

/* SYS_EXIT_EXTENDED (0x20): r1 points at {reason, status}, the reason
 * ADP_Stopped_ApplicationExit. */
static void semihost_exit(int status)
{
    unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = (unsigned)status;
    __asm__ volatile("mov r0, #0x20\n\tmov r1, %0\n\tsvc #0x123456"
                     : : "r"(blk) : "r0", "r1", "memory");
}

void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    semihost_exit(status & 0xff);
    for (;;)
        ;
}
