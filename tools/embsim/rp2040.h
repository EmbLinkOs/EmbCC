/* rp2040.h -- what the RP2040's models share (rp2040*.c): its clocks, its
 * microsecond tick, its pins, and the Raspberry Pi Pico around it.
 *
 * The part's registers are RP2040.svd's (the Pico SDK's), on the bus by
 * svd-map.c with the part's own bus rules (RF_ATOMIC_ALIASES and
 * RF_WIDE_WRITES); each model is found from the others by its
 * peripheral's name. Time is the core's cycles, as everywhere in EmbSim,
 * but the RP2040's clocks are modelled: the core runs at clk_sys, which
 * the program sets up (the ring oscillator, the crystal, the PLLs, the
 * clock generators), so a timer counts microseconds of clk_ref whatever
 * clk_sys is, and the board reports its events at their time. */
#ifndef EMBSIM_RP2040_H
#define EMBSIM_RP2040_H

#include "sim.h"

/* the clocks, as the clock generators number them (CLK_GPOUT0..) */
enum {
    RP_CLK_GPOUT0, RP_CLK_GPOUT1, RP_CLK_GPOUT2, RP_CLK_GPOUT3,
    RP_CLK_REF, RP_CLK_SYS, RP_CLK_PERI, RP_CLK_USB, RP_CLK_ADC, RP_CLK_RTC,
    RP_NCLK
};

/* the Pico's crystal, and the ring oscillator's nominal frequency at
 * boot (the datasheet's 2.17.1; its 2.15.2.1 says "typically 6MHz") */
#define RP_XOSC_HZ 12000000u
#define RP_ROSC_HZ 6500000u

/* a clock's frequency now, in Hz: 0 when it is stopped (rp2040.c) */
u32 rp2040_hz(struct sim *s, int clk);
/* the time since the part came out of reset, in nanoseconds, at the
 * clk_sys each stretch of cycles ran at */
double rp2040_now_ns(struct sim *s);
/* the cycles the devices have been told of since reset: the board's
 * time, at clk_sys (the core's own count adds its taken branches') */
u64 rp2040_cycles(struct sim *s);
/* clk_sys is about to change: the stretch at the old one is counted */
void rp2040_clock_change(struct sim *s);
/* the watchdog's tick generator: the clk_ref cycles a microsecond tick
 * takes (TICK.CYCLES), or 0 while it is stopped */
u32 rp2040_tick_cycles(struct sim *s);

/* ---- the pins (rp2040-io.c) ------------------------------------------ */

/* the SIO's outputs to the user bank (GPIO_OUT, GPIO_OE) and to the QSPI
 * bank (GPIO_HI_OUT, GPIO_HI_OE) changed: the pins follow */
void rp2040_sio_pins(struct sim *s, u32 out, u32 oe, u32 hi_out, u32 hi_oe);
/* what the SIO reads: GPIO_IN (the user bank's inputs, after INOVER) and
 * GPIO_HI_IN (the QSPI bank's) */
u32 rp2040_gpio_in(struct sim *s);
u32 rp2040_gpio_hi_in(struct sim *s);
/* the board drove a pin or let it go: the pins again */
void rp2040_pins_changed(struct sim *s);

/* ---- between the models --------------------------------------------- */

/* the watchdog's microsecond ticks came, n of them: the timer counts
 * (rp2040-timer.c); and the ticks until its next alarm that raises an
 * interrupt, 0 when none is armed */
void rp2040_timer_ticks(struct sim *s, u32 n);
int rp2040_timer_next(struct sim *s, u32 *ticks);
/* a clock started or stopped: the tick generator and the UARTs see it */
void rp2040_tick_clock(struct sim *s);
void rp2040_uart_kick(struct sim *s);
/* IO_QSPI's chip select override changed: a command to the flash may
 * have ended (rp2040-ssi.c) */
void rp2040_ssi_select(struct sim *s);

#endif
