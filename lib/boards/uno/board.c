/* The Arduino Uno's runtime (embcc --board=uno links it after the
 * startup), and the Mega 2560's (--board=mega2560: lib/boards/mega2560
 * builds this file for its part): the console on USART0, the pins by the
 * board's numbers, and
 * Timer1 as a 1 kHz clock -- CTC at 16 MHz / 64 / 250, exactly. Timer1
 * and not Arduino's Timer0: QEMU's ATmega328P has the 16-bit timers only,
 * and the board is tested there too. */
#include <board.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdio.h>
#include <util/delay_basic.h>

static volatile unsigned long g_ms;
static volatile unsigned long long g_us;    /* g_ms * 1000, kept: a 64-bit multiply is
                                             * a long call on AVR */

ISR(TIMER1_COMPA_vect)
{
    g_ms++;
    g_us += 1000;
}

void board_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = (unsigned char)c;
}

int board_getc(void)
{
    if (!(UCSR0A & (1 << RXC0)))
        return -1;
    return UDR0;
}

/* stdout (lib/avr's printf sends each character here; its own putchar is
 * weak): "\n" as "\r\n" */
int putchar(int c)
{
    if (c == '\n')
        board_putc('\r');
    board_putc((char)c);
    return (unsigned char)c;
}

/* stdin: waits for a byte */
int getchar(void)
{
    int c;
    while ((c = board_getc()) < 0)
        ;
    return c;
}

/* Each pin's port and bit, (port << 3) | bit, the ports by the data
 * address of their PIN register (DDR follows it, then PORT, on both
 * parts). The Uno: 0-7 PORTD, 8-13 PORTB, 14-19 PORTC (A0-A5). The Mega
 * 2560: the Arduino Mega's numbering (its pins_arduino.h), 0-53 the
 * digital header, 54-69 A0-A15. */
#if defined(__AVR_ATmega2560__)
enum { PA_, PB_, PC_, PD_, PE_, PF_, PG_, PH_, PJ_, PK_, PL_ };
static const unsigned short port_pin[] = { 0x20, 0x23, 0x26, 0x29, 0x2C, 0x2F, 0x32,
                                           0x100, 0x103, 0x106, 0x109 };
#define P(port, bit) ((port) << 3 | (bit))
static const unsigned char pin_map[] = {
    P(PE_, 0), P(PE_, 1), P(PE_, 4), P(PE_, 5), P(PG_, 5), P(PE_, 3), P(PH_, 3), P(PH_, 4),
    P(PH_, 5), P(PH_, 6), P(PB_, 4), P(PB_, 5), P(PB_, 6), P(PB_, 7), P(PJ_, 1), P(PJ_, 0),
    P(PH_, 1), P(PH_, 0), P(PD_, 3), P(PD_, 2), P(PD_, 1), P(PD_, 0), P(PA_, 0), P(PA_, 1),
    P(PA_, 2), P(PA_, 3), P(PA_, 4), P(PA_, 5), P(PA_, 6), P(PA_, 7), P(PC_, 7), P(PC_, 6),
    P(PC_, 5), P(PC_, 4), P(PC_, 3), P(PC_, 2), P(PC_, 1), P(PC_, 0), P(PD_, 7), P(PG_, 2),
    P(PG_, 1), P(PG_, 0), P(PL_, 7), P(PL_, 6), P(PL_, 5), P(PL_, 4), P(PL_, 3), P(PL_, 2),
    P(PL_, 1), P(PL_, 0), P(PB_, 3), P(PB_, 2), P(PB_, 1), P(PB_, 0),
    P(PF_, 0), P(PF_, 1), P(PF_, 2), P(PF_, 3), P(PF_, 4), P(PF_, 5), P(PF_, 6), P(PF_, 7),
    P(PK_, 0), P(PK_, 1), P(PK_, 2), P(PK_, 3), P(PK_, 4), P(PK_, 5), P(PK_, 6), P(PK_, 7),
};
#else
enum { PB_, PC_, PD_ };
static const unsigned short port_pin[] = { 0x23, 0x26, 0x29 };
#define P(port, bit) ((port) << 3 | (bit))
static const unsigned char pin_map[] = {
    P(PD_, 0), P(PD_, 1), P(PD_, 2), P(PD_, 3), P(PD_, 4), P(PD_, 5), P(PD_, 6), P(PD_, 7),
    P(PB_, 0), P(PB_, 1), P(PB_, 2), P(PB_, 3), P(PB_, 4), P(PB_, 5),
    P(PC_, 0), P(PC_, 1), P(PC_, 2), P(PC_, 3), P(PC_, 4), P(PC_, 5),
};
#endif
#define NPINS ((int)(sizeof pin_map / sizeof pin_map[0]))

/* the pin's PIN (which 0), DDR (1) or PORT (2) register, and its bit */
static volatile unsigned char *pin_reg(int pin, int which, unsigned char *bit)
{
    unsigned char m = pin_map[pin];
    *bit = (unsigned char)(1u << (m & 7));
    return (volatile unsigned char *)(unsigned)(port_pin[m >> 3] + (unsigned)which);
}

void board_pin_mode(int pin, int mode)
{
    unsigned char b;
    if (pin < 0 || pin >= NPINS)
        return;
    volatile unsigned char *ddr = pin_reg(pin, 1, &b), *port = pin_reg(pin, 2, &b);
    unsigned char s = SREG;
    cli();
    if (mode == BOARD_OUTPUT) {
        *ddr |= b;
    } else {
        *ddr &= (unsigned char)~b;
        if (mode == BOARD_INPUT_PULLUP)
            *port |= b;
        else
            *port &= (unsigned char)~b;
    }
    SREG = s;
}

void board_pin_write(int pin, int high)
{
    unsigned char b;
    if (pin < 0 || pin >= NPINS)
        return;
    volatile unsigned char *port = pin_reg(pin, 2, &b);
    unsigned char s = SREG;
    cli();
    if (high)
        *port |= b;
    else
        *port &= (unsigned char)~b;
    SREG = s;
}

int board_pin_read(int pin)
{
    unsigned char b;
    if (pin < 0 || pin >= NPINS)
        return 0;
    return (*pin_reg(pin, 0, &b) & b) != 0;
}

void board_led(int on)
{
    board_pin_write(BOARD_LED, on);
}

unsigned long board_millis(void)
{
    unsigned char s = SREG;
    cli();
    unsigned long ms = g_ms;
    SREG = s;
    return ms;
}

/* the millisecond, and Timer1's count into it (4 us a count); a compare
 * match pending since the read is the next millisecond */
unsigned long long board_micros(void)
{
    unsigned char s = SREG;
    cli();
    unsigned long long us = g_us;
    unsigned t = TCNT1;
    if ((TIFR1 & (1 << OCF1A)) && t < 249)
        us += 1000;
    SREG = s;
    return us + (unsigned)(t * 4u);
}

/* _delay_loop_2 is four cycles a turn: a quarter microsecond at 16 MHz */
void board_delay_us(unsigned long us)
{
    while (us >= 16000) {
        _delay_loop_2(64000);
        us -= 16000;
    }
    if (us)
        _delay_loop_2((unsigned short)(us * 4));
}

void board_delay_ms(unsigned long ms)
{
    unsigned long t0 = board_millis();
    while (board_millis() - t0 < ms)
        ;
}

/* Before main (the startup runs this object's constructor first): the
 * console at 115200 (U2X, UBRR 16, as Arduino's core sets it: 117647 baud,
 * which the board's USB bridge reads), Timer1's clock, interrupts on, and
 * the LED's pin an output. */
__attribute__((constructor)) static void board_init(void)
{
    UCSR0A = 1 << U2X0;
    UBRR0H = 0;
    UBRR0L = 16;
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    UCSR0B = (1 << TXEN0) | (1 << RXEN0);
    TCCR1A = 0;
    TCNT1 = 0;
    OCR1A = 249;
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);   /* CTC, / 64 */
    TIMSK1 = 1 << OCIE1A;
    sei();
    board_pin_mode(BOARD_LED, BOARD_OUTPUT);
}
