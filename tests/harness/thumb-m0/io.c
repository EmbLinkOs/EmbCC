/* Output for the ARMv6-M harness: the nRF51's UART0, which QEMU wires to
 * its stdout. QEMU's model (hw/char/nrf51_uart.c) sends a byte written to
 * TXD only once the peripheral is ENABLEd (4) and its STARTTX task has
 * been triggered, and raises EVENTS_TXDRDY when the byte has gone -- so
 * each byte waits for that and clears it, as on the real part.
 *
 * write() and _exit() replace lib/libc's weak bare-metal defaults
 * (lib/libc/os/baremetal/backend.c), so printf reaches the UART and a
 * program's exit status reaches run.sh.
 */
#ifdef HARNESS_LM3S
/* The same harness on the Cortex-M3 board (lm3s6965evb, UART0 at
 * 0x4000C000, nothing to set up): the reference the ARMv6-M exec suite
 * runs each program's ARMv7-M build on (run-m3.sh). */
#define UART0_DR (*(volatile unsigned *)0x4000C000u)
void uart_init(void) { }
void writec(int c) { UART0_DR = (unsigned)c & 0xffu; }
#else
#define UART 0x40002000u
#define REG(off) (*(volatile unsigned *)(UART + (off)))
#define TASKS_STARTTX 0x008u
#define EVENTS_TXDRDY 0x11cu
#define ENABLE        0x500u
#define TXD           0x51cu

void uart_init(void)
{
    REG(ENABLE) = 4;
    REG(TASKS_STARTTX) = 1;
}

void writec(int c)
{
    REG(TXD) = (unsigned)c & 0xffu;
    while (!REG(EVENTS_TXDRDY))
        ;
    REG(EVENTS_TXDRDY) = 0;
}
#endif

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

/* The end of a run: the status, then a permanent stop. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    put_dec(status);
    puts_("==\n");
    for (;;)
        ;
}

/* NMI and HardFault: an undefined instruction or an unaligned access,
 * which on this core are the two ways wrong code goes wrong. The handler
 * is naked so that the exception frame is at msp when it runs: the
 * stacked pc is its seventh word (r0-r3, r12, lr, pc, xPSR). */
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
