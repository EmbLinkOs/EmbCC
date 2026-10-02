/* Output for the ARMv8-M harness: the mps2-an505 board's CMSDK UART0, which QEMU
 * wires to its own stdout. Writing a byte to the data register prints
 * it — no driver, no initialisation, because the reset defaults are
 * already what a `-nographic` QEMU reads.
 *
 * MMIO through a `volatile` pointer, which is the one thing the
 * optimizer must not touch: without it, a loop writing the same address
 * repeatedly is a loop whose writes all but the last look dead.
 */
/* The CMSDK UART, unlike the an386's, does NOT accept a blind write:
 * its CTRL register gates the transmitter and comes up with it off, so a
 * program that just stores to DATA prints nothing and looks like a hang.
 * That is the whole difference between this file and the ARMv7-M one. */
#define UART0_DR   (*(volatile unsigned *)0x40200000u)
#define UART0_CTRL (*(volatile unsigned *)0x40200008u)

static void uart_init(void)
{
    UART0_CTRL = 1u;          /* bit 0: transmit enable */
}

void writec(int c)
{
    /* Enabled on the first write rather than from a start-up hook: this
     * harness has no init list, and the register is idempotent. */
    static int on;
    if (!on) { uart_init(); on = 1; }
    UART0_DR = (unsigned)c;
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
    if (v < 0) { writec('-'); v = -v; }
    do { b[n++] = (char)('0' + (int)(v - (v / 10) * 10)); v /= 10; } while (v);
    while (n)
        writec(b[--n]);
    writec(' ');
}
