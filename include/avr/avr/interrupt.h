/* EmbCC's <avr/interrupt.h>: interrupt handlers and the global interrupt
 * flag, with avr-libc's interface.
 *
 * ISR(vector) defines vector's handler -- `ISR(TIMER1_COMPA_vect) { ... }`
 * -- as the function the startup's vector table jumps to (__vector_11 for
 * that one; lib/avr/crt.S), with the `signal` attribute: EmbCC saves SREG
 * and every register the body uses, clears r1, and returns with reti. A
 * vector with no handler goes to __bad_interrupt, as with avr-libc.
 *
 * The attributes ISR() takes after the vector:
 *   ISR_BLOCK    interrupts stay disabled in the body (the default);
 *   ISR_NOBLOCK  the body begins by enabling them (`interrupt`), so a
 *                higher-priority vector can preempt it;
 *   ISR_NAKED    no prologue and no epilogue: the body is inline asm and
 *                ends with reti() itself;
 *   ISR_ALIASOF(v) this vector runs v's handler (ISR_ALIAS defines it).
 * The vector names (TIMER1_COMPA_vect, ...) come from <avr/io.h>. */
#ifndef _AVR_INTERRUPT_H_
#define _AVR_INTERRUPT_H_

#include <avr/io.h>

#ifndef __ASSEMBLER__

/* The global interrupt flag. Each is a compiler barrier too: memory
 * accesses are not moved across them. */
#define sei()  __asm__ __volatile__("sei" ::: "memory")
#define cli()  __asm__ __volatile__("cli" ::: "memory")
/* The return from a naked handler. */
#define reti() __asm__ __volatile__("reti" ::: "memory")

#define __EMB_ISR_STR2(x) #x
#define __EMB_ISR_STR(x)  __EMB_ISR_STR2(x)

#define ISR_BLOCK
#define ISR_NOBLOCK    __attribute__((__interrupt__))
#define ISR_NAKED      __attribute__((__naked__))
#define ISR_ALIASOF(v) __attribute__((__alias__(__EMB_ISR_STR(v))))

#define ISR(vector, ...)                                                    \
    void vector(void) __attribute__((__signal__, __used__)) __VA_ARGS__;    \
    void vector(void)

/* avr-libc's older spelling of ISR(vector). */
#define SIGNAL(vector) ISR(vector)

/* A handler that only returns: the vector is acknowledged and nothing else
 * happens. */
#define EMPTY_INTERRUPT(vector)                                             \
    void vector(void) __attribute__((__signal__, __naked__, __used__));     \
    void vector(void) { __asm__ __volatile__("reti" ::); }

/* `vector` runs `target_vector`'s handler: the same code, under the second
 * name. The target must be defined in the same file. */
#define ISR_ALIAS(vector, target_vector)                                    \
    void vector(void) __attribute__((__signal__, __used__))                 \
        ISR_ALIASOF(target_vector)

/* What runs for a vector that has no handler, in place of the reset that
 * __bad_interrupt otherwise jumps to: ISR(BADISR_vect) { ... }. */
#define BADISR_vect __vector_default

#endif /* !__ASSEMBLER__ */

#endif /* _AVR_INTERRUPT_H_ */
