/* pic24e-spi.c -- a dsPIC33E's or PIC24E's SPI units in master mode, over
 * the EDC's registers (SPIxSTAT, SPIxCON1, SPIxCON2, SPIxBUF: one
 * peripheral, SPI), by the family reference manual's SPI section.
 *
 *   - With SPIxSTAT.SPIEN and SPIxCON1.MSTEN, a byte written to SPIxBUF
 *     (MODE16: a word) is exchanged at once with what the board has on
 *     the unit's pins (struct board_io's spi, a byte at a time, the most
 *     significant first; 0xFF a byte where nothing answers); what came
 *     back is the next read of SPIxBUF: SPIRBF says it is there, reading
 *     SPIxBUF clears it, and a word received with SPIRBF still set sets
 *     SPIROV (the word is lost). The transmitter is always empty again
 *     (SPITBF clear, SRMPT set), and the unit's interrupt (SPIx) is
 *     flagged with each word.
 * Not modelled: the transfer's time (the prescalers), slave and framed
 * modes, the enhanced buffer (SPIBEN), the error interrupt. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NUNIT 4

struct unit {
    struct rf_reg *stat, *con1, *buf;
    int irq;
    u16 rx;                         /* the word received */
};

struct spi {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NUNIT];
};

#define STAT_SPIEN  (1u << 15)
#define STAT_SRMPT  (1u << 7)
#define STAT_SPIROV (1u << 6)
#define STAT_SPITBF (1u << 1)
#define STAT_SPIRBF 1u
#define CON1_MODE16 (1u << 10)
#define CON1_MSTEN  (1u << 5)

static u32 spi_read(void *ctx, u32 off, int n)
{
    struct spi *c = ctx;
    for (int k = 0; k < NUNIT && !c->s->bus.debug; k++) {
        struct unit *q = &c->u[k];
        if (q->buf && (off & ~1u) == rf_off(q->buf)) {
            rf_hw(q->buf, q->rx);
            rf_hw(q->stat, q->stat->value & ~(u64)STAT_SPIRBF);
        }
    }
    return rf_read(c->p, off, n);
}

static void spi_write(void *ctx, u32 off, int n, u32 v)
{
    struct spi *c = ctx;
    for (int k = 0; k < NUNIT; k++) {
        struct unit *q = &c->u[k];
        if (!q->buf || (off & ~1u) != rf_off(q->buf))
            continue;
        if (!(q->stat->value & STAT_SPIEN) || !(q->con1->value & CON1_MSTEN))
            return;
        struct board_io *io = &c->s->io;
        int bytes = (q->con1->value & CON1_MODE16) ? 2 : 1;
        u32 in = 0;
        for (int b = bytes - 1; b >= 0; b--) {
            int r = io->spi ? io->spi(io->ctx, k + 1, (int)(v >> (8 * b) & 0xff)) : -1;
            in = in << 8 | (u32)(r < 0 ? 0xff : r & 0xff);
        }
        u64 st = q->stat->value | STAT_SRMPT;
        if (st & STAT_SPIRBF)
            st |= STAT_SPIROV;              /* the word received is lost */
        else
            q->rx = (u16)in;
        rf_hw(q->stat, (st | STAT_SPIRBF) & ~(u64)STAT_SPITBF);
        if (q->irq >= 0)
            pic24_irq(c->s, q->irq);
        return;
    }
    rf_write(c->p, off, n, v);
}

static void spi_reset(void *ctx)
{
    struct spi *c = ctx;
    for (int k = 0; k < NUNIT; k++)
        c->u[k].rx = 0;
}

const struct dev_ops pic24e_spi_ops = {
    "pic24e-spi", spi_read, spi_write, spi_reset, 0, 0,
};

void *pic24e_spi_create(struct sim *s, struct rf_periph *p)
{
    struct spi *c = calloc(1, sizeof *c);
    char nm[24];        /* "SPI" or "U", any int, the longest suffix: GCC counts it */
    int any = 0;
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    for (int k = 0; k < NUNIT; k++) {
        struct unit *q = &c->u[k];
        snprintf(nm, sizeof nm, "SPI%dSTAT", k + 1);
        q->stat = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "SPI%dCON1", k + 1);
        q->con1 = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "SPI%dBUF", k + 1);
        q->buf = rf_reg(p, nm);
        if (!q->stat || !q->con1 || !q->buf) {
            q->buf = 0;
            continue;
        }
        any = 1;
        rf_set_reset(q->stat, STAT_SRMPT);
        rf_set_acc(q->stat, STAT_SPIRBF | STAT_SPITBF | STAT_SRMPT, ACC_RO);
        snprintf(nm, sizeof nm, "SPI%d", k + 1);
        q->irq = pic24_irq_named(s, nm);
    }
    if (!any) {
        free(c);
        return 0;
    }
    return c;
}
