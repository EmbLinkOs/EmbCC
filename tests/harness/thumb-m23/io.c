/* Output for the ARMv8-M Baseline harness: the mps2-an505's CMSDK UART0
 * (see ../thumb-m33/io.c), with the exit and fault reporting of
 * ../thumb-m0/io.c. write() and _exit() replace lib/libc's weak bare-metal
 * defaults, so printf reaches the UART and the exit status reaches run.sh. */
#define UART0_DR   (*(volatile unsigned *)0x40200000u)
#define UART0_CTRL (*(volatile unsigned *)0x40200008u)

void writec(int c)
{
    static int on;
    if (!on) { UART0_CTRL = 1u; on = 1; }     /* transmit enable */
    UART0_DR = (unsigned)c & 0xffu;
}

void puts_(const char *s)
{
    while (*s)
        writec(*s++);
}

void putn(long v)
{
    char b[16];
    int n = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    if (v < 0) writec('-');
    do { b[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (n)
        writec(b[--n]);
    writec(' ');
}

long write(int fd, const void *buf, unsigned long n)
{
    const char *p = buf;
    (void)fd;
    for (unsigned long k = 0; k < n; k++)
        writec(p[k]);
    return (long)n;
}

static void put_dec(int v)
{
    char b[12];
    int n = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    if (v < 0) writec('-');
    do { b[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u);
    while (n)
        writec(b[--n]);
}

void _exit(int status)
{
    puts_("\n==EXIT ");
    put_dec(status);
    puts_("==\n");
    for (;;)
        ;
}

/* HardFault (and NMI): an UNDEFINED instruction or an unaligned access.
 * Naked, so the exception frame is at msp: the stacked pc is its seventh
 * word. */
void harness_fault_c(unsigned *frame)
{
    unsigned pc = frame[6];
    puts_("\n==FAULT pc=");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(pc >> k) & 15u]);
    puts_("==");
    _exit(125);
}

__attribute__((naked)) void harness_fault(void)
{
    __asm__ volatile ("mrs r0, msp\n\tbl harness_fault_c");
}
