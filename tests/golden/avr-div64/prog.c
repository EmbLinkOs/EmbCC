/* tests/golden/avr-div64.sh: 64-bit division and remainder on QEMU's
 * ATmega328P, signed and unsigned, and how long one takes. */
#include <avr/io.h>
#include <stdio.h>
#include <stdint.h>
static void putch(char c) { loop_until_bit_is_set(UCSR0A, UDRE0); UDR0 = (uint8_t)c; }
static void ps(const char *s) { while (*s) putch(*s++); }
static const uint64_t ns[] = {
#define P(n, d) n,
#include "pairs.h"
#undef P
};
static const uint64_t ds[] = {
#define P(n, d) d,
#include "pairs.h"
#undef P
};
volatile uint64_t vn, vd;
int main(void)
{
    char b[80];
    unsigned i;
    uint16_t t0, t1;
    UBRR0 = 8; UCSR0B = _BV(TXEN0); UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
    for (i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        uint64_t n = ns[i], d = ds[i];
        int64_t sn = (int64_t)n, sd = (int64_t)d;
        snprintf(b, sizeof b, "%llx %llx %llx %llx\n", n / d, n % d,
                 sd == -1 && sn == INT64_MIN ? 0ULL : (unsigned long long)(sn / sd),
                 sd == -1 && sn == INT64_MIN ? 0ULL : (unsigned long long)(sn % sd));
        ps(b);
    }
    vn = 0xffffffffffffffffULL;
    vd = 1000000ULL;
    TCCR1B = _BV(CS10);
    t0 = TCNT1;
    vn = vn / vd;
    t1 = TCNT1;
    snprintf(b, sizeof b, "cycles %s\n", (uint16_t)(t1 - t0) < 4000u ? "fast" : "slow");
    ps(b);
    ps("==END==\n");
    for (;;) {
    }
}
