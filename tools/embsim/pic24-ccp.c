/* pic24-ccp.c -- a PIC24FJ's capture/compare/PWM/timer modules: MCCP1-3
 * and SCCP4-7 (the EDC's CCP: CCPxCON1L to CCPxBUFH), as the data
 * sheet's section 16 and the family reference manual's "Capture/Compare/
 * PWM/Timer (MCCP and SCCP)" (DS30003035) have them.
 *
 *   - The time base counts, while CCPON, the system clock (CLKSEL 000:
 *     the instruction clock) or twice it (100, the peripheral clock's
 *     double) over TMRPS (1, 4, 16, 64). It goes back to 0 on the count
 *     after it matched its period (CCPxPRL; with T32 in Timer mode
 *     CCPxPRH:CCPxPRL), or after SYNC's source came (another CCP's sync
 *     output: its time base resetting); a period of 0 holds it. Each reset
 *     is the module's sync output and the CCTx interrupt. With TRIGEN the
 *     time base is held at 0 until the source (or TRSET) sets CCPTRIG,
 *     and TRCLR holds it again;
 *   - Timer mode (CCSEL 0, MOD 0000): T32 0 is two 16-bit timers, the
 *     second (CCPxTMRH, CCPxPRH) giving the CCPx interrupt at its period;
 *     T32 1 one 32-bit timer;
 *   - output compare (CCSEL 0): the output set (0001) or cleared (0010)
 *     at CCPxTMRL's match with CCPxRA, once until the program clears
 *     SCEVT, which returns the output to its start; toggled at each match
 *     (0011); a pulse each period from the match with CCPxRA to the match
 *     with CCPxRB (0100; 0101 too, with CCPxRA and CCPxRB taking effect
 *     at the period's start), none when they are equal. The CCPx
 *     interrupt at each match that changes the output's end (SCEVT set in
 *     the single edge modes). The output drives the pins OCxEN enables --
 *     an MCCP's OCMxA to OCMxF, an SCCP's OCMx through the PPS (output
 *     functions 16-18, 27) -- inverted by POLACE (A, C, E) and POLBDF (B,
 *     D, F);
 *   - input capture (CCSEL 1): the time base runs free (16 bits, or 32
 *     with T32, the FIFO then two deep), and the ICMx pin's edges MOD
 *     selects -- every edge (0000, 0011), rising (0001), falling (0010),
 *     every fourth (0100) or sixteenth (0101) rising one -- capture it
 *     into a four-deep FIFO (CCPxBUFL, CCPxBUFH; ICBNE), unless ICDIS;
 *     the CCPx interrupt every OPS + 1 captures. A capture with the FIFO
 *     full sets ICOV (not in Edge Detect mode, 0000), which stops the
 *     captures, and their interrupts unless OPS is 0, until the FIFO is
 *     read empty or ICOV cleared, which empties it.
 * The capture is the time base's count when the edge reaches the module.
 * Not modelled: the center-aligned (0110), variable frequency (0111) and
 * external input (1111) modes (the output stays at its start), the
 * output modes other than steering (OUTM), dead time, auto-shutdown and
 * gating, one-shot triggers, the postscaler in the compare modes, the
 * 32-bit compare, the clocks other than the system clock and its double,
 * the comparators', the CLCs', the ADC's, the CTMU's and the INTx pins'
 * sync sources, the capture sources other than the pin (ICS). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define NU 7

/* CCPxCON1L */
#define CCPON   (1u << 15)
#define T32     (1u << 5)
#define CCSEL   (1u << 4)
/* CCPxCON1H */
#define TRIGEN  (1u << 7)
/* CCPxCON3H */
#define POLACE  (1u << 5)
#define POLBDF  (1u << 4)
/* CCPxSTATL */
#define ICBNE   (1u << 0)
#define ICOV    (1u << 1)
#define ICDIS   (1u << 2)
#define SCEVT   (1u << 3)
#define ASEVT   (1u << 4)
#define TRCLR   (1u << 5)
#define TRSET   (1u << 6)
#define CCPTRIG (1u << 7)

/* the capture pins of the PIC24FJ1024GB610 family, which its EDC leaves
 * unnamed: the data sheet's Table 1-1 */
static const char *const icm_pins[NU + 1] = {
    0, "RG6", "RG8", "RB5", "RD1", "RD8", "RD0", "RD3",
};

struct unit {
    struct rf_reg *c1l, *c1h, *c2h, *c3h, *stat, *tmrl, *tmrh, *prl, *prh;
    struct rf_reg *ral, *rbl, *bufl, *bufh;
    int k, irq, irq_t;              /* CCPx, CCTx */
    int fn;                         /* an SCCP's PPS output function */
    int pin[6];                     /* an MCCP's OCMxA to OCMxF */
    int level[6];                   /* what each was told */
    int icm;                        /* the capture pin */
    u32 acc;                        /* half cycles toward the next count */
    u32 rab, rbb;                   /* CCPxRA, CCPxRB as the period has them */
    int out;                        /* the compare output */
    int sync;                       /* SYNC's source came: 0 next */
    int input;                      /* the capture pin, as last looked at */
    u32 fifo[4];
    int n, count, pre;
};

struct ccp {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NU + 1];
    int running;
};

static u32 val(const struct rf_reg *r)
{
    return (u32)r->value & 0xffff;
}

static int on(const struct unit *u)
{
    return u->c1l && (u->c1l->value & CCPON);
}

static int capturing(const struct unit *u)
{
    return on(u) && (u->c1l->value & CCSEL);
}

static int mode(const struct unit *u)
{
    return (int)(u->c1l->value & 15);
}

/* the half cycles a count of the time base's clock, or 0 */
static u32 clock_halves(const struct unit *u)
{
    static const u32 ps[4] = { 1, 4, 16, 64 };
    u32 sel = (u32)(u->c1l->value >> 8 & 7), pre = ps[u->c1l->value >> 6 & 3];
    return sel == 0 ? 2 * pre : sel == 4 ? pre : 0;
}

/* SYNC selects another module's sync output: the source's number, else
 * 0 (the module's own period) */
static int ext_sync(const struct unit *u)
{
    int sel = (int)(u->c1h->value & 31), k = sel - 1;
    if (sel >= 2 && sel <= 8 && k != u->k)
        return k;
    if (sel == 0 || sel == 1 || sel == 31 || (sel >= 2 && sel <= 8))
        return 0;
    return -1;                      /* a source not modelled: it never comes */
}

static void irq(struct ccp *m, int n, u64 t)
{
    if (n >= 0)
        pic24_irq_at(m->s, n, t);
}

/* the output on the pins, from time t */
static void drive(struct ccp *m, struct unit *u, u64 t)
{
    int outputs = on(u) && !(u->c1l->value & CCSEL) && mode(u) != 0;
    u32 en = u->c2h ? (u32)(u->c2h->value >> 8 & 0x3f) : 0;
    u32 pol = u->c3h ? (u32)u->c3h->value : 0;
    for (int j = 0; j < 6; j++) {
        int lv = -1;
        if (outputs && (en >> j & 1))
            lv = u->out ^ !!(pol & (j & 1 ? POLBDF : POLACE));
        if (lv == u->level[j])
            continue;
        u->level[j] = lv;
        if (u->fn && j == 0)
            pic24_pps_out(m->s, u->fn, lv, t);
        else if (u->pin[j] >= 0)
            pic24_pin_out(m->s, u->pin[j], lv, t);
    }
}

static void set(struct ccp *m, struct unit *u, int v, u64 t)
{
    u->out = v;
    drive(m, u, t);
}

/* the output as the mode starts it */
static void start(struct ccp *m, struct unit *u, u64 t)
{
    u->rab = val(u->ral);
    u->rbb = val(u->rbl);
    set(m, u, mode(u) == 2, t);
}

/* the time base back at 0: the sync output, CCTx, the buffers */
static void period(struct ccp *m, struct unit *u, u64 t)
{
    pic24_event(m->s, PIC24_EV_CCP_SYNC + u->k, t);
    irq(m, u->irq_t, t);
    u->rab = val(u->ral);
    u->rbb = val(u->rbl);
}

static void compare(struct ccp *m, struct unit *u, u64 t)
{
    u32 c = val(u->tmrl), st = (u32)u->stat->value;
    u32 ra = mode(u) == 5 ? u->rab : val(u->ral);
    u32 rb = mode(u) == 5 ? u->rbb : val(u->rbl);
    switch (mode(u)) {
    case 1:
    case 2:
    case 3:
        if (c != ra || (mode(u) != 3 && (st & SCEVT)))
            break;
        set(m, u, mode(u) == 1 ? 1 : mode(u) == 2 ? 0 : !u->out, t);
        rf_hw(u->stat, st | SCEVT);
        irq(m, u->irq, t);
        break;
    case 4:
    case 5:
        if (ra == rb)
            break;
        if (c == ra) {
            set(m, u, 1, t);
        } else if (c == rb) {
            set(m, u, 0, t);
            irq(m, u->irq, t);
        }
        break;
    }
}

/* one count of the time base, at time t */
static void count(struct ccp *m, struct unit *u, u64 t)
{
    int cap = (u->c1l->value & CCSEL) != 0, wide = (u->c1l->value & T32) != 0;
    int ext = ext_sync(u);
    if (((u->c1h->value & TRIGEN) && !(u->stat->value & CCPTRIG)) || ext < 0)
        return;                     /* held at 0 */
    if (wide && (cap || mode(u) == 0)) {
        u32 v = val(u->tmrl) | val(u->tmrh) << 16;
        u32 pr = cap ? 0xffffffffu : val(u->prl) | val(u->prh) << 16;
        if (!pr && !cap)
            return;
        if (u->sync || (!ext && v == pr) || v == 0xffffffffu) {
            u->sync = 0;
            v = 0;
            period(m, u, t);
        } else {
            v++;
        }
        rf_hw(u->tmrl, v & 0xffff);
        rf_hw(u->tmrh, v >> 16);
        return;
    }
    u32 v = val(u->tmrl), pr = cap ? 0xffff : val(u->prl);
    if (pr) {
        if (u->sync || (!ext && v == pr) || v == 0xffff) {
            u->sync = 0;
            rf_hw(u->tmrl, 0);
            period(m, u, t);
        } else {
            rf_hw(u->tmrl, v + 1);
        }
        if (!cap)
            compare(m, u, t);
    }
    if (!cap && mode(u) == 0) {
        /* the secondary timer: its period the CCPx interrupt */
        u32 h = val(u->tmrh), prh = val(u->prh);
        if (!prh)
            return;
        if (h == prh) {
            rf_hw(u->tmrh, 0);
            irq(m, u->irq, t);
        } else {
            rf_hw(u->tmrh, h + 1);
        }
    }
}

static void ccp_tick(void *ctx, u32 cycles)
{
    struct ccp *m = ctx;
    u64 t0 = m->s->cycles - cycles;
    for (u32 h = 1; h <= 2 * cycles; h++)
        for (int k = 1; k <= NU; k++) {
            struct unit *u = &m->u[k];
            u32 halves;
            if (!on(u) || !(halves = clock_halves(u)))
                continue;
            if (++u->acc >= halves) {
                u->acc = 0;
                count(m, u, t0 + (h + 1) / 2);
            }
        }
}

static int ccp_next(void *ctx, u32 *cycles)
{
    struct ccp *m = ctx;
    for (int k = 1; k <= NU; k++)
        if (on(&m->u[k]) && clock_halves(&m->u[k])) {
            *cycles = 1;
            return 1;
        }
    return 0;
}

static void run_clock(struct ccp *m)
{
    int any = 0;
    for (int k = 1; k <= NU; k++)
        any |= on(&m->u[k]);
    rf_clock(m->p, &m->running, any);
}

static void empty(struct unit *u)
{
    u->n = u->count = 0;
    rf_hw(u->stat, u->stat->value & ~(u64)(ICBNE | ICOV));
}

static void capture(struct ccp *m, struct unit *u, u64 t)
{
    int wide = (u->c1l->value & T32) != 0, depth = wide ? 2 : 4;
    u32 ops = (u32)(u->c1h->value >> 8 & 15), st = (u32)u->stat->value;
    u32 v = val(u->tmrl) | (wide ? val(u->tmrh) << 16 : 0);
    if (u->n == depth || (st & ICOV)) {
        if (mode(u) == 0) {         /* Edge Detect: no overflow */
            if (++u->count >= (int)ops + 1) {
                u->count = 0;
                irq(m, u->irq, t);
            }
            return;
        }
        rf_hw(u->stat, st | ICOV);
        if (!ops)
            irq(m, u->irq, t);
        return;
    }
    u->fifo[u->n++] = v;
    rf_hw(u->stat, st | ICBNE);
    if (++u->count >= (int)ops + 1) {
        u->count = 0;
        irq(m, u->irq, t);
    }
}

static int input(struct ccp *m, const struct unit *u)
{
    return u->icm >= 0 && !(u->c2h->value & 7) ? pic24_pin_level(m->s, u->icm) : 0;
}

/* the pins may have changed: each capture input against its last level */
static void watch(void *ctx, u64 when)
{
    struct ccp *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!capturing(u))
            continue;
        int lv = input(m, u), cap = 0;
        if (lv == u->input)
            continue;
        u->input = lv;
        if (u->stat->value & ICDIS)
            continue;
        switch (mode(u)) {
        case 0:
        case 3: cap = 1; break;
        case 1: cap = lv; break;
        case 2: cap = !lv; break;
        case 4:
        case 5:
            if (lv && ++u->pre >= (mode(u) == 4 ? 4 : 16)) {
                u->pre = 0;
                cap = 1;
            }
            break;
        }
        if (cap)
            capture(m, u, when);
    }
}

/* another module's sync output */
static void event(void *ctx, int ev, u64 when)
{
    struct ccp *m = ctx;
    (void)when;                     /* the next count is 0 */
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        int src = on(u) ? ext_sync(u) : 0;
        if (src <= 0 || ev != PIC24_EV_CCP_SYNC + src)
            continue;
        if (u->c1h->value & TRIGEN)
            rf_hw(u->stat, u->stat->value | CCPTRIG);
        else
            u->sync = 1;
    }
}

static struct unit *unit_of(struct ccp *m, u32 off, struct rf_reg **which)
{
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        struct rf_reg *r[6] = { u->c1l, u->c1h, u->c2h, u->c3h, u->stat, u->bufl };
        if (!u->c1l)
            continue;
        for (int j = 0; j < 6; j++)
            if (r[j] && (off & ~1u) == rf_off(r[j])) {
                *which = r[j];
                return u;
            }
        if ((off & ~1u) == rf_off(u->bufh) || (off & ~1u) == rf_off(u->ral) ||
            (off & ~1u) == rf_off(u->rbl)) {
            *which = (off & ~1u) == rf_off(u->bufh) ? u->bufh :
                     (off & ~1u) == rf_off(u->ral) ? u->ral : u->rbl;
            return u;
        }
    }
    return 0;
}

static u32 ccp_read(void *ctx, u32 off, int n)
{
    struct ccp *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    if (u && (r == u->bufl || r == u->bufh)) {
        int wide = (u->c1l->value & T32) != 0;
        rf_hw(u->bufl, u->fifo[0] & 0xffff);
        rf_hw(u->bufh, wide ? u->fifo[0] >> 16 : 0);
        /* the FIFO moves on with its most significant word read */
        if (u->n && !m->s->bus.debug && r == (wide ? u->bufh : u->bufl) &&
            (n == 2 || (off & 1))) {
            u32 head = u->fifo[0];
            memmove(u->fifo, u->fifo + 1, sizeof u->fifo - sizeof u->fifo[0]);
            u->fifo[3] = head;
            if (!--u->n)
                empty(u);
        }
    }
    return rf_read(m->p, off, n);
}

static void ccp_write(void *ctx, u32 off, int n, u32 v)
{
    struct ccp *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    u64 now = m->s->cycles;
    if (!u) {
        rf_write(m->p, off, n, v);
        return;
    }
    if (r == u->stat) {
        u32 old = (u32)u->stat->value, w, st = old;
        w = n == 2 ? v : off & 1 ? (v & 0xff) << 8 | (old & 0xff) : (old & 0xff00) | (v & 0xff);
        st = (st & ~(ICDIS | SCEVT | ASEVT)) | (w & (ICDIS | SCEVT | ASEVT));
        if ((old & ICOV) && !(w & ICOV)) {
            st &= ~(ICOV | ICBNE);  /* the FIFO discarded */
            u->n = u->count = 0;
        }
        if ((w & TRSET) && (u->c1h->value & TRIGEN))
            st |= CCPTRIG;
        if (w & TRCLR) {
            st &= ~CCPTRIG;
            rf_hw(u->tmrl, 0);
            rf_hw(u->tmrh, 0);
        }
        rf_hw(u->stat, st);
        if ((old & SCEVT) && !(st & SCEVT) && (mode(u) == 1 || mode(u) == 2))
            set(m, u, mode(u) == 2, now);   /* re-armed: the output at its start */
        return;
    }
    u32 was = (u32)u->c1l->value;
    rf_write(m->p, off, n, v);
    if (r == u->c1l) {
        u32 now_c = (u32)u->c1l->value;
        if ((now_c ^ was) & (CCPON | CCSEL | 15 | T32)) {
            empty(u);
            u->pre = u->acc = 0;
            u->input = input(m, u);
            start(m, u, now);
        }
    }
    if ((r == u->ral || r == u->rbl) && !on(u)) {
        u->rab = val(u->ral);       /* off: the buffers take it at once */
        u->rbb = val(u->rbl);
    }
    drive(m, u, now);
    run_clock(m);
}

static void ccp_reset(void *ctx)
{
    struct ccp *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        u->acc = u->rab = u->rbb = 0;
        u->out = u->sync = u->input = 0;
        u->n = u->count = u->pre = 0;
        for (int j = 0; j < 6; j++)
            u->level[j] = -1;
    }
    m->running = 0;
}

const struct dev_ops pic24_ccp_ops = {
    "pic24-ccp", ccp_read, ccp_write, ccp_reset, ccp_tick, ccp_next,
};

void *pic24_ccp_create(struct sim *s, struct rf_periph *p)
{
    static const int sccp_fn[NU + 1] = { 0, 0, 0, 0, 16, 17, 18, 27 };
    struct ccp *m = calloc(1, sizeof *m);
    char nm[24];
    int any = 0;
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
#define R(f, name) snprintf(nm, sizeof nm, "CCP%d" name, k); u->f = rf_reg(p, nm)
        R(c1l, "CON1L"); R(c1h, "CON1H"); R(c2h, "CON2H"); R(c3h, "CON3H");
        R(stat, "STATL"); R(tmrl, "TMRL"); R(tmrh, "TMRH"); R(prl, "PRL");
        R(prh, "PRH"); R(ral, "RAL"); R(rbl, "RBL"); R(bufl, "BUFL"); R(bufh, "BUFH");
#undef R
        if (!u->c1l || !u->c1h || !u->c2h || !u->stat || !u->tmrl || !u->tmrh ||
            !u->prl || !u->prh || !u->ral || !u->rbl || !u->bufl || !u->bufh) {
            memset(u, 0, sizeof *u);
            continue;
        }
        any = 1;
        u->k = k;
        snprintf(nm, sizeof nm, "CCP%d", k);
        u->irq = pic24_irq_named(s, nm);
        snprintf(nm, sizeof nm, "CCT%d", k);
        u->irq_t = pic24_irq_named(s, nm);
        for (int j = 0; j < 6; j++) {
            snprintf(nm, sizeof nm, "OCM%d%c", k, 'A' + j);
            u->pin[j] = pic24_pin(s, nm);
            u->level[j] = -1;
        }
        u->fn = u->pin[0] < 0 ? sccp_fn[k] : 0;
        snprintf(nm, sizeof nm, "ICM%d", k);
        u->icm = pic24_pin(s, nm);
        if (u->icm < 0)
            u->icm = pic24_pin(s, icm_pins[k]);
        rf_set_acc(u->stat, 0xffff, ACC_RO);
        rf_set_acc(u->bufl, 0xffff, ACC_RO);
        rf_set_acc(u->bufh, 0xffff, ACC_RO);
    }
    if (!any) {
        free(m);
        return 0;
    }
    pic24_irq_listen(s, event, m);
    pic24_pin_watch(s, watch, m);
    return m;
}
