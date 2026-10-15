/* pic24-oc.c -- a PIC24FJ's output compare modules, OC1 to OC6, with
 * their dedicated timers (the EDC's OC: OCxCON1, OCxCON2, OCxRS, OCxR,
 * OCxTMR), as the data sheet's section 15 and the family reference
 * manual's "Output Compare with Dedicated Timer" (DS70005159) have them.
 *
 *   - OCxTMR counts, while the module is on, the clock OCTSEL selects:
 *     the instruction clock (111), or Timer1-5's prescaled clock while
 *     that timer runs (100 Timer1, 011 Timer5, 010 Timer4, 001 Timer3,
 *     000 Timer2). It goes back to 0 on the count after SYNCSEL's source
 *     comes: the module's own match with OCxRS (11111), Timer1-3's period
 *     match, the ICs', the CCPs' and the ADC's interrupts, the other OCs'
 *     and the CCPs' sync outputs; with SYNCSEL 00000 it wraps at 0xFFFF.
 *     With OCTRIG it is held at 0 until the source (or the program) sets
 *     TRIGSTAT, which TRIGMODE clears at the match with OCxRS. A match
 *     with OCxRS is the module's sync output, which the others may select;
 *   - OCM: the output set (001) or cleared (010) at the match with OCxR,
 *     the ICx interrupt then; toggled at each match (011); a pulse, set
 *     at OCxR and cleared at OCxRS, once (100) or each period (101), the
 *     interrupt at its end; edge-aligned PWM (110): set at the period's
 *     start (unless OCxR is 0), cleared at OCxR, the interrupt at each
 *     period; center-aligned PWM (111): as 101. In the PWM modes OCxR and
 *     OCxRS take effect at the period's start (double-buffered);
 *   - the output, OCINV inverting it, drives the pin the PPS selects it on
 *     (OC1-3: output functions 13-15) while the module is on and OCTRIS
 *     is clear.
 * The output changes at the count of the match (the part's is a cycle
 * later). Not modelled: the faults (ENFLT, OCFLT), the cascaded 32-bit
 * mode (OC32), DCB's fraction of a cycle, the comparators', the CTMU's
 * and the OCTRIGx pins' triggers. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define NU 6

/* OCxCON1 */
#define TRIGMODE (1u << 3)
/* OCxCON2 */
#define OCINV   (1u << 12)
#define OCTRIG  (1u << 7)
#define TRIGSTAT (1u << 6)
#define OCTRIS  (1u << 5)

/* SYNCSEL's sources, by the data sheet's Register 15-2 (11111 is the
 * module's own) */
static const char *const sources[32] = {
    0, "OC1sync", "OC3sync", "OC5sync", "CCP1sync", "CCP2sync", "CCP3sync",
    "CCP4sync", "CCP5sync", "CCP6sync", "CCP7sync", "T1", "T2", "T3", "CCP6",
    "CCP7", "IC1", "IC2", "IC3", "CCP1", "CCP2", "CCP3", "CCP4", "CCP5", 0, 0,
    0, "ADC1", 0, 0, 0, 0,
};

struct unit {
    struct rf_reg *con1, *con2, *rs, *r, *tmr;
    int k, irq, fn;                 /* fn: its PPS output function, or 0 */
    u32 acc;                        /* cycles toward the next count */
    u32 rb, rsb;                    /* OCxR and OCxRS as the period has them */
    int out;                        /* the output, before OCINV */
    int done;                       /* 100's pulse */
    int sync;                       /* SYNCSEL's source came: 0 next */
    int level;                      /* what the pin was told */
};

struct oc {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NU + 1];
    int ev[32];
    int running;
};

static int mode(const struct unit *u)
{
    return (int)(u->con1->value & 7);
}

static int pwm(const struct unit *u)
{
    return mode(u) >= 6;
}

static int on(const struct unit *u)
{
    return u->con1 && mode(u) != 0;
}

static int held(const struct unit *u)
{
    return (u->con2->value & OCTRIG) && !(u->con2->value & TRIGSTAT);
}

static u32 clock_div(struct oc *m, const struct unit *u)
{
    static const int timer[8] = { 2, 3, 4, 5, 1, 0, 0, 0 };
    u32 sel = (u32)(u->con1->value >> 10 & 7);
    if (sel == 7)
        return 1;
    return timer[sel] ? pic24_timer_div(m->s, timer[sel]) : 0;
}

/* the pin: the output while the module is on and OCTRIS clear */
static void drive(struct oc *m, struct unit *u, u64 t)
{
    int lv = -1;
    if (on(u) && !(u->con2->value & OCTRIS))
        lv = u->out ^ !!(u->con2->value & OCINV);
    if (lv != u->level) {
        u->level = lv;
        if (u->fn)
            pic24_pps_out(m->s, u->fn, lv, t);
    }
}

static void set(struct oc *m, struct unit *u, int v, u64 t)
{
    u->out = v;
    drive(m, u, t);
}

static void irq(struct oc *m, struct unit *u, u64 t)
{
    if (u->irq >= 0)
        pic24_irq_at(m->s, u->irq, t);
}

/* the timer back at 0: a period's start */
static void period(struct oc *m, struct unit *u, u64 t)
{
    if (!pwm(u))
        return;
    u->rb = (u32)u->r->value & 0xffff;
    u->rsb = (u32)u->rs->value & 0xffff;
    if (mode(u) == 6) {
        set(m, u, u->rb != 0, t);
        irq(m, u, t);
    }
}

/* the timer at a new count, at time t: the matches */
static void compare(struct oc *m, struct unit *u, u64 t)
{
    u32 c = (u32)u->tmr->value & 0xffff;
    u32 r = pwm(u) ? u->rb : (u32)u->r->value & 0xffff;
    u32 rs = pwm(u) ? u->rsb : (u32)u->rs->value & 0xffff;
    if (c == rs) {
        pic24_event(m->s, PIC24_EV_OC_SYNC + u->k, t);
        if ((u->con1->value & TRIGMODE) && (u->con2->value & OCTRIG))
            rf_hw(u->con2, u->con2->value & ~(u64)TRIGSTAT);
    }
    switch (mode(u)) {
    case 1:
        if (c == r && !u->out) {
            set(m, u, 1, t);
            irq(m, u, t);
        }
        break;
    case 2:
        if (c == r && u->out) {
            set(m, u, 0, t);
            irq(m, u, t);
        }
        break;
    case 3:
        if (c == r) {
            set(m, u, !u->out, t);
            irq(m, u, t);
        }
        break;
    case 4:
    case 5:
    case 7:
        if (!u->out && !u->done && c == r) {
            set(m, u, 1, t);
        } else if (u->out && c == rs) {
            set(m, u, 0, t);
            irq(m, u, t);
            u->done = mode(u) == 4;
        }
        break;
    case 6:
        if (u->out && c == r)
            set(m, u, 0, t);
        break;
    }
}

/* one count of the timer's clock, at time t */
static void count(struct oc *m, struct unit *u, u64 t)
{
    u32 c = (u32)u->tmr->value & 0xffff;
    u32 rs = pwm(u) ? u->rsb : (u32)u->rs->value & 0xffff;
    int self = (u->con2->value & 31) == 31;
    if (held(u))
        return;
    if (u->sync || (self && c == rs) || c == 0xffff) {
        u->sync = 0;
        rf_hw(u->tmr, 0);
        period(m, u, t);
    } else {
        rf_hw(u->tmr, c + 1);
    }
    compare(m, u, t);
}

static void oc_tick(void *ctx, u32 cycles)
{
    struct oc *m = ctx;
    u64 t0 = m->s->cycles - cycles;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        u32 div;
        if (!on(u) || !(div = clock_div(m, u)))
            continue;
        for (u32 c = 1; c <= cycles; c++)
            if (++u->acc >= div) {
                u->acc = 0;
                count(m, u, t0 + c);
            }
    }
}

/* the matches still to come are events: one a count, at most */
static int oc_next(void *ctx, u32 *cycles)
{
    struct oc *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!on(u) || !clock_div(m, u) || held(u) ||
            (mode(u) == 1 && u->out) || (mode(u) == 2 && !u->out) ||
            (mode(u) == 4 && u->done))
            continue;
        *cycles = 1;
        return 1;
    }
    return 0;
}

static void run_clock(struct oc *m)
{
    int any = 0;
    for (int k = 1; k <= NU; k++)
        any |= on(&m->u[k]);
    rf_clock(m->p, &m->running, any);
}

/* an event SYNCSEL may select */
static void event(void *ctx, int ev, u64 when)
{
    struct oc *m = ctx;
    (void)when;                     /* the next count is 0 */
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        int sel = u->con1 ? (int)(u->con2->value & 31) : 0;
        if (!on(u) || !sel || sel == 31 || m->ev[sel] != ev ||
            ev == PIC24_EV_OC_SYNC + k)
            continue;
        if (u->con2->value & OCTRIG)
            rf_hw(u->con2, u->con2->value | TRIGSTAT);
        else
            u->sync = 1;
    }
}

static struct unit *unit_of(struct oc *m, u32 off)
{
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (u->con1 && ((off & ~1u) == rf_off(u->con1) || (off & ~1u) == rf_off(u->con2)))
            return u;
    }
    return 0;
}

static u32 oc_read(void *ctx, u32 off, int n)
{
    struct oc *m = ctx;
    return rf_read(m->p, off, n);
}

static void oc_write(void *ctx, u32 off, int n, u32 v)
{
    struct oc *m = ctx;
    struct unit *u = unit_of(m, off);
    u64 now = m->s->cycles;
    int was = u ? mode(u) : 0;
    rf_write(m->p, off, n, v);
    if (u) {
        if (held(u))
            rf_hw(u->tmr, 0);
        if ((off & ~1u) == rf_off(u->con1) && (mode(u) != was || mode(u) == 4)) {
            /* a mode written: the output as the mode starts it (a pulse's
             * sequence starts again, written the same) */
            u->done = 0;
            u->rb = (u32)u->r->value & 0xffff;
            u->rsb = (u32)u->rs->value & 0xffff;
            u->out = mode(u) == 2 || (mode(u) == 6 && u->rb != 0);
        }
        drive(m, u, now);
    }
    run_clock(m);
}

static void oc_reset(void *ctx)
{
    struct oc *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        u->acc = u->rb = u->rsb = 0;
        u->out = u->done = u->sync = 0;
        u->level = -1;
    }
    m->running = 0;
}

const struct dev_ops pic24_oc_ops = {
    "pic24-oc", oc_read, oc_write, oc_reset, oc_tick, oc_next,
};

void *pic24_oc_create(struct sim *s, struct rf_periph *p)
{
    struct oc *m = calloc(1, sizeof *m);
    char nm[16];
    int any = 0;
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
#define R(f, name) snprintf(nm, sizeof nm, "OC%d" name, k); u->f = rf_reg(p, nm)
        R(con1, "CON1"); R(con2, "CON2"); R(rs, "RS"); R(r, "R"); R(tmr, "TMR");
#undef R
        if (!u->con1 || !u->con2 || !u->rs || !u->r || !u->tmr) {
            memset(u, 0, sizeof *u);
            continue;
        }
        any = 1;
        u->k = k;
        snprintf(nm, sizeof nm, "OC%d", k);
        u->irq = pic24_irq_named(s, nm);
        u->fn = k <= 3 ? 12 + k : 0;    /* Table 11-4: OC1-3 are 13-15 */
        u->level = -1;
    }
    if (!any) {
        free(m);
        return 0;
    }
    for (int i = 0; i < 32; i++)
        m->ev[i] = sources[i] ? pic24_event_named(s, sources[i]) : -1;
    pic24_irq_listen(s, event, m);
    return m;
}
