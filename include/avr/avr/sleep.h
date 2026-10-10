/* EmbCC's <avr/sleep.h>: the sleep modes, avr-libc's interface.
 *
 *   set_sleep_mode(SLEEP_MODE_PWR_DOWN);
 *   cli();
 *   if (nothing_to_do) {
 *       sleep_enable();
 *       sei();             the instruction after sei runs before any
 *       sleep_cpu();       interrupt, so this sleep cannot miss a wakeup
 *       sleep_disable();
 *   }
 *   sei();
 *
 * sleep_mode() is enable, sleep, disable in one. */
#ifndef _AVR_SLEEP_H_
#define _AVR_SLEEP_H_

#include <avr/io.h>
#include <stdint.h>

/* SM2:0 in SMCR, as the datasheet's "Sleep Mode Select" table has them. */
#define SLEEP_MODE_IDLE        0
#define SLEEP_MODE_ADC         _BV(SM0)
#define SLEEP_MODE_PWR_DOWN    _BV(SM1)
#define SLEEP_MODE_PWR_SAVE    (_BV(SM1) | _BV(SM0))
#define SLEEP_MODE_STANDBY     (_BV(SM2) | _BV(SM1))
#define SLEEP_MODE_EXT_STANDBY (_BV(SM2) | _BV(SM1) | _BV(SM0))

#ifndef __ASSEMBLER__

#define set_sleep_mode(mode)                                                 \
    do {                                                                     \
        SMCR = (uint8_t)((SMCR & ~(_BV(SM2) | _BV(SM1) | _BV(SM0))) |        \
                         (mode));                                            \
    } while (0)

#define sleep_enable()  do { SMCR |= (uint8_t)_BV(SE); } while (0)
#define sleep_disable() do { SMCR &= (uint8_t)~_BV(SE); } while (0)
#define sleep_cpu()     __asm__ __volatile__("sleep" ::: "memory")

#define sleep_mode()                                                         \
    do {                                                                     \
        sleep_enable();                                                      \
        sleep_cpu();                                                         \
        sleep_disable();                                                     \
    } while (0)

#if defined(BODS) && defined(BODSE)
/* The picoPower parts turn the brown-out detector off for the next sleep:
 * BODS and BODSE together, then BODS alone within four cycles, then sleep
 * within three more -- so this goes right before sleep_cpu(). */
#define sleep_bod_disable()                                                  \
    do {                                                                     \
        uint8_t __emb_t1, __emb_t2;                                          \
        __asm__ __volatile__(                                                \
            "in %0, %2\n\t"                                                  \
            "ori %0, %3\n\t"                                                 \
            "mov %1, %0\n\t"                                                 \
            "andi %1, %4\n\t"                                                \
            "out %2, %0\n\t"                                                 \
            "out %2, %1"                                                     \
            : "=&d"(__emb_t1), "=&d"(__emb_t2)                               \
            : "I"(_SFR_IO_ADDR(MCUCR)),                                      \
              "i"(_BV(BODS) | _BV(BODSE)),                                   \
              "i"((uint8_t)~_BV(BODSE))                                      \
            : "memory");                                                     \
    } while (0)
#endif

#endif /* !__ASSEMBLER__ */

#endif /* _AVR_SLEEP_H_ */
