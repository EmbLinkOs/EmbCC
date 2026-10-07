/* Output for the LoongArch64 harness: the virt board's first 16550 UART,
 * at 0x1fe001e0 (QEMU's `info mtree`), one byte register apart; it is the
 * first serial port, so run.sh passes `-serial stdio`.
 *
 * The transmit-holding register takes a byte once the line status
 * register's THRE bit (5) says it is empty, which QEMU reports at once and
 * a real 16550 after the previous byte has gone. */
#define UART_THR (*(volatile unsigned char *)0x1fe001e0UL)
#define UART_LSR (*(volatile unsigned char *)0x1fe001e5UL)
/* The ACPI GED's sleep-control register: SLP_EN | SLP_TYP S5 powers the
 * board off, and QEMU exits. */
#define GED_SLEEP_CTL (*(volatile unsigned char *)0x100e001cUL)

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

void harness_poweroff(void)
{
    GED_SLEEP_CTL = 0x34;          /* SLP_EN (0x20) | SLP_TYP 5 << 2 */
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

/* The end of the run: the sentinel, then the power-off. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    harness_poweroff();
    for (;;)
        ;
}
