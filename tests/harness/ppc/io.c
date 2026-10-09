/* Output for the PowerPC harness: the first 16550 UART of QEMU ppce500's
 * CCSR block, at CCSR+0x4500 -- 0xE0004500 once boot.c has mapped the
 * block at 0xE0000000 -- its registers one byte apart. QEMU wires it to
 * the first serial port, so run.sh passes `-serial stdio`.
 *
 * The transmit-holding register takes a byte once the line status
 * register's THRE bit (5) says it is empty, which QEMU reports at once and
 * a real 16550 after the previous byte has gone. */
#define UART_THR (*(volatile unsigned char *)0xe0004500u)
#define UART_LSR (*(volatile unsigned char *)0xe0004505u)
/* The global utilities' reset control register: HRESET_REQ (2) resets the
 * board, which QEMU run with -no-reboot turns into an exit. */
#define RSTCR    (*(volatile unsigned *)0xe00e00b0u)

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
 * weak, and this one puts stdout and stderr on the UART. Weak itself, so a
 * program that brings its own write() links with this harness too. */
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
    RSTCR = 2;
    for (;;)
        ;
}
