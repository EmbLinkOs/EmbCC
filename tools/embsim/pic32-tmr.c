/* pic32-tmr.c -- a PIC32's 16-bit timers, over the ATDF's registers:
 * Timer1 (type A: TCKPS 1, 8, 64, 256) and Timer2 to Timer9 (type B:
 * TCKPS 1 to 256 in eight steps; T32 pairs an even timer with the next
 * odd one into a 32-bit timer).
 *
 *   - TMRx counts while TxCON.ON is set, one count per prescale clocks of
 *     peripheral bus 3 (PB3DIV: pic32_pb_div), from the core's cycles;
 *   - when it has reached PRx it goes back to 0 on the next count, and
 *     the timer's interrupt flag is set (TIMER_x); a TMRx above PRx counts
 *     on to 0xFFFF and wraps, without the flag;
 *   - in 32-bit mode (T32 in the even timer's TxCON) the even timer's
 *     TMR and PR are the low half, the odd one's the high half, and the
 *     flag is the odd timer's; the odd timer's own TxCON is not used;
 *   - a write to TMRx or TxCON clears the prescaler's count;
 *   - PRx is 0xFFFF out of reset.
 * TMRx is kept current as the core runs, so a program that polls it
 * reads the count. Not modelled: the external clock (TCS: the timer
 * stops, as with no clock on its pin), gated accumulation, the
 * asynchronous Timer1's write-in-progress bits. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

struct tmr {
    struct sim *s;
    struct rf_periph *p;
    int n;                          /* 1 to 9 */
    struct rf_reg *con, *tmr, *pr;
    struct tmr *odd;                /* an even timer's partner, for T32 */
    struct tmr *even;               /* an odd timer's */
    int irq, irq32;                 /* TIMER_n, and the pair's (the odd one's) */
    u32 acc;                        /* core cycles toward the next count */
    int running;
    struct rf_reg *pb3div;          /* the timers' bus's divider */
};

static const u32 psa[4] = { 1, 8, 64, 256 };
static const u32 psb[8] = { 1, 2, 4, 8, 16, 32, 64, 256 };

static int t32(const struct tmr *t)
{
    return t->odd && (t->con->value >> 3 & 1);
}

/* an odd timer whose even partner has it in 32-bit mode */
static int slave(const struct tmr *t)
{
    return t->even && t32(t->even);
}

static u32 divisor(const struct tmr *t)
{
    u32 ck = (u32)(t->con->value >> 4);
    u32 ps = t->n == 1 ? psa[ck & 3] : psb[ck & 7];
    return ps * pic32_pb_of(t->pb3div);
}

static int on(const struct tmr *t)
{
    return (t->con->value >> 15 & 1) && !(t->con->value >> 1 & 1) && !slave(t);
}

static u64 count_of(const struct tmr *t)
{
    if (t32(t))
        return (t->tmr->value & 0xffff) | (t->odd->tmr->value & 0xffff) << 16;
    return t->tmr->value & 0xffff;
}

static u64 period_of(const struct tmr *t)
{
    if (t32(t))
        return (t->pr->value & 0xffff) | (t->odd->pr->value & 0xffff) << 16;
    return t->pr->value & 0xffff;
}

static void set_count(struct tmr *t, u64 c)
{
    t->tmr->value = (t->tmr->value & ~0xffffull) | (c & 0xffff);
    if (t32(t))
        t->odd->tmr->value = (t->odd->tmr->value & ~0xffffull) | (c >> 16 & 0xffff);
}

static void run_clock(struct tmr *t)
{
    rf_clock(t->p, &t->running, on(t));
}

static void advance(struct tmr *t, u64 ticks)
{
    u64 c = count_of(t), pr = period_of(t);
    u64 top = t32(t) ? 0xffffffffull : 0xffff;
    int flag = 0;
    if (c > pr) {
        /* above PR: on to the top, and round to 0 */
        u64 room = top - c + 1;
        if (ticks < room) {
            set_count(t, c + ticks);
            return;
        }
        ticks -= room;
        c = 0;
    }
    /* from c (at most PR): every PR + 1 counts, a match and back to 0 */
    u64 to_match = pr - c + 1;
    if (ticks >= to_match) {
        flag = 1;
        ticks -= to_match;
        c = ticks % (pr + 1);
    } else {
        c += ticks;
    }
    set_count(t, c);
    if (flag)
        pic32_irq(t->s, t32(t) ? t->irq32 : t->irq);
}

static void tmr_tick(void *ctx, u32 cycles)
{
    struct tmr *t = ctx;
    u32 d = divisor(t);
    u64 a = (u64)t->acc + cycles;
    if (a < d) {
        t->acc = (u32)a;
        return;
    }
    t->acc = (u32)(a % d);
    advance(t, a / d);
}

/* when the flag next sets, while its interrupt is enabled */
static int tmr_next(void *ctx, u32 *cycles)
{
    struct tmr *t = ctx;
    if (!on(t) || !pic32_irq_enabled(t->s, t32(t) ? t->irq32 : t->irq))
        return 0;
    u64 c = count_of(t), pr = period_of(t);
    u64 top = t32(t) ? 0xffffffffull : 0xffff;
    u64 ticks = c <= pr ? pr - c + 1 : top - c + 1 + pr + 1;
    u64 cyc = ticks * divisor(t) - t->acc;
    *cycles = cyc > 0xffffffffu ? 0xffffffffu : (u32)cyc;
    return 1;
}

static u32 tmr_read(void *ctx, u32 off, int n)
{
    struct tmr *t = ctx;
    return rf_read(t->p, off, n);
}

static void tmr_write(void *ctx, u32 off, int n, u32 v)
{
    struct tmr *t = ctx;
    rf_write(t->p, off, n, v);
    if (off == rf_off(t->tmr) || off == rf_off(t->con))
        t->acc = 0;
    run_clock(t);
    if (t->odd)
        run_clock(t->odd);
}

static void tmr_reset(void *ctx)
{
    struct tmr *t = ctx;
    t->acc = 0;
    t->running = 0;
}

const struct dev_ops pic32_tmr_ops = {
    "pic32-tmr", tmr_read, tmr_write, tmr_reset, tmr_tick, tmr_next,
};

static struct tmr *timer_of(struct sim *s, int n)
{
    char nm[8];
    snprintf(nm, sizeof nm, "TMR%d", n);
    struct rf_periph *p = rf_periph(s->svd, nm);
    return p && p->ops == &pic32_tmr_ops ? p->ctx : 0;
}

void *pic32_tmr_create(struct sim *s, struct rf_periph *p)
{
    char nm[16];
    int n = 0;
    if (sscanf(p->name, "TMR%d", &n) != 1 || n < 1 || n > 9)
        return 0;
    struct tmr t;
    memset(&t, 0, sizeof t);
    t.s = s;
    t.p = p;
    t.n = n;
    snprintf(nm, sizeof nm, "T%dCON", n);
    t.con = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "TMR%d", n);
    t.tmr = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "PR%d", n);
    t.pr = rf_reg(p, nm);
    if (!t.con || !t.tmr || !t.pr)
        return 0;
    snprintf(nm, sizeof nm, "TIMER_%d", n);
    t.irq = pic32_irq_named(s, nm);
    rf_set_reset(t.pr, 0xffff);
    t.pb3div = pic32_pb_reg(s, 3);
    struct tmr *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = t;
    /* pairs: the even timer and the odd one after it, whichever is made
     * second finding the first */
    struct tmr *o = timer_of(s, n & 1 ? n - 1 : n + 1);
    if (o && n >= 2) {
        struct tmr *ev = n & 1 ? o : c, *od = n & 1 ? c : o;
        ev->odd = od;
        od->even = ev;
        ev->irq32 = od->irq;
    }
    return c;
}
