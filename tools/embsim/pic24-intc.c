/* pic24-intc.c -- a PIC24F's interrupt controller, over its registers in
 * the EDC (INTCON1, INTCON2, INTCON4, IFSx, IECx, IPCx, INTTREG),
 * driving the core (struct pic24_intc).
 *
 *   - Interrupt n is requested while its flag (IFS<n/16>, bit n%16) and
 *     its enable (IEC) are set and its priority (IPC<n/4>, three bits at
 *     4*(n%4)) is not 0, and INTCON2.GIE is set. The highest priority is
 *     presented to the core, then the lowest number (the natural order);
 *     the core takes it when it is above its own priority.
 *   - Taking it, INTTREG says which: VECNUM (the vector number, n + 8)
 *     and ILR (the new priority).
 *   - The core's traps set INTCON1's status bits (OSCFAIL, ADDRERR,
 *     STKERR, MATHERR) and INTCON4.SGHT for a hard trap; the program
 *     clears them in its handler.
 *   - INTCON1.NSTDIS: an interrupt runs at priority 7, so none nests.
 * A model raises its interrupt with pic24_irq; the program clears the
 * flag. Writing a flag sets it too, as on the part.
 *
 * Not modelled: the external interrupt pins (INTx and their edges),
 * the alternate vector table (AIVTEN), the software trap (SWTRAP). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

struct intc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *intcon1, *intcon2, *intcon4, *inttreg;
    struct rf_reg *ifs[16], *iec[16], *ipc[64];
    int nw;
    struct pic24_intc ic;
};

static struct intc *intc_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "INT") : 0;
    return p && p->ops == &pic24_intc_ops ? p->ctx : 0;
}

static int prio(const struct intc *c, int irq)
{
    const struct rf_reg *r = c->ipc[irq / 4];
    return r ? (int)(r->value >> (4 * (irq % 4)) & 7) : 0;
}

static int request(void *ctx, int *irq)
{
    struct intc *c = ctx;
    if (c->intcon2 && !(c->intcon2->value & 0x8000))
        return 0;
    int best = 0;
    for (int w = 0; w < c->nw; w++) {
        u32 pend = (u32)(c->ifs[w]->value & c->iec[w]->value) & 0xffff;
        while (pend) {
            int bit = 0;
            while (!(pend >> bit & 1))
                bit++;
            pend &= pend - 1;
            int n = 16 * w + bit, pr = prio(c, n);
            if (pr > best) {
                best = pr;
                *irq = n;
            }
        }
    }
    return best;
}

static void taken(void *ctx, int irq, int ipl)
{
    struct intc *c = ctx;
    if (c->inttreg)
        rf_hw(c->inttreg, (u64)((u32)(ipl & 15) << 8 | (u32)((irq + 8) & 0xff)));
}

static void trap(void *ctx, int n)
{
    static const u16 bit[8] = { 0x0002, 0x0008, 0, 0x0004, 0x0010, 0, 0, 0 };
    struct intc *c = ctx;
    if (bit[n] && c->intcon1)
        rf_hw(c->intcon1, c->intcon1->value | bit[n]);
    if (n == TRAP_HARD && c->intcon4)
        rf_hw(c->intcon4, c->intcon4->value | 1);
}

static int nstdis(void *ctx)
{
    struct intc *c = ctx;
    return c->intcon1 && (c->intcon1->value & 0x8000);
}

static u32 intc_read(void *ctx, u32 off, int n)
{
    struct intc *c = ctx;
    return rf_read(c->p, off, n);
}

static void intc_write(void *ctx, u32 off, int n, u32 v)
{
    struct intc *c = ctx;
    rf_write(c->p, off, n, v);
}

static void intc_reset(void *ctx)
{
    (void)ctx;
}

const struct dev_ops pic24_intc_ops = {
    "pic24-intc", intc_read, intc_write, intc_reset, 0, 0,
};

void pic24_irq(struct sim *s, int irq)
{
    struct intc *c = intc_of(s);
    if (!c || irq < 0 || irq / 16 >= c->nw)
        return;
    struct rf_reg *r = c->ifs[irq / 16];
    if (!(r->value >> (irq % 16) & 1))
        rf_hw(r, r->value | 1u << (irq % 16));
}

int pic24_irq_flag(struct sim *s, int irq)
{
    struct intc *c = intc_of(s);
    if (!c || irq < 0 || irq / 16 >= c->nw)
        return 0;
    return (int)(c->ifs[irq / 16]->value >> (irq % 16) & 1);
}

int pic24_irq_enabled(struct sim *s, int irq)
{
    struct intc *c = intc_of(s);
    if (!c || irq < 0 || irq / 16 >= c->nw)
        return 0;
    return (int)(c->iec[irq / 16]->value >> (irq % 16) & 1);
}

int pic24_irq_named(struct sim *s, const char *name)
{
    return s->svd ? rf_dev_irq(s->svd, name) : -1;
}

void *pic24_intc_create(struct sim *s, struct rf_periph *p)
{
    if (!pic24_is(s->cpu))
        die("the PIC24 interrupt controller is for the PIC24 core");
    struct intc *c = calloc(1, sizeof *c);
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    c->intcon1 = rf_reg(p, "INTCON1");
    c->intcon2 = rf_reg(p, "INTCON2");
    c->intcon4 = rf_reg(p, "INTCON4");
    c->inttreg = rf_reg(p, "INTTREG");
    char nm[16];
    for (int w = 0; w < 16; w++) {
        snprintf(nm, sizeof nm, "IFS%d", w);
        c->ifs[w] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "IEC%d", w);
        c->iec[w] = rf_reg(p, nm);
        if (!c->ifs[w] || !c->iec[w])
            break;
        c->nw = w + 1;
    }
    for (int k = 0; k < 64; k++) {
        snprintf(nm, sizeof nm, "IPC%d", k);
        c->ipc[k] = rf_reg(p, nm);
    }
    if (!c->nw)
        die("the PIC24's INT peripheral has no IFS0 and IEC0");
    c->ic.request = request;
    c->ic.taken = taken;
    c->ic.trap = trap;
    c->ic.nstdis = nstdis;
    c->ic.ctx = c;
    pic24_intc_attach(s, &c->ic);
    return c;
}
