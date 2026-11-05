/* The Arduino Uno's runtime (embcc --board=uno links it after the
 * startup): the console on USART0, the pins by the board's numbers, and
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

/* the port, its bit: 0-7 PORTD, 8-13 PORTB, 14-19 PORTC */
static volatile unsigned char *pin_reg(int pin, int which, unsigned char *bit)
{
    static volatile unsigned char *const regs[3][3] = {
        { &PIND, &DDRD, &PORTD }, { &PINB, &DDRB, &PORTB }, { &PINC, &DDRC, &PORTC },
    };
    int port = pin < 8 ? 0 : pin < 14 ? 1 : 2;
    *bit = (unsigned char)(1u << (pin < 8 ? pin : pin < 14 ? pin - 8 : pin - 14));
    return regs[port][which];
}

void board_pin_mode(int pin, int mode)
{
    unsigned char b;
    if (pin < 0 || pin > 19)
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
    if (pin < 0 || pin > 19)
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
    if (pin < 0 || pin > 19)
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
