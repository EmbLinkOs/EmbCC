/* stm32-tim.c -- an STM32 timer's time base (RM0090 18.3.1-2, 20.3.1-2):
 * what every timer from the basic TIM6 to the advanced TIM1 has, over the
 * SVD's registers.
 *
 *   - CNT counts up while CR1.CEN is set, one count per PSC+1 clocks of
 *     the timer's clock; after ARR it goes back to 0 and that is an
 *     update event: SR.UIF sets and, with DIER.UIE, the timer's interrupt
 *     is made pending (the SVD's: the one named ..._UP_... if it has
 *     several, as TIM1 has). ARR of 0 holds the counter;
 *   - PSC is buffered: it takes effect at the next update event, as on
 *     the part; ARR too with CR1.ARPE, else at once;
 *   - EGR.UG reinitializes the counter and the prescaler and makes an
 *     update event, which sets UIF unless CR1.URS is set; CR1.UDIS stops
 *     update events (UG then only reinitializes); CR1.OPM clears CEN at
 *     the update event;
 *   - SR's flags are rc_w0: written 0, cleared; 1 leaves them;
 *   - the timer's clock is its APB's, from RCC's CFGR: the core's clock
 *     when the APB prescaler (PPRE1 or PPRE2) is 1, else twice the APB's
 *     clock (RM0090 6.2: the core's clock divided by the prescaler over
 *     2). Without RCC's registers, the core's clock;
 *   - it does not count while its clock is gated off (RCC's enable bit).
 * The counter width is CNT's fields' (TIM2 and TIM5 are 32-bit).
 *
 * Not modelled: counting down and center-aligned (DIR and CMS), the
 * repetition counter, capture/compare and its flags, slave modes,
 * triggers, DMA requests. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

struct tim {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *cr1, *dier, *sr, *egr, *cnt, *psc, *arr;
    u64 cen, udis, urs, opm, arpe, uie, uif, ug;
    u64 cntmask;                    /* the counter's bits */
    u32 psc_now, arr_now;           /* the shadow registers in use */
    u32 pc;                         /* the prescaler's counter */
    struct rf_reg *cfgr;            /* RCC's, for the APB prescaler */
    u64 ppre;
    int ppre_lsb;
    u32 frac;                       /* core cycles toward the next clock */
    int irq, running;
};

static int gated(const struct tim *t)
{
    return t->p->gate && !(t->p->gate->value & t->p->gate_mask);
}

/* core cycles per timer clock: 1, or with an APB prescaler of 4, 8 or 16,
 * 2, 4 or 8 */
static u32 divisor(const struct tim *t)
{
    if (!t->ppre)
        return 1;
    u32 v = (u32)((t->cfgr->value & t->ppre) >> t->ppre_lsb);
    return v < 5 ? 1 : 1u << (v - 4);
}

static void irq_check(struct tim *t)
{
    if ((t->sr->value & t->uif) && (t->dier->value & t->uie))
        rf_interrupt(t->p, t->irq);
}

static void run_clock(struct tim *t)
{
    rf_clock(t->p, &t->running, (t->cr1->value & t->cen) != 0);
}

/* an update event; `ug`: made by EGR.UG */
static void update(struct tim *t, int ug)
{
    if (t->cr1->value & t->udis)
        return;
    t->psc_now = (u32)t->psc->value & 0xffff;
    t->arr_now = (u32)(t->arr->value & t->cntmask);
    if (!(ug && (t->cr1->value & t->urs))) {
        rf_hw(t->sr, t->sr->value | t->uif);
        irq_check(t);
    }
    if (!ug && (t->cr1->value & t->opm)) {
        rf_hw(t->cr1, t->cr1->value & ~t->cen);
        run_clock(t);
    }
}

/* `n` counts of the counter */
static void count(struct tim *t, u64 n)
{
    u64 cnt = t->cnt->value & t->cntmask;
    if (cnt + n <= t->arr_now) {    /* no update on the way */
        t->cnt->value += n;
        return;
    }
    while (n && (t->cr1->value & t->cen)) {
        u64 to;                     /* counts to the next wrap */
        if (cnt > t->arr_now)
            to = t->cntmask - cnt + 1;          /* past ARR: to 0, no event */
        else
            to = t->arr_now - cnt + 1;
        if (n < to) {
            cnt += n;
            break;
        }
        n -= to;
        int event = cnt <= t->arr_now;
        cnt = 0;
        t->cnt->value = (t->cnt->value & ~t->cntmask) | cnt;
        if (event)
            update(t, 0);
    }
    t->cnt->value = (t->cnt->value & ~t->cntmask) | cnt;
}

/* called after each instruction while CEN is set: kept short */
static void tim_tick(void *ctx, u32 cycles)
{
    struct tim *t = ctx;
    if (!t->arr_now || gated(t))
        return;
    u64 clocks = cycles;
    u32 d = divisor(t);
    if (d > 1) {
        u32 all = t->frac + cycles;
        clocks = all / d;
        t->frac = all % d;
    }
    u64 all = t->pc + clocks;
    if (all <= t->psc_now) {        /* the prescaler has not wrapped */
        t->pc = (u32)all;
        return;
    }
    if (!t->psc_now) {
        t->pc = 0;
        count(t, all);
        return;
    }
    u64 per = (u64)t->psc_now + 1;
    t->pc = (u32)(all % per);
    count(t, all / per);
}

static int tim_next_event(void *ctx, u32 *cycles)
{
    struct tim *t = ctx;
    if (!(t->cr1->value & t->cen) || !(t->dier->value & t->uie) ||
        (t->cr1->value & t->udis) || !t->arr_now || gated(t))
        return 0;
    u64 cnt = t->cnt->value & t->cntmask, counts;
    if (cnt > t->arr_now)
        counts = (t->cntmask - cnt + 1) + (u64)t->arr_now + 1;
    else
        counts = t->arr_now - cnt + 1;
    u64 per = (u64)t->psc_now + 1;
    u64 clocks = (per - t->pc) + (counts - 1) * per;
    u64 c = clocks * divisor(t) - t->frac;
    *cycles = c > 0xffffffffULL ? 0xffffffffu : (u32)c;
    return 1;
}

static u32 tim_read(void *ctx, u32 off, int n)
{
    struct tim *t = ctx;
    return rf_read(t->p, off, n);
}

static void tim_write(void *ctx, u32 off, int n, u32 v)
{
    struct tim *t = ctx;
    rf_write(t->p, off, n, v);
    if (off == rf_off(t->arr) && !(t->cr1->value & t->arpe))
        t->arr_now = (u32)(t->arr->value & t->cntmask);
    if (off == rf_off(t->egr) && (v & t->ug)) {
        /* UG: the counter and the prescaler start again */
        t->cnt->value &= ~t->cntmask;
        t->pc = 0;
        update(t, 1);
    }
    run_clock(t);
    irq_check(t);
}

static void tim_reset(void *ctx)
{
    struct tim *t = ctx;
    t->psc_now = (u32)t->psc->value & 0xffff;
    t->arr_now = (u32)(t->arr->value & t->cntmask);
    t->pc = t->frac = 0;
    t->running = 0;
}

const struct dev_ops stm32_tim_ops = {
    "stm32-tim", tim_read, tim_write, tim_reset, tim_tick, tim_next_event,
};

void *stm32_tim_create(struct sim *s, struct rf_periph *p)
{
    struct tim t;
    memset(&t, 0, sizeof t);
    t.s = s;
    t.p = p;
    t.cr1 = rf_reg(p, "CR1");
    t.dier = rf_reg(p, "DIER");
    t.sr = rf_reg(p, "SR");
    t.egr = rf_reg(p, "EGR");
    t.cnt = rf_reg(p, "CNT");
    t.psc = rf_reg(p, "PSC");
    t.arr = rf_reg(p, "ARR");
    if (!t.cr1 || !t.dier || !t.sr || !t.egr || !t.cnt || !t.psc || !t.arr)
        return 0;
    t.cen = rf_mask(t.cr1, "CEN");
    t.udis = rf_mask(t.cr1, "UDIS");
    t.urs = rf_mask(t.cr1, "URS");
    t.opm = rf_mask(t.cr1, "OPM");
    t.arpe = rf_mask(t.cr1, "ARPE");
    t.uie = rf_mask(t.dier, "UIE");
    t.uif = rf_mask(t.sr, "UIF");
    t.ug = rf_mask(t.egr, "UG");
    if (!t.cen || !t.uif || !t.ug)
        return 0;
    for (int i = 0; i < t.cnt->nf; i++) {
        int top = t.cnt->f[i].lsb + t.cnt->f[i].width;
        u64 m = top >= 64 ? ~0ULL : (1ULL << top) - 1;
        if (m > t.cntmask)
            t.cntmask = m;
    }
    if (t.cntmask > 0xffffffffULL || !t.cntmask)
        t.cntmask = 0xffff;
    rf_set_mwv(t.sr, ~0ULL, MWV_0CLR);
    t.irq = rf_irq(p, "_UP");
    /* the APB it is on: the one whose enable register has its bit */
    struct rf_periph *rcc = rf_periph(s->svd, "RCC");
    t.cfgr = rf_reg(rcc, "CFGR");
    if (t.cfgr && p->gate_name)
        t.ppre = rf_mask(t.cfgr, strstr(p->gate_name, "APB2") ? "PPRE2" : "PPRE1");
    while (t.ppre && !(t.ppre >> t.ppre_lsb & 1))
        t.ppre_lsb++;
    struct tim *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = t;
    tim_reset(c);
    return c;
}
