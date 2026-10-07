/* Output for the ColdFire harness: UART0 of QEMU's mcf5208evb, at
 * 0xfc060000, its registers four bytes apart -- USR (status) at +0x04, UCR
 * (command) at +0x08, UTB (transmit buffer) at +0x0c. QEMU wires it to the
 * first serial port, so run.sh passes `-serial stdio`.
 *
 * The transmitter is off at reset: the first byte enables it with the
 * TC_ENABLE command (UCR 0x04). A byte goes to UTB once USR's TxRDY bit
 * (0x04) says the buffer is free, which QEMU reports at once. */
#define UART_USR (*(volatile unsigned char *)0xfc060004u)
#define UART_UCR (*(volatile unsigned char *)0xfc060008u)
#define UART_UTB (*(volatile unsigned char *)0xfc06000cu)

static int tx_on;

void writec(int c)
{
    if (!tx_on) {
        UART_UCR = 0x04;
        tx_on = 1;
    }
    while (!(UART_USR & 0x04))
        ;
    UART_UTB = (unsigned char)c;
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

/* The end of the run: the sentinel, then a halt. The board has no reset
 * that ends QEMU, so the runner (tests/harness/qrun.sh --until) stops it
 * at the sentinel. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    for (;;)
        ;
}
