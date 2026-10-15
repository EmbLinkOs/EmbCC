/* pic24-dma.c -- a PIC24FJ's DMA controller (the EDC's DMA: DMACON,
 * DMAH, DMAL, DMABUF, and each channel's DMACHn, DMAINTn, DMASRCn,
 * DMADSTn, DMACNTn), as the data sheet's section 5 has it.
 *
 *   - A channel with CHEN set (and DMACON.DMAEN) moves a byte or a word
 *     (SIZE) from DMASRCn to DMADSTn in the data space -- RAM or an SFR, so
 *     a peripheral sees the access -- each time its trigger comes: the
 *     interrupt event CHSEL names (Table 5-1: the timers, UARTs, SPIs,
 *     I2Cs, the CCPs, ICs, OCs, the ADC, the RTCC's alarm, the IOC, the
 *     other channels...), or CHREQ written 1 (cleared when the transfer is
 *     done);
 *   - SAMODE and DAMODE: the address kept, incremented or decremented by
 *     the size after each transaction; TRMODE: one transaction a trigger
 *     (one-shot) or DMACNTn of them (continuous), the repeated modes
 *     reloading the addresses and the count when the count runs out and
 *     staying on, the others leaving CHEN cleared (RELOAD reloads them
 *     too);
 *   - DMACNTn counts the transactions down: at 0 DONEIF and the channel's
 *     interrupt (DMAn), at half way HALFIF, and the interrupt too with
 *     HALFEN;
 *   - an access to RAM (0x0800 up) below DMAL or above DMAH stops the
 *     channel (CHEN cleared) with LOWIF or HIGHIF and its interrupt.
 * A transfer takes no time, so none is ever overrun (OVRUNIF). DMAL and
 * DMAH are 0 out of reset: a transfer to or from RAM needs them set, as
 * the data sheet's setup does. Not modelled: the peripheral indirect
 * addressing mode (SAMODE or DAMODE 11: the address is kept), the null
 * write mode, the priority schemes (channels serve their triggers in
 * number order). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NCH 8

/* DMACHn */
#define CH_CHEN     1u
#define CH_SIZE     2u
#define CH_RELOAD   (1u << 9)
#define CH_CHREQ    (1u << 8)
/* DMAINTn */
#define INT_HIGHIF  (1u << 7)
#define INT_LOWIF   (1u << 6)
#define INT_DONEIF  (1u << 5)
#define INT_HALFIF  (1u << 4)
#define INT_HALFEN  1u

struct ch {
    struct rf_reg *ch, *in, *src, *dst, *cnt;
    int irq;                        /* DMAn */
    u32 src0, dst0, cnt0;           /* for the reloads */
    int started;                    /* a transfer under way since the last reload */
};

struct dma {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con, *buf, *l, *h;
    struct ch c[NCH];
    int nch;
    int sel_irq[128];               /* CHSEL's trigger, as the EDC numbers it */
    int busy;
};

/* Table 5-1: CHSEL's triggers, by the EDC's interrupt names */
static const struct { u8 sel; const char *name; } triggers[] = {
    { 0x01, "CCP7" }, { 0x02, "CCT7" }, { 0x03, "CCP6" }, { 0x04, "CCT6" },
    { 0x05, "CCP5" }, { 0x06, "CCT5" }, { 0x07, "CCP4" }, { 0x08, "CCT4" },
    { 0x0b, "CCP3" }, { 0x0c, "CCT3" }, { 0x0d, "CCP2" }, { 0x0e, "CCT2" },
    { 0x0f, "CCP1" }, { 0x10, "CCT1" }, { 0x11, "OC6" }, { 0x12, "OC5" },
    { 0x13, "OC4" }, { 0x14, "OC3" }, { 0x15, "OC2" }, { 0x16, "OC1" },
    { 0x17, "IC6" }, { 0x18, "IC5" }, { 0x19, "IC4" }, { 0x1a, "IC3" },
    { 0x1b, "IC2" }, { 0x1c, "IC1" }, { 0x20, "SPI3RX" }, { 0x21, "SPI3TX" },
    { 0x22, "SPI3" }, { 0x23, "SPI2RX" }, { 0x24, "SPI2TX" }, { 0x25, "SPI2" },
    { 0x26, "SPI1RX" }, { 0x27, "SPI1TX" }, { 0x28, "SPI1" }, { 0x2c, "SI2C3" },
    { 0x2d, "MI2C3" }, { 0x2e, "I2C3BC" }, { 0x2f, "SI2C2" }, { 0x30, "MI2C2" },
    { 0x31, "I2C2BC" }, { 0x32, "SI2C1" }, { 0x33, "MI2C1" }, { 0x34, "I2C1BC" },
    { 0x35, "U6TX" }, { 0x36, "U6RX" }, { 0x37, "U6Err" }, { 0x38, "U5TX" },
    { 0x39, "U5RX" }, { 0x3a, "U5Err" }, { 0x3b, "U4TX" }, { 0x3c, "U4RX" },
    { 0x3d, "U4Err" }, { 0x3e, "U3TX" }, { 0x3f, "U3RX" }, { 0x40, "U3Err" },
    { 0x41, "U2TX" }, { 0x42, "U2RX" }, { 0x43, "U2Err" }, { 0x44, "U1TX" },
    { 0x45, "U1RX" }, { 0x46, "U1Err" }, { 0x49, "DMA7" }, { 0x4a, "DMA6" },
    { 0x4b, "DMA5" }, { 0x4c, "DMA4" }, { 0x4d, "DMA3" }, { 0x4e, "DMA2" },
    { 0x4f, "DMA1" }, { 0x50, "DMA0" }, { 0x51, "ADC1" }, { 0x52, "USB1" },
    { 0x53, "PMP" }, { 0x54, "LVD" }, { 0x55, "CRC" }, { 0x59, "CLC4" },
    { 0x5a, "CLC3" }, { 0x5b, "CLC2" }, { 0x5c, "CLC1" }, { 0x5e, "RTCC" },
    { 0x5f, "T5" }, { 0x60, "T4" }, { 0x61, "T3" }, { 0x62, "T2" },
    { 0x63, "T1" }, { 0x66, "CTMU" }, { 0x67, "Comp" }, { 0x68, "INT4" },
    { 0x69, "INT3" }, { 0x6a, "INT2" }, { 0x6b, "INT1" }, { 0x6c, "INT0" },
    { 0x6d, "IOC" },
};

static void flag(struct dma *m, struct ch *c, u32 bits, int irq)
{
    rf_hw(c->in, c->in->value | bits);
    if (irq && c->irq >= 0)
        pic24_irq(m->s, c->irq);
}

static void reload(struct ch *c)
{
    rf_hw(c->src, c->src0);
    rf_hw(c->dst, c->dst0);
    rf_hw(c->cnt, c->cnt0);
}

/* the address after a transaction: SAMODE or DAMODE */
static u32 step(u32 a, u32 mode, u32 size)
{
    if (mode == 1)
        return (a + size) & 0xffff;
    if (mode == 2)
        return (a - size) & 0xffff;
    return a;
}

/* RAM outside DMAL..DMAH: the bit to flag, or 0 */
static u32 out_of_limits(const struct dma *m, u32 a)
{
    if (a < 0x800)
        return 0;
    if (a < (u32)m->l->value)
        return INT_LOWIF;
    if (a > (u32)m->h->value)
        return INT_HIGHIF;
    return 0;
}

/* one transaction: 0, or 1 when the channel stopped */
static int transaction(struct dma *m, struct ch *c)
{
    u32 chv = (u32)c->ch->value, size = (chv & CH_SIZE) ? 1 : 2;
    u32 sa = (u32)c->src->value, da = (u32)c->dst->value, v = 0, lim;
    struct bus *b = &m->s->bus;
    if ((lim = out_of_limits(m, sa)) || (lim = out_of_limits(m, da))) {
        rf_hw(c->ch, chv & ~(CH_CHEN | CH_CHREQ));
        flag(m, c, lim, 1);
        return 1;
    }
    if (bus_read(b, PIC24_DATA + (size == 2 ? sa & ~1u : sa), (int)size, &v))
        v = 0;
    rf_hw(m->buf, v);
    bus_write(b, PIC24_DATA + (size == 2 ? da & ~1u : da), (int)size, v);
    rf_hw(c->src, step(sa, chv >> 6 & 3, size));
    rf_hw(c->dst, step(da, chv >> 4 & 3, size));
    u32 n = ((u32)c->cnt->value - 1) & 0xffff;
    rf_hw(c->cnt, n);
    if (c->cnt0 >= 2 && n == c->cnt0 / 2)
        flag(m, c, INT_HALFIF, (u32)c->in->value & INT_HALFEN);
    if (n)
        return 0;
    /* the count ran out: done */
    u32 tr = chv >> 2 & 3;
    flag(m, c, INT_DONEIF, 1);
    c->started = 0;
    if (tr & 1) {                               /* repeated: on again */
        reload(c);
    } else {
        if (chv & CH_RELOAD)
            reload(c);
        rf_hw(c->ch, (u32)c->ch->value & ~(CH_CHEN | CH_CHREQ));
    }
    return 1;
}

static void trigger(struct dma *m, struct ch *c)
{
    u32 chv = (u32)c->ch->value;
    if (!(m->con->value >> 15 & 1) || !(chv & CH_CHEN))
        return;
    if (!c->started) {
        c->started = 1;
        c->src0 = (u32)c->src->value;
        c->dst0 = (u32)c->dst->value;
        c->cnt0 = (u32)c->cnt->value;
    }
    if (chv >> 3 & 1) {                         /* continuous: the whole count */
        while (!transaction(m, c))
            ;
    } else {
        transaction(m, c);
    }
    rf_hw(c->ch, (u32)c->ch->value & ~CH_CHREQ);
}

static void on_irq(void *ctx, int irq)
{
    struct dma *m = ctx;
    if (m->busy > 4 || !(m->con->value >> 15 & 1))
        return;                     /* a channel triggering itself ad infinitum */
    m->busy++;
    for (int k = 0; k < m->nch; k++) {
        struct ch *c = &m->c[k];
        u32 sel = (u32)(c->in->value >> 8 & 0x7f);
        if (sel && m->sel_irq[sel] == irq)
            trigger(m, c);
    }
    m->busy--;
}

static u32 dma_read(void *ctx, u32 off, int n)
{
    struct dma *m = ctx;
    return rf_read(m->p, off, n);
}

static void dma_write(void *ctx, u32 off, int n, u32 v)
{
    struct dma *m = ctx;
    rf_write(m->p, off, n, v);
    for (int k = 0; k < m->nch; k++) {
        struct ch *c = &m->c[k];
        if (off == rf_off(c->ch) || off == rf_off(c->ch) + 1) {
            if (!(c->ch->value & CH_CHEN))
                c->started = 0;
            if (c->ch->value & CH_CHREQ) {      /* the software's trigger */
                m->busy++;
                trigger(m, c);
                m->busy--;
            }
        } else if (off == rf_off(c->src) || off == rf_off(c->dst) ||
                   off == rf_off(c->cnt)) {
            c->started = 0;                     /* a new transfer */
        }
    }
}

static void dma_reset(void *ctx)
{
    struct dma *m = ctx;
    for (int k = 0; k < m->nch; k++)
        m->c[k].started = 0;
    m->busy = 0;
    pic24_irq_listen(m->s, on_irq, m);
}

const struct dev_ops pic24_dma_ops = {
    "pic24-dma", dma_read, dma_write, dma_reset, 0, 0,
};

void *pic24_dma_create(struct sim *s, struct rf_periph *p)
{
    struct dma *m = calloc(1, sizeof *m);
    char nm[16];
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    m->con = rf_reg(p, "DMACON");
    m->buf = rf_reg(p, "DMABUF");
    m->l = rf_reg(p, "DMAL");
    m->h = rf_reg(p, "DMAH");
    if (!m->con || !m->buf || !m->l || !m->h) {
        free(m);
        return 0;
    }
    for (int k = 0; k < NCH; k++) {
        struct ch *c = &m->c[k];
#define R(f, name) snprintf(nm, sizeof nm, name "%d", k); c->f = rf_reg(p, nm)
        R(ch, "DMACH"); R(in, "DMAINT"); R(src, "DMASRC"); R(dst, "DMADST"); R(cnt, "DMACNT");
#undef R
        if (!c->ch || !c->in || !c->src || !c->dst || !c->cnt)
            break;
        snprintf(nm, sizeof nm, "DMA%d", k);
        c->irq = pic24_irq_named(s, nm);
        m->nch = k + 1;
    }
    for (int i = 0; i < 128; i++)
        m->sel_irq[i] = -1;
    for (size_t i = 0; i < sizeof triggers / sizeof triggers[0]; i++)
        m->sel_irq[triggers[i].sel] = pic24_irq_named(s, triggers[i].name);
    pic24_irq_listen(s, on_irq, m);
    return m;
}
