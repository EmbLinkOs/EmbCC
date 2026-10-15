/* rp2040-timer.c -- the RP2040's system timer (its datasheet, 4.6), over
 * RP2040.svd's TIMER registers.
 *
 *   - a 64-bit count of the watchdog's microsecond ticks (rp2040.c makes
 *     them, from clk_ref), while the tick runs and PAUSE is clear and
 *     the timer is out of reset;
 *   - TIMERAWH and TIMERAWL read it as it is; a read of TIMELR latches
 *     the high half, which TIMEHR then reads; TIMELW then TIMEHW write it;
 *   - ALARM0-3: a write arms the alarm (its ARMED bit); when the count's
 *     low half passes the value written, INTR's bit sets and ARMED's
 *     clears. Writing ARMED's bit disarms it;
 *   - INTS is (INTR | INTF) & INTE, and each of its bits is TIMER_IRQ_n,
 *     made pending in the NVIC when it sets (and again when a write leaves
 *     it set). INTR's bits are cleared by writing 1.
 * DBGPAUSE has no effect: a debugger's halt stops all of EmbSim's time. */
#include <stdlib.h>

#include "devices.h"
#include "rp2040.h"
#include "svd-map.h"

struct timer {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *timehw, *timelw, *timehr, *timelr, *alarm[4], *armed;
    struct rf_reg *rawh, *rawl, *pause, *intr, *inte, *intf, *ints;
    int irq[4];
    u64 time;
    u32 lw;                         /* TIMELW, until TIMEHW */
    u32 latched;                    /* the high half TIMELR latched */
    int latch;
};

static struct timer *timer_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "TIMER") : 0;
    return p && p->ops == &rp2040_timer_ops ? p->ctx : 0;
}

static void show(struct timer *t)
{
    t->rawh->value = t->time >> 32;
    t->rawl->value = (u32)t->time;
}

/* INTS again: each bit that sets raises its TIMER_IRQ_n, and with
 * `again` (a write) each that stays set */
static void irqs(struct timer *t, u32 was, int again)
{
    u32 s = (u32)((t->intr->value | t->intf->value) & t->inte->value) & 15;
    if (s != t->ints->value)
        rf_hw(t->ints, s);
    for (int n = 0; n < 4; n++)
        if (s >> n & 1 && (!(was >> n & 1) || again))
            rf_interrupt(t->p, t->irq[n]);
}

/* `n` microsecond ticks: the count goes on, and the alarms it passes fire */
void rp2040_timer_ticks(struct sim *s, u32 n)
{
    struct timer *t = timer_of(s);
    if (!t || rf_gated(t->p) || (t->pause->value & 1))
        return;
    u32 lo = (u32)t->time, was = (u32)t->ints->value;
    t->time += n;
    show(t);
    u32 fired = 0;
    for (int k = 0; k < 4; k++)
        if (t->armed->value >> k & 1) {
            u32 d = (u32)t->alarm[k]->value - lo;    /* ticks to the match */
            if (d && d <= n)
                fired |= 1u << k;
        }
    if (fired) {
        rf_hw(t->armed, t->armed->value & ~(u64)fired);
        rf_hw(t->intr, t->intr->value | fired);
        irqs(t, was, 0);
    }
}

/* the ticks until the next alarm that raises an interrupt fires */
int rp2040_timer_next(struct sim *s, u32 *ticks)
{
    struct timer *t = timer_of(s);
    int any = 0;
    if (!t || rf_gated(t->p) || (t->pause->value & 1))
        return 0;
    for (int k = 0; k < 4; k++)
        if ((t->armed->value & t->inte->value) >> k & 1) {
            u32 d = (u32)t->alarm[k]->value - (u32)t->time;
            if (!d)
                continue;           /* passed: the count must wrap first */
            if (!any || d < *ticks)
                *ticks = d;
            any = 1;
        }
    return any;
}

static u32 timer_read(void *ctx, u32 off, int n)
{
    struct timer *t = ctx;
    show(t);
    if (off == rf_off(t->timelr) && !t->s->bus.debug) {
        t->timelr->value = (u32)t->time;
        t->latched = (u32)(t->time >> 32);
        t->latch = 1;
    }
    if (off == rf_off(t->timehr)) {
        t->timehr->value = t->latch ? t->latched : (u32)(t->time >> 32);
        if (!t->s->bus.debug)
            t->latch = 0;
    }
    return rf_read(t->p, off, n);
}

static void timer_write(void *ctx, u32 off, int n, u32 v)
{
    struct timer *t = ctx;
    u32 was = (u32)t->ints->value;
    rf_write(t->p, off, n, v);
    if (off == rf_off(t->timelw))
        t->lw = v;
    else if (off == rf_off(t->timehw)) {
        t->time = (u64)v << 32 | t->lw;
        show(t);
    }
    for (int k = 0; k < 4; k++)
        if (off == rf_off(t->alarm[k]))
            rf_hw(t->armed, t->armed->value | 1u << k);
    irqs(t, was, 1);
}

static void timer_reset(void *ctx)
{
    struct timer *t = ctx;
    t->time = 0;
    t->lw = 0;
    t->latch = 0;
    show(t);
}

const struct dev_ops rp2040_timer_ops = {
    "rp2040-timer", timer_read, timer_write, timer_reset, 0, 0,
};

void *rp2040_timer_create(struct sim *s, struct rf_periph *p)
{
    static const char *const an[4] = { "ALARM0", "ALARM1", "ALARM2", "ALARM3" };
    struct timer *t = calloc(1, sizeof *t);
    if (!t)
        die("out of memory");
    t->s = s;
    t->p = p;
    if (!(t->timehw = rf_reg(p, "TIMEHW")) || !(t->timelw = rf_reg(p, "TIMELW")) ||
        !(t->timehr = rf_reg(p, "TIMEHR")) || !(t->timelr = rf_reg(p, "TIMELR")) ||
        !(t->armed = rf_reg(p, "ARMED")) || !(t->rawh = rf_reg(p, "TIMERAWH")) ||
        !(t->rawl = rf_reg(p, "TIMERAWL")) || !(t->pause = rf_reg(p, "PAUSE")) ||
        !(t->intr = rf_reg(p, "INTR")) || !(t->inte = rf_reg(p, "INTE")) ||
        !(t->intf = rf_reg(p, "INTF")) || !(t->ints = rf_reg(p, "INTS"))) {
        free(t);
        return 0;
    }
    for (int k = 0; k < 4; k++) {
        char nm[16];
        if (!(t->alarm[k] = rf_reg(p, an[k]))) {
            free(t);
            return 0;
        }
        snprintf(nm, sizeof nm, "TIMER_IRQ_%d", k);
        t->irq[k] = rf_irq(p, nm);
    }
    return t;
}
