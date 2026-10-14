/* EmbCC's <avr/wdt.h>: the watchdog, avr-libc's interface.
 *
 * Changing the watchdog's configuration takes the datasheet's timed
 * sequence: with interrupts off, write WDCE and WDE to WDTCSR, then the
 * new value within four cycles. That is written as one asm statement, so
 * no optimisation level can put anything between the two stores.
 *
 *   wdt_enable(WDTO_500MS);   reset unless wdt_reset() comes within 0.5 s
 *   wdt_reset();
 *   wdt_disable();
 *
 * wdt_disable() does not clear WDRF in MCUSR; a watchdog reset leaves it
 * set and it holds WDE on, so code that disables the watchdog after one
 * clears it first (MCUSR = 0), as with avr-libc. */
#ifndef _AVR_WDT_H_
#define _AVR_WDT_H_

#include <avr/io.h>
#include <stdint.h>

/* The time-outs, as WDTO_*: the WDP3:0 value for each (the datasheet's
 * "Watchdog Timer Prescale Select", at the 128 kHz oscillator). */
#define WDTO_15MS  0
#define WDTO_30MS  1
#define WDTO_60MS  2
#define WDTO_120MS 3
#define WDTO_250MS 4
#define WDTO_500MS 5
#define WDTO_1S    6
#define WDTO_2S    7
#define WDTO_4S    8
#define WDTO_8S    9

#ifndef __ASSEMBLER__

#define wdt_reset() __asm__ __volatile__("wdr" ::: "memory")

/* WDE and the time-out's WDP bits: WDP3 is bit 5, apart from WDP2:0. */
#define __EMB_WDT_BITS(value) \
    ((uint8_t)(_BV(WDE) | (((value) & 0x08) ? _BV(WDP3) : 0) | ((value) & 0x07)))

static __inline__ __attribute__((__always_inline__)) void
__emb_wdt_write(uint8_t value)
{
    uint8_t sreg;
    __asm__ __volatile__(
        "in %0, __SREG__\n\t"
        "cli\n\t"
        "wdr\n\t"
        "sts %1, %2\n\t"
        "sts %1, %3\n\t"
        "out __SREG__, %0"
        : "=&r"(sreg)
        : "n"(_SFR_MEM_ADDR(WDTCSR)), "r"((uint8_t)(_BV(WDCE) | _BV(WDE))),
          "r"(value)
        : "memory");
}

#define wdt_enable(value) __emb_wdt_write(__EMB_WDT_BITS(value))
#define wdt_disable()     __emb_wdt_write(0)

#endif /* !__ASSEMBLER__ */

#endif /* _AVR_WDT_H_ */
