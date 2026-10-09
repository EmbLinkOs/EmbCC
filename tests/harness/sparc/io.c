/* Output for the SPARC harness: GRLIB's APBUART on QEMU's leon3_generic,
 * at 0x80000100 -- the data register at +0, the status at +4 (bit 2: the
 * transmitter FIFO is empty) and the control at +8 (bit 1: transmitter
 * enable, which QEMU's boot loader sets and the harness sets again). */
#define UART_DATA (*(volatile unsigned *)0x80000100u)
#define UART_STAT (*(volatile unsigned *)0x80000104u)
#define UART_CTRL (*(volatile unsigned *)0x80000108u)

void harness_shutdown(void);

void harness_uart_init(void)
{
    UART_CTRL = UART_CTRL | 3u;          /* receiver and transmitter on */
}

void writec(int c)
{
    while (!(UART_STAT & 4u))
        ;
    UART_DATA = (unsigned)(unsigned char)c;
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

/* The end of the run: the sentinel, then the board stopped. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    harness_shutdown();
    for (;;)
        ;
}
