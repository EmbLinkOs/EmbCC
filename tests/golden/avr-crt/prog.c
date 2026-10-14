/* tests/golden/avr-crt.sh: a program written against avr-libc's interface
 * alone -- USART0 output, a Timer1 compare interrupt, an unhandled vector
 * caught by BADISR_vect -- and the startup's work: .data copied, .bss
 * zeroed, a constructor run, a user's .init3 step (early.S) run before
 * main. */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

static volatile uint8_t ticks, bad;
static uint8_t zeroed[8];                       /* .bss */
static int inited = 1234;                       /* .data */
static const char greeting[] = "copied";        /* .rodata, in RAM */
static volatile uint8_t ctor_ran;
uint8_t early_flag __attribute__((section(".noinit")));

__attribute__((constructor)) void emb_ctor(void) { ctor_ran = 7; }

/* early_flag is set by early.S, in .init3: before .data is copied and
 * .bss cleared, which .noinit is spared. */

ISR(TIMER1_COMPA_vect) { ticks++; }

/* Timer1's overflow (vector 13) has no handler: __bad_interrupt sends it
 * here, and this stops it. */
ISR(BADISR_vect)
{
    bad++;
    TIMSK1 &= (uint8_t)~_BV(TOIE1);
}

static void putch(char c)
{
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = (uint8_t)c;
}

static void puts_ram(const char *s) { while (*s) putch(*s++); }

static void puts_P(PGM_P s)
{
    char c;
    while ((c = (char)pgm_read_byte(s++)) != 0)
        putch(c);
}

static void put_u(unsigned v)
{
    char b[6];
    int i = 0;
    do { b[i++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (i) putch(b[--i]);
}

int main(void)
{
    unsigned sum = 0, spin;
    uint8_t i;
    UBRR0 = 8;                                   /* 115200 at 16 MHz */
    UCSR0B = _BV(TXEN0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
    for (i = 0; i < sizeof zeroed; i++)
        sum += zeroed[i];
    puts_P(PSTR("data "));
    put_u((unsigned)inited);
    putch(' ');
    puts_ram(greeting);
    puts_P(PSTR(" bss "));
    put_u(sum);
    puts_P(PSTR(" ctor "));
    put_u(ctor_ran);
    puts_P(PSTR(" init3 "));
    put_u(early_flag);
    putch('\n');

    OCR1A = 2499;                                /* 10 ms at /64 */
    TCCR1B = _BV(WGM12) | _BV(CS11) | _BV(CS10);
    TIMSK1 = _BV(OCIE1A);
    sei();
    for (spin = 0; ticks < 3 && spin < 60000u; spin++)
        _delay_us(10);
    cli();
    TIMSK1 = 0;
    TCCR1B = _BV(CS10);                          /* normal mode, /1 */
    TCNT1 = 0;
    TIFR1 = _BV(TOV1);
    TIMSK1 = _BV(TOIE1);
    sei();
    for (spin = 0; !bad && spin < 60000u; spin++)
        _delay_us(10);
    puts_P(PSTR("ticks "));
    put_u(ticks);
    puts_P(PSTR(" bad "));
    put_u(bad);
    puts_P(PSTR("\n==END==\n"));
    for (;;) {
    }
}
