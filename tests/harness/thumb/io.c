/* Output for the ARMv7-M harness: the lm3s6965's UART0, which QEMU
 * wires to its own stdout. Writing a byte to the data register prints
 * it — no driver, no initialisation, because the reset defaults are
 * already what a `-nographic` QEMU reads.
 *
 * MMIO through a `volatile` pointer, which is the one thing the
 * optimizer must not touch: without it, a loop writing the same address
 * repeatedly is a loop whose writes all but the last look dead.
 */
#define UART0_DR (*(volatile unsigned *)0x4000C000u)

void writec(int c)
{
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
