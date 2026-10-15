/* pic32-evic.c -- a PIC32MZ's interrupt controller (the EVIC), over its
 * registers in the ATDF (module INT), driving the core in EIC mode.
 *
 *   - An interrupt is requested while its flag (IFSx) and its enable
 *     (IECx) are set and its priority (IPCx's IP, 1 to 7) is not 0. Of
 *     those, the highest priority is presented to the core, then the
 *     highest subpriority (IS), then the lowest number (the natural
 *     order).
 *   - The core takes it when the priority is above Status.IPL: at EBASE
 *     plus OFFn of its vector (the vector is the interrupt's number) in
 *     multi-vector mode (INTCON.MVEC), at EBASE + 0x200 in single-vector
 *     mode; in the shadow register set PRISS gives its priority (PRIxSS;
 *     SS0 in single-vector mode). INTSTAT then says which (SIRQ) and at
 *     what level (SRIPL).
 *   - The core's own interrupts are numbers 0 to 2: its timer (Cause.TI)
 *     and the two software interrupts (Cause.IP0, IP1) set CTIF, CS0IF
 *     and CS1IF as they rise; the program clears them, as the others.
 *   - The flags are the hardware's: a model sets one with pic32_irq,
 *     and the program clears it (IFSxCLR). Writing one sets it too, as
 *     on the part (a software-triggered interrupt). A persistent source
 *     (a UART's transmitter with room) sets its flag again as soon as it
 *     is cleared, while its condition holds.
 *
 * Not modelled: INTCON's external interrupt edges (INTxEP: no pin is
 * driven), the proximity timer (TPC, IPTMR). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "mips.h"
#include "pic32.h"
#include "svd-map.h"

struct evic {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *intcon, *priss, *intstat;
    struct rf_reg *ifs[8], *iec[8], *ipc[64], *off[256];
    int nw;                         /* IFS/IEC words */
    struct mips_eic eic;
    int presented;                  /* the interrupt presented, or -1 */
    u32 lines;                      /* the core's lines, as last told */
    struct level { int irq; int (*cond)(void *); void *ctx; } lv[16];
    int nlv;
};

static struct evic *evic_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "INT") : 0;
    return p && p->ops == &pic32_evic_ops ? p->ctx : 0;
}

static int prio(const struct evic *e, int irq, int *sub)
{
    const struct rf_reg *r = e->ipc[irq / 4];
    u32 b = r ? (u32)(r->value >> (8 * (irq % 4))) : 0;
    *sub = (int)(b & 3);
    return (int)((b >> 2) & 7);
}

static void set_flag(struct evic *e, int irq);

/* what the controller presents to the core; first, the persistent
 * sources' flags, which a write cannot clear while their condition holds */
static void evaluate(struct evic *e)
{
    for (int i = 0; i < e->nlv; i++)
        if (e->lv[i].cond(e->lv[i].ctx))
            set_flag(e, e->lv[i].irq);
    int best = -1, bp = 0, bs = -1;
    for (int w = 0; w < e->nw; w++) {
        u32 pend = (u32)(e->ifs[w]->value & e->iec[w]->value);
        while (pend) {
            int bit = 0;
            while (!(pend >> bit & 1))
                bit++;
            pend &= pend - 1;
            int irq = 32 * w + bit, sub, pr = prio(e, irq, &sub);
            if (pr && (pr > bp || (pr == bp && sub > bs))) {
                best = irq;
                bp = pr;
                bs = sub;
            }
        }
    }
    int mvec = (e->intcon->value >> 12) & 1;
    e->presented = best;
    e->eic.ripl = bp;
    e->eic.offset = 0x200;
    if (best >= 0 && mvec && e->off[best])
        e->eic.offset = (u32)e->off[best]->value & 0x3fffeu;
    u32 priss = (u32)e->priss->value;
    e->eic.srs = mvec ? (int)((priss >> (4 * bp)) & 15) : (int)(priss & 1);
    if (!bp)
        e->eic.srs = 0;
    mips_eic_changed(e->s);
}

static void set_flag(struct evic *e, int irq)
{
    struct rf_reg *r = irq >= 0 && irq / 32 < e->nw ? e->ifs[irq / 32] : 0;
    u64 bit = 1ull << (irq % 32);
    if (r && !(r->value & bit))
        rf_hw(r, r->value | bit);
}

void pic32_irq(struct sim *s, int irq)
{
    struct evic *e = evic_of(s);
    if (!e || irq < 0)
        return;
    set_flag(e, irq);
    evaluate(e);
}

int pic32_irq_flag(struct sim *s, int irq)
{
    struct evic *e = evic_of(s);
    if (!e || irq < 0 || irq / 32 >= e->nw)
        return 0;
    return (int)(e->ifs[irq / 32]->value >> (irq % 32) & 1);
}

int pic32_irq_enabled(struct sim *s, int irq)
{
    struct evic *e = evic_of(s);
    int sub;
    if (!e || irq < 0 || irq / 32 >= e->nw)
        return 0;
    return (int)(e->iec[irq / 32]->value >> (irq % 32) & 1) &&
           prio(e, irq, &sub) > 0;
}

int pic32_irq_named(struct sim *s, const char *name)
{
    return s->svd ? rf_dev_irq(s->svd, name) : -1;
}

void pic32_irq_level(struct sim *s, int irq, int (*cond)(void *ctx), void *ctx)
{
    struct evic *e = evic_of(s);
    if (!e || irq < 0)
        return;
    if (e->nlv == (int)(sizeof e->lv / sizeof e->lv[0]))
        die("too many persistent interrupt sources");
    e->lv[e->nlv].irq = irq;
    e->lv[e->nlv].cond = cond;
    e->lv[e->nlv].ctx = ctx;
    e->nlv++;
}

/* ---- the core's side ---------------------------------------------------- */

static void core_irq(void *ctx, int timer, int sw0, int sw1)
{
    struct evic *e = ctx;
    u32 l = (u32)(timer != 0) | (u32)(sw0 != 0) << 1 | (u32)(sw1 != 0) << 2;
    u32 rise = l & ~e->lines;
    e->lines = l;
    for (int i = 0; i < 3; i++)
        if (rise >> i & 1)
            set_flag(e, i);
    if (rise)
        evaluate(e);
}

static void taken(void *ctx)
{
    struct evic *e = ctx;
    if (e->presented >= 0)
        rf_hw(e->intstat, (e->intstat->value & ~0x7ffull) |
                              (u64)e->eic.ripl << 8 | (u64)(e->presented & 0xff));
}

static int timer_on(void *ctx)
{
    struct evic *e = ctx;
    int sub;
    return (int)(e->iec[0]->value & 1) && prio(e, 0, &sub) > 0;
}

/* ---- the registers ------------------------------------------------------- */

static u32 evic_read(void *ctx, u32 off, int n)
{
    struct evic *e = ctx;
    return rf_read(e->p, off, n);
}

static void evic_write(void *ctx, u32 off, int n, u32 v)
{
    struct evic *e = ctx;
    rf_write(e->p, off, n, v);
    evaluate(e);
}

static void evic_reset(void *ctx)
{
    struct evic *e = ctx;
    e->lines = 0;
    evaluate(e);
}

const struct dev_ops pic32_evic_ops = {
    "pic32-evic", evic_read, evic_write, evic_reset, 0, 0,
};

void *pic32_evic_create(struct sim *s, struct rf_periph *p)
{
    struct evic e;
    char nm[16];
    memset(&e, 0, sizeof e);
    e.s = s;
    e.p = p;
    e.intcon = rf_reg(p, "INTCON");
    e.priss = rf_reg(p, "PRISS");
    e.intstat = rf_reg(p, "INTSTAT");
    for (int i = 0; i < 8; i++) {
        snprintf(nm, sizeof nm, "IFS%d", i);
        e.ifs[i] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "IEC%d", i);
        e.iec[i] = rf_reg(p, nm);
        if (!e.ifs[i] || !e.iec[i])
            break;
        e.nw = i + 1;
    }
    for (int i = 0; i < 64; i++) {
        snprintf(nm, sizeof nm, "IPC%d", i);
        e.ipc[i] = rf_reg(p, nm);
    }
    for (int i = 0; i < 256; i++) {
        snprintf(nm, sizeof nm, "OFF%03d", i);
        e.off[i] = rf_reg(p, nm);
    }
    if (!e.intcon || !e.priss || !e.intstat || !e.nw || !mips_is(s->cpu))
        return 0;
    struct evic *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = e;
    c->eic.core_irq = core_irq;
    c->eic.taken = taken;
    c->eic.timer_on = timer_on;
    c->eic.ctx = c;
    c->presented = -1;
    mips_eic_attach(s, &c->eic);
    return c;
}
