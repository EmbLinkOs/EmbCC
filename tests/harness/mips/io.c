/* Output for the MIPS harness: the malta FPGA's 16550 UART, at physical
 * 0x1f000900 -- 0xbf000900 in KSEG1, the uncached window -- with its
 * registers eight bytes apart. QEMU wires it to the THIRD serial port, so
 * run.sh passes `-serial null -serial null -serial stdio`.
 *
 * The transmit-holding register takes a byte once the line status
 * register's THRE bit (5) says it is empty, which QEMU reports at once and
 * a real 16550 after the previous byte has gone. */
/* KSEG: a KSEG0/KSEG1 address as a pointer -- itself on MIPS32, and its
 * sign extension on MIPS64 (n64, tests/harness/mips64), where the same
 * windows are 0xffffffff80000000 up. */
#define KSEG(a)  ((unsigned long)(long)(int)(a))
#define UART_THR (*(volatile unsigned char *)KSEG(0xbf000900u))
#define UART_LSR (*(volatile unsigned char *)KSEG(0xbf000928u))
/* The FPGA's SOFTRES register: 0x42 resets the board, which QEMU run with
 * -no-reboot turns into an exit. */
#define SOFTRES  (*(volatile unsigned *)KSEG(0xbf000500u))

void writec(int c)
{
    while (!(UART_LSR & 0x20))
        ;
    UART_THR = (unsigned char)c;
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
 * a program that brings its own write() (tests/golden/embedded-libc.c)
 * links with this harness too. */
__attribute__((weak)) long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    (void)fd;
    for (unsigned long k = 0; k < n; k++)
        writec(p[k]);
    return (long)n;
}

/* The end of the run: the sentinel, then the board's reset. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    SOFTRES = 0x42;
    for (;;)
        ;
}
