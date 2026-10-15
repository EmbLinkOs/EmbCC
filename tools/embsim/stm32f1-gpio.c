/* stm32f1-gpio.c -- an STM32F1 GPIO port (CRL, CRH, IDR, ODR, BSRR, BRR,
 * LCKR: RM0008's "General-purpose and alternate-function I/Os"), over
 * the SVD's registers.
 *
 *   - BSRR's low half sets ODR's bits and its high half resets them (set
 *     wins); BRR resets them;
 *   - IDR is what each pin reads, from its configuration (CNFy, MODEy):
 *       input, analog (CNF 00): 0, its Schmitt trigger off;
 *       input, floating (01, out of reset): what the board drives, else 0
 *         (floating, EmbSim's choice);
 *       input, pull-up/pull-down (10): what the board drives, else the
 *         pull ODR's bit picks (1 up, 0 down);
 *       general-purpose output, push-pull (00): ODR's bit; open-drain
 *         (01): 0 when ODR's bit is 0, else let go (as floating);
 *       alternate-function output (10, 11): a USART's TX pin, the line
 *         idle, reads 1 (USART1 PA9, or PB6 remapped; USART2 PA2; USART3
 *         PB10, or PC10 partly remapped); any other, whose peripheral
 *         drives it, 0;
 *   - the debug port's pins, while AFIO's MAPR.SWJ_CFG keeps them (out of
 *     reset all five), are its, whatever CRL and CRH say: PA13 (JTMS)
 *     and PA15 (JTDI) pulled up, PA14 (JTCK) pulled down, PB4 (JNTRST)
 *     pulled up, PB3 (JTDO) 0. SWJ_CFG 001 gives PB4 back, 010 PA15, PB3
 *     and PB4, 100 all five;
 *   - each pin's level as it changes goes to the board (struct board_io:
 *     port 0 is A), and the board's drive comes back into IDR;
 *   - LCKR is a plain register: the lock sequence is not modelled.
 * IDR's and ODR's changes show in --trace-periph as the hardware's. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

struct f1gpio {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *crl, *crh, *idr, *odr, *bsrr, *brr;
    int port;                       /* 0 for A */
    int out[16];                    /* what each pin was told to be: driven
                                       0 or 1, or -1 let go */
    int known;
};

static u32 cfg(const struct f1gpio *g, int pin)
{
    const struct rf_reg *r = pin < 8 ? g->crl : g->crh;
    return (u32)(r->value >> 4 * (pin & 7)) & 15;     /* CNF:MODE */
}

/* AFIO's MAPR, for the debug port and the USARTs' remaps */
static u32 mapr(struct sim *s)
{
    struct rf_periph *a = s->svd ? rf_periph(s->svd, "AFIO") : 0;
    struct rf_reg *m = a ? rf_reg(a, "MAPR") : 0;
    return m ? (u32)m->value : 0;
}

/* -1, or the level the debug port gives a pin it holds */
static int debug_pin(const struct f1gpio *g, int pin, u32 mp)
{
    u32 swj = mp >> 24 & 7;
    if (g->port == 0 && pin >= 13 && pin <= 15) {
        if (swj == 4 || (swj == 2 && pin == 15))
            return -1;
        return pin == 14 ? 0 : 1;
    }
    if (g->port == 1 && (pin == 3 || pin == 4)) {
        if (swj == 4 || swj == 2 || (swj == 1 && pin == 4))
            return -1;
        return pin == 4 ? 1 : 0;
    }
    return -1;
}

/* a USART's TX pin, as AFIO's MAPR places it */
static int usart_tx(const struct f1gpio *g, int pin, u32 mp)
{
    int u1 = mp >> 2 & 1, u3 = mp >> 4 & 3;
    return (g->port == 0 && pin == 9 && !u1) || (g->port == 1 && pin == 6 && u1) ||
           (g->port == 0 && pin == 2 && !(mp >> 3 & 1)) ||
           (g->port == 1 && pin == 10 && u3 == 0) || (g->port == 2 && pin == 10 && u3 == 1);
}

static void update(struct f1gpio *g)
{
    struct sim *s = g->s;
    u32 mp = mapr(s), idr = 0, odr = (u32)g->odr->value;
    for (int i = 0; i < 16; i++) {
        u32 c = cfg(g, i), mode = c & 3, cnf = c >> 2;
        int lvl, ext, driven = -1;  /* driven: the pin's output, or -1 */
        int dbg = debug_pin(g, i, mp);
        if (dbg >= 0)
            lvl = dbg;
        else {
            if (mode) {
                if (cnf & 2)
                    driven = usart_tx(g, i, mp);
                else if (!(cnf & 1) || !(odr >> i & 1))
                    driven = (int)(odr >> i & 1);
            }
            if (driven >= 0)
                lvl = driven;
            else if (!mode && cnf == 0)
                lvl = 0;            /* analog */
            else if (s->io.pin_in && s->io.pin_in(s->io.ctx, g->port, i, &ext))
                lvl = ext != 0;
            else if (!mode && cnf == 2)
                lvl = (int)(odr >> i & 1);
            else
                lvl = 0;
        }
        if (!(!mode && cnf == 0))
            idr |= (u32)lvl << i;
        int o = dbg >= 0 ? -1 : driven;
        if (s->io.pin_out && (!g->known || g->out[i] != o))
            s->io.pin_out(s->io.ctx, g->port, i, o);
        g->out[i] = o;
    }
    g->known = 1;
    if ((g->idr->value & 0xffff) != idr)
        rf_hw(g->idr, (g->idr->value & ~0xffffULL) | idr);
}

static u32 f1gpio_read(void *ctx, u32 off, int n)
{
    struct f1gpio *g = ctx;
    update(g);
    return rf_read(g->p, off, n);
}

static void f1gpio_write(void *ctx, u32 off, int n, u32 v)
{
    struct f1gpio *g = ctx;
    rf_write(g->p, off, n, v);
    u64 odr = g->odr->value, nv = odr;
    if (off == rf_off(g->bsrr))
        nv = (odr & ~(u64)(v >> 16 & 0xffff)) | (v & 0xffff);
    else if (g->brr && off == rf_off(g->brr))
        nv = odr & ~(u64)(v & 0xffff);
    if (nv != odr)
        rf_hw(g->odr, nv);
    update(g);
}

static void f1gpio_reset(void *ctx)
{
    struct f1gpio *g = ctx;
    g->known = 0;
    update(g);
}

const struct dev_ops stm32f1_gpio_ops = {
    "stm32f1-gpio", f1gpio_read, f1gpio_write, f1gpio_reset, 0, 0,
};

/* the board drove a pin or let it go: every port again */
void stm32f1_pins_changed(struct sim *s)
{
    for (int i = 0; s->svd; i++) {
        struct rf_periph *p = rf_periph_at(s->svd, i);
        if (!p)
            break;
        if (p->ops == &stm32f1_gpio_ops)
            update(p->ctx);
    }
}

void *stm32f1_gpio_create(struct sim *s, struct rf_periph *p)
{
    struct f1gpio g;
    memset(&g, 0, sizeof g);
    g.s = s;
    g.p = p;
    g.crl = rf_reg(p, "CRL");
    g.crh = rf_reg(p, "CRH");
    g.idr = rf_reg(p, "IDR");
    g.odr = rf_reg(p, "ODR");
    g.bsrr = rf_reg(p, "BSRR");
    g.brr = rf_reg(p, "BRR");
    if (!g.crl || !g.crh || !g.idr || !g.odr || !g.bsrr || strncmp(p->name, "GPIO", 4) ||
        p->name[4] < 'A' || p->name[4] > 'K' || p->name[5])
        return 0;                   /* not this layout: the registers alone */
    g.port = p->name[4] - 'A';
    struct f1gpio *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = g;
    return c;
}
