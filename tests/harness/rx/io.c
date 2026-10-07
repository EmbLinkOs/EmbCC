/* Output for the RX harness: SCI0 of the RX62N, at 0x00088240, which QEMU
 * wires to the first serial port. A byte goes to TDR (+3) once SSR (+4)
 * says the transmit data register is empty (TDRE, bit 7); the transmitter
 * is enabled by SCR's TE bit (+2, bit 5). BRR 0 makes QEMU's per-byte
 * transmit time zero. */
#define SCI0_SMR (*(volatile unsigned char *)0x00088240u)
#define SCI0_BRR (*(volatile unsigned char *)0x00088241u)
#define SCI0_SCR (*(volatile unsigned char *)0x00088242u)
#define SCI0_TDR (*(volatile unsigned char *)0x00088243u)
#define SCI0_SSR (*(volatile unsigned char *)0x00088244u)

void harness_io_init(void)
{
    SCI0_SCR = 0;               /* SMR and BRR change only with TE clear */
    SCI0_SMR = 0;
    SCI0_BRR = 0;
    SCI0_SCR = 0x20;
}

void writec(int c)
{
    while (!(SCI0_SSR & 0x80))
        ;
    SCI0_TDR = (unsigned char)c;
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

/* lib/libc's output, when the program is linked with it. Weak, so a
 * program that brings its own write() links with this harness too. */
__attribute__((weak)) long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    (void)fd;
    for (unsigned long k = 0; k < n; k++)
        writec(p[k]);
    return (long)n;
}

/* The end of the run: the sentinel. Nothing in the gdbsim stops QEMU from
 * software, so the runner (tests/harness/qrun.sh --until) stops at it. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    for (;;)
        ;
}
