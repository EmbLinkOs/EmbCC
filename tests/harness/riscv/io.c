/* Output for the RISC-V harness: QEMU `virt`'s 16550A UART at
 * 0x10000000, which QEMU wires to its own stdout. Writing a byte to the
 * transmit-holding register prints it -- no driver and no
 * initialisation, because the reset defaults are already what a
 * `-nographic` QEMU reads.
 *
 * MMIO through a `volatile` pointer, which is the one thing the
 * optimizer must not touch: without it, a loop writing the same address
 * repeatedly is a loop whose writes all but the last look dead.
 */
#define UART0_THR (*(volatile unsigned char *)0x10000000u)

void writec(int c)
{
    UART0_THR = (unsigned char)c;
}

void puts_(const char *s)
{
    while (*s)
        writec(*s++);
}

/* The trailing space is part of the interface, not decoration: the same
 * programs run against the host's hostio.c, whose putn is a
 * printf("%ld ") -- so the two must agree character for character, or
 * every comparison fails on whitespace rather than on arithmetic. */
void putn(long v)
{
    char b[24];
    int n = 0;
    if (v < 0) { writec('-'); v = -v; }
    do { b[n++] = (char)('0' + (int)(v - (v / 10) * 10)); v /= 10; } while (v);
    while (n)
        writec(b[--n]);
    writec(' ');
}
