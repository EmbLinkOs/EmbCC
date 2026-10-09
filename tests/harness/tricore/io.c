/* Output for the TriCore harness. QEMU's tricore_testboard has no UART,
 * so a byte is stored to UART_TX, a word at the top of the board's
 * internal data RAM that nothing else uses, and putc.c -- a TCG plugin
 * run.sh loads -- prints every byte stored there. */
#define UART_TX  (*(volatile unsigned char *)0xd000bff0u)
/* The board's test device: a word written here ends QEMU with that word
 * as its exit status. */
#define TESTDEV  (*(volatile unsigned *)0xf0000000u)

void writec(int c)
{
    UART_TX = (unsigned char)c;
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

/* The end of the run: the sentinel, then the test device. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    TESTDEV = (unsigned)(status & 0xff);
    for (;;)
        ;
}
