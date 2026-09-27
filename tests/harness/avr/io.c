/* Output for the AVR harness: the ATmega328P's USART0, which QEMU's
 * arduino-uno machine wires to its own serial port.
 *
 * Real registers at real addresses, because AVR's I/O space is mapped
 * into the DATA address space at 0x20 + the I/O number -- so a volatile
 * pointer reaches it and no special instruction is needed. That is the
 * one place the Harvard split works in this file's favour.
 *
 * The baud rate matters to hardware and not to QEMU, which passes bytes
 * through regardless; it is set anyway so that the same file runs on a
 * real Nano at 9600.
 */
#define UCSR0A (*(volatile unsigned char *)0xC0u)
#define UCSR0B (*(volatile unsigned char *)0xC1u)
#define UCSR0C (*(volatile unsigned char *)0xC2u)
#define UBRR0L (*(volatile unsigned char *)0xC4u)
#define UBRR0H (*(volatile unsigned char *)0xC5u)
#define UDR0   (*(volatile unsigned char *)0xC6u)

#define UDRE0  5      /* UCSR0A: the data register is empty */
#define TXEN0  3      /* UCSR0B: transmitter enable */

static void uart_init(void)
{
    /* 16 MHz / (16 * 9600) - 1 = 103 */
    UBRR0H = 0;
    UBRR0L = 103;
    UCSR0B = (unsigned char)(1u << TXEN0);
    /* 8 data bits, no parity, one stop bit: UCSZ01|UCSZ00 = bits 2:1. */
    UCSR0C = (unsigned char)((1u << 2) | (1u << 1));
}

void writec(int c)
{
    /* Initialised on the first write rather than from a start-up hook:
     * this harness has no init list, and the registers are idempotent. */
    static int on;
    if (!on) { uart_init(); on = 1; }
    while (!(UCSR0A & (unsigned char)(1u << UDRE0)))
        ;
    UDR0 = (unsigned char)c;
}

void puts_(const char *s)
{
    while (*s)
        writec(*s++);
}

void putn(long v)
{
    /* No divide: this backend has none yet. But not repeated subtraction
     * of ten either -- that is O(v) and 50529027 took long enough for a
     * QEMU test to time out and look exactly like a miscompile. One
     * digit at a time against the powers of ten is at most nine
     * subtractions per digit, which is 90 for the widest value a
     * four-byte long holds. */
    static const long p10[10] = {
        1000000000L, 100000000L, 10000000L, 1000000L, 100000L,
        10000L, 1000L, 100L, 10L, 1L
    };
    int i, started = 0;
    if (v < 0) { writec('-'); v = -v; }
    for (i = 0; i < 10; i++) {
        int d = 0;
        while (v >= p10[i]) { v -= p10[i]; d++; }
        if (d || started || i == 9) { writec((int)('0' + d)); started = 1; }
    }
    writec(' ');
}
