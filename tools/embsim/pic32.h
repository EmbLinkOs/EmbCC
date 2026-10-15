/* pic32.h -- what a PIC32's peripheral models share: raising an
 * interrupt through the EVIC (pic32-evic.c), and the peripheral bus
 * clocks (pic32-cru.c).
 *
 *   pic32-evic.c   the EVIC: IFS, IEC, IPC, OFFn, INTCON, PRISS,
 *                  INTSTAT, and the core's EIC mode
 *   pic32-cru.c    the oscillator's registers: OSCCON, the PBxDIV
 *                  dividers, SYSKEY's unlock
 *   pic32-tmr.c    Timer1 (type A) and Timer2-9 (type B, and 32-bit pairs)
 *   pic32-gpio.c   ports A-K: TRIS, LAT, PORT, ANSEL, the pulls
 *   pic32-uart.c   UART1-6
 *   pic32-pmp.c    the parallel master port
 *   pic32-adc.c    ADCHS: the conversions a driver starts and reads
 *
 * Each sits on the part's registers as the device pack's ATDF has them
 * (atdf.c, svd-map.c), found by the file's names. A model's interrupt is
 * its IFS flag set by the hardware, pic32_irq; the EVIC decides from
 * there. */
#ifndef EMBSIM_PIC32_H
#define EMBSIM_PIC32_H

#include "sim.h"
#include "svd-map.h"

/* the hardware sets interrupt irq's flag (IFSx), as the ATDF numbers it */
void pic32_irq(struct sim *s, int irq);
/* the flag is set, or its interrupt enabled (IECx) */
int pic32_irq_flag(struct sim *s, int irq);
int pic32_irq_enabled(struct sim *s, int irq);
/* an interrupt's number by the ATDF's name (TIMER_2), or -1 */
int pic32_irq_named(struct sim *s, const char *name);
/* A persistent source: its flag cannot be cleared while `cond` says the
 * condition holds (a UART with room to transmit), as the part's are. */
void pic32_irq_level(struct sim *s, int irq, int (*cond)(void *ctx), void *ctx);
/* SYSKEY's unlock sequence has been written, and not locked since */
int pic32_unlocked(struct sim *s);

/* core cycles per clock of peripheral bus n (1 to 8): PBnDIV's divider
 * (the core runs on SYSCLK, as PB7 does); pic32_pb_reg finds the
 * register once, for a model that divides on every tick */
u32 pic32_pb_div(struct sim *s, int bus);
struct rf_reg *pic32_pb_reg(struct sim *s, int bus);

/* the core cycles per second the board runs at, for its reports */
#define PIC32_SYSCLK 200000000u

/* a PBnDIV register's divider (1 without one) */
static inline u32 pic32_pb_of(const struct rf_reg *r)
{
    return r ? (u32)(r->value & 0x7f) + 1 : 1;
}

#endif
