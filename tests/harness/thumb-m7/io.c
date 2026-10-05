/* Output for the Cortex-M7 harness: mps2-an500's CMSDK UART0 at
 * 0x40004000, the same block and address as mps2-an386's, which QEMU wires
 * to its stdout. Its transmitter is OFF at reset and CTRL bit 0 turns it
 * on; a store to DATA before that prints nothing at all.
 *
 * MMIO through `volatile`, so the optimizer keeps every store. */
#define UART0_DR   (*(volatile unsigned *)0x40004000u)
#define UART0_CTRL (*(volatile unsigned *)0x40004008u)

void writec(int c)
{
    static int on;
    if (!on) { UART0_CTRL = 1u; on = 1; }
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

/* The end of a run: main's value, as a line the runner can stop at
 * (tests/harness/qrun.sh --until "==EXIT"). Weak, so exec.c can put
 * lib/libc's exit() in front of it. */
__attribute__((weak)) void harness_exit(int status)
{
    puts_("\n==EXIT ");
    putn(status);
    puts_("==\n");
    for (;;)
        ;
}
