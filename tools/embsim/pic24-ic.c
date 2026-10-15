/* pic24-ic.c -- a PIC24FJ's input capture modules, IC1 to IC6, with their
 * dedicated timers (the EDC's IC: ICxCON1, ICxCON2, ICxBUF, ICxTMR), as
 * the data sheet's section 14 and the family reference manual's "Input
 * Capture with Dedicated Timer" (DS70000352) have them.
 *
 *   - ICxTMR counts, while the module is on, the clock ICTSEL selects:
 *     the instruction clock (111), or Timer1-5's prescaled clock while
 *     that timer runs (100 Timer1, 011 Timer5, 010 Timer4, 001 Timer2,
 *     000 Timer3); it wraps at 0xFFFF;
 *   - SYNCSEL's source -- Timer1-3's period match, the other ICs', the
 *     CCPs' and the ADC's interrupts, the OCs' and the CCPs' sync outputs
 *     -- clears it as it comes (ICTRIG 0); with ICTRIG 1 the timer is held
 *     at 0 until the source sets TRIGSTAT, or the program does (SYNCSEL 0
 *     then);
 *   - the module's input -- IC1-3 the pin the PPS maps to them (IC1R to
 *     IC3R), IC4-6 their own pins -- captures ICxTMR into a four-deep FIFO
 *     (ICxBUF, ICBNE) on the edges ICM selects: every edge (001), every
 *     falling (010) or rising (011) one, every fourth (100) or sixteenth
 *     (101) rising one. The capture is the timer's value at the time the
 *     edge came;
 *   - the ICx interrupt every ICI + 1 captures (every capture in 001); a
 *     capture with the FIFO full is lost, and is still an interrupt when
 *     ICI is 00 or ICM 001, else sets ICOV, which stops the captures
 *     until the FIFO has been read empty. Reading it empty clears ICBNE,
 *     ICOV and the count of captures; ICM 000 empties it, clears ICOV and
 *     the prescaler.
 * Not modelled: the cascaded 32-bit mode (IC32), the Sleep and Idle
 * interrupt pin mode (111), the comparators' and the CTMU's triggers,
 * the cycle or two the capture's synchronization takes. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define NU 6

/* ICxCON1 */
#define ICOV    (1u << 4)
#define ICBNE   (1u << 3)
/* ICxCON2 */
#define ICTRIG  (1u << 7)
#define TRIGSTAT (1u << 6)

/* SYNCSEL's sources, by the data sheet's Register 14-2 */
static const char *const sources[32] = {
    0, "OC1sync", "OC2sync", "OC3sync", "CCP1sync", "CCP2sync", "CCP3sync",
    "CCP4sync", "CCP5sync", "CCP6sync", "CCP7sync", "T1", "T2", "T3", "CCP6",
    "CCP7", "IC1", "IC2", "IC3", "CCP1", "CCP2", "CCP3", "CCP4", "CCP5", 0, 0,
    0, "ADC1", 0, "IC4", "IC5", "IC6",
};

struct unit {
    struct rf_reg *con1, *con2, *buf, *tmr;
    int irq;
    int pin;                        /* IC4-6's own pin; IC1-3's is the PPS's */
    char field[8];                  /* IC1R... */
    int level;                      /* the input, as last looked at */
    u16 fifo[4];
    int n, count, pre;              /* captures held; since the interrupt; edges */
    /* the timer: base counts at t0, then a count each div cycles (0:
     * stopped) */
    u32 base, div;
    u64 t0;
};

struct ic {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NU + 1];
    int ev[32];                     /* SYNCSEL's sources' events */
    int running;
};

static int mode(const struct unit *u)
{
    return (int)(u->con1->value & 7);
}

static int on(const struct unit *u)
{
    return u->con1 && mode(u) != 0 && mode(u) != 6;
}

static int held(const struct unit *u)
{
    return (u->con2->value & ICTRIG) && !(u->con2->value & TRIGSTAT);
}

/* the instruction cycles a count of ICTSEL's clock, or 0 */
static u32 clock_div(struct ic *m, const struct unit *u)
{
    static const int timer[8] = { 3, 2, 4, 5, 1, 0, 0, 0 };
    u32 sel = (u32)(u->con1->value >> 10 & 7);
    if (!on(u) || held(u))
        return 0;
    if (sel == 7)
        return 1;
    return timer[sel] ? pic24_timer_div(m->s, timer[sel]) : 0;
}

static u32 tmr_at(const struct unit *u, u64 t)
{
    if (!u->div || t < u->t0)
        return u->base & 0xffff;
    return (u->base + (u32)((t - u->t0) / u->div)) & 0xffff;
}

/* the timer from time t on: at its value then (or v), on its clock now */
static void rebase(struct ic *m, struct unit *u, u64 t, long v)
{
    u->base = v >= 0 ? (u32)v : tmr_at(u, t);
    u->t0 = t;
    u->div = clock_div(m, u);
}

static void run_clock(struct ic *m)
{
    int any = 0;
    for (int k = 1; k <= NU; k++)
        any |= on(&m->u[k]) && (m->u[k].con1->value >> 10 & 7) != 7;
    rf_clock(m->p, &m->running, any);
}

static int input(struct ic *m, const struct unit *u)
{
    int pin = u->pin >= 0 ? u->pin : pic24_pin_of_input(m->s, u->field);
    return pin >= 0 ? pic24_pin_level(m->s, pin) : 0;
}

static void empty(struct unit *u)
{
    u->n = u->count = 0;
    rf_hw(u->con1, u->con1->value & ~(u64)(ICOV | ICBNE));
}

static void capture(struct ic *m, struct unit *u, u64 t)
{
    u32 c1 = (u32)u->con1->value, ici = c1 >> 5 & 3;
    if (u->n == 4) {
        if (!ici || mode(u) == 1) {
            if (u->irq >= 0)
                pic24_irq_at(m->s, u->irq, t);
        } else {
            rf_hw(u->con1, c1 | ICOV);
        }
        return;
    }
    if (c1 & ICOV)
        return;
    u->fifo[u->n++] = (u16)tmr_at(u, t);
    rf_hw(u->con1, c1 | ICBNE);
    if (mode(u) == 1 || ++u->count >= (int)ici + 1) {
        u->count = 0;
        if (u->irq >= 0)
            pic24_irq_at(m->s, u->irq, t);
    }
}

static void edge(struct ic *m, struct unit *u, int rising, u64 t)
{
    int cap = 0;
    switch (mode(u)) {
    case 1: cap = 1; break;
    case 2: cap = !rising; break;
    case 3: cap = rising; break;
    case 4:
    case 5:
        if (rising && ++u->pre >= (mode(u) == 4 ? 4 : 16)) {
            u->pre = 0;
            cap = 1;
        }
        break;
    }
    if (cap)
        capture(m, u, t);
}

/* the pins may have changed: each input against its last level */
static void watch(void *ctx, u64 when)
{
    struct ic *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!on(u))
            continue;
        int lv = input(m, u);
        if (lv != u->level) {
            u->level = lv;
            edge(m, u, lv, when);
        }
    }
}

/* an event SYNCSEL may select */
static void event(void *ctx, int ev, u64 now)
{
    struct ic *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        int sel = u->con1 ? (int)(u->con2->value & 31) : 0;
        if (!on(u) || !sel || m->ev[sel] != ev || ev == u->irq)
            continue;
        if (u->con2->value & ICTRIG) {
            if (!(u->con2->value & TRIGSTAT)) {
                rf_hw(u->con2, u->con2->value | TRIGSTAT);
                rebase(m, u, now, 0);
            }
        } else {
            rebase(m, u, now, 0);
        }
    }
}

static void ic_tick(void *ctx, u32 cycles)
{
    struct ic *m = ctx;
    u64 t = m->s->cycles - cycles;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (on(u) && clock_div(m, u) != u->div)
            rebase(m, u, t, -1);    /* its timer started, stopped, or changed */
    }
}

static struct unit *unit_of(struct ic *m, u32 off, struct rf_reg **which)
{
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        struct rf_reg *r[4] = { u->con1, u->con2, u->buf, u->tmr };
        for (int j = 0; u->con1 && j < 4; j++)
            if ((off & ~1u) == rf_off(r[j])) {
                *which = r[j];
                return u;
            }
    }
    return 0;
}

static u32 ic_read(void *ctx, u32 off, int n)
{
    struct ic *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    if (u && r == u->tmr) {
        rf_hw(u->tmr, tmr_at(u, m->s->cycles));
    } else if (u && r == u->buf) {
        rf_hw(u->buf, u->fifo[0]);
        if (u->n && !m->s->bus.debug && (n == 2 || (off & 1))) {
            memmove(u->fifo, u->fifo + 1, sizeof u->fifo - sizeof u->fifo[0]);
            if (!--u->n)
                empty(u);
        }
    }
    return rf_read(m->p, off, n);
}

static void ic_write(void *ctx, u32 off, int n, u32 v)
{
    struct ic *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    u64 now = m->s->cycles;
    if (!u) {
        rf_write(m->p, off, n, v);
        return;
    }
    int was = on(u), was_held = held(u);
    u32 old = tmr_at(u, now);
    rf_write(m->p, off, n, v);      /* ICxTMR: read-only, as the EDC has it */
    if (r == u->con1 || r == u->con2) {
        if (mode(u) == 0) {
            empty(u);
            u->pre = 0;
        }
        if (on(u) && !was)
            u->level = input(m, u); /* no edge: the input as it is */
        if (held(u))
            rebase(m, u, now, 0);
        else
            rebase(m, u, now, was_held ? 0 : (long)old);
    }
    run_clock(m);
}

static void ic_reset(void *ctx)
{
    struct ic *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con1)
            continue;
        u->n = u->count = u->pre = 0;
        u->base = u->div = 0;
        u->t0 = 0;
        u->level = 0;
    }
    m->running = 0;
}

const struct dev_ops pic24_ic_ops = {
    "pic24-ic", ic_read, ic_write, ic_reset, ic_tick, 0,
};

void *pic24_ic_create(struct sim *s, struct rf_periph *p)
{
    struct ic *m = calloc(1, sizeof *m);
    char nm[16];
    int any = 0;
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
#define R(f, name) snprintf(nm, sizeof nm, "IC%d" name, k); u->f = rf_reg(p, nm)
        R(con1, "CON1"); R(con2, "CON2"); R(buf, "BUF"); R(tmr, "TMR");
#undef R
        if (!u->con1 || !u->con2 || !u->buf || !u->tmr) {
            memset(u, 0, sizeof *u);
            continue;
        }
        any = 1;
        snprintf(nm, sizeof nm, "IC%d", k);
        u->irq = pic24_irq_named(s, nm);
        u->pin = pic24_pin(s, nm);  /* a pin named for it: IC4-6 */
        snprintf(u->field, sizeof u->field, "IC%dR", k);
        rf_set_acc(u->con1, ICOV | ICBNE, ACC_RO);
        rf_set_acc(u->buf, 0xffff, ACC_RO);
    }
    if (!any) {
        free(m);
        return 0;
    }
    for (int i = 0; i < 32; i++)
        m->ev[i] = sources[i] ? pic24_event_named(s, sources[i]) : -1;
    pic24_irq_listen(s, event, m);
    pic24_pin_watch(s, watch, m);
    return m;
}
