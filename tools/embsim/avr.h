/* avr.h -- the AVR core's state, and the interface its peripherals raise
 * interrupts through.
 *
 *   avr.c          the core: the AVR5 instruction set of the ATmega328P,
 *                  SREG, the data space, interrupts, sleep, step, reset,
 *                  and the CPU interface (registers for GDB too)
 *   avr-io.c       the I/O registers nothing else models: they keep what
 *                  is written
 *   avr-usart.c    USART0
 *   avr-timer16.c  Timer/Counter1
 *
 * Program memory (flash) is on the bus at 0, by byte address; the data
 * space at AVR_DATA + its address, as gdb's AVR target and QEMU number
 * them. The core answers for the registers and for SP and SREG itself;
 * the rest of the data space is the bus's. */
#ifndef EMBSIM_AVR_H
#define EMBSIM_AVR_H

#include "sim.h"

#define AVR_DATA 0x800000u          /* the data space on the bus */
#define AVR_VECTORS 26

/* SREG's bits */
enum { SREG_C = 1, SREG_Z = 2, SREG_N = 4, SREG_V = 8, SREG_S = 16,
       SREG_H = 32, SREG_T = 64, SREG_I = 128 };

/* An interrupt source: a peripheral's flag and enable. `pending` says
 * whether vector `vec` is requested now; `ack` is the hardware clearing
 * the flag as the vector is entered (0 when the program must clear it). */
struct avr_irq {
    int vec;
    int (*pending)(void *ctx, int vec);
    void (*ack)(void *ctx, int vec);
    void *ctx;
};

#define AVR_IRQS 16

struct avr_state {
    struct cpu cpu;                 /* the interface; first */
    struct sim *sim;
    u32 flash_words;                /* the program counter's range */
    u8 r[32];
    u8 sreg;
    u16 sp;
    u32 pc;                         /* in words */
    u32 npc;
    int inhibit;                    /* SEI's and RETI's next instruction runs */
    int sleeping;                   /* in SLEEP: waking costs 4 cycles more */
    int changed;                    /* the instruction changed state */
    struct avr_irq src[AVR_IRQS];
    int nsrc;
    u32 sw_pend;                    /* vectors pended through the CPU interface */
    int pend;                       /* the lowest vector requested, or 0 */
};

/* a peripheral's interrupt source, on the board's core */
void avr_irq_source(struct sim *s, int vec,
                    int (*pending)(void *ctx, int vec),
                    void (*ack)(void *ctx, int vec), void *ctx);
/* a source's flag or enable changed: the core looks again */
void avr_irq_changed(struct sim *s);

#endif
