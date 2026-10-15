/* pic24-spi.c -- a PIC24FJ's SPI1-SPI3 in master mode, over the EDC's
 * registers (SPIxCON1L, SPIxSTATL, SPIxBUFL...: one peripheral, SPI).
 *
 *   - With SPIEN and MSTEN, a byte written to SPIxBUFL (MODE16: a word)
 *     is exchanged at once with what the board has on the unit's pins
 *     (struct board_io's spi: the slave whose clock and data lines the
 *     Peripheral Pin Select connects), and what came back is the next
 *     read of SPIxBUFL: SPIRBE clears until it is read, SPIRBF sets once
 *     the receive FIFO (one deep, or 16 with ENHBUF) is full, and a byte
 *     arriving at a full one sets SPIROV. The transmit side is always
 *     empty again (SPITBE set, SPITBF clear, SRMT set).
 * Not modelled: the transfer's time (SPIxBRG's baud rate), slave mode,
 * framed and audio modes, MODE32, the interrupts. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NUNIT 3
#define FIFO 16

struct unit {
    struct rf_reg *con1, *stat, *bufl, *bufh;
    u16 rx[FIFO];
    int head, n;
};

struct spi {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NUNIT];
};

#define STAT_SPIRBF (1u << 0)
#define STAT_SPITBF (1u << 1)
#define STAT_SPITBE (1u << 3)
#define STAT_SPIRBE (1u << 5)
#define STAT_SPIROV (1u << 6)
#define STAT_SRMT   (1u << 7)

static int depth(const struct unit *q)
{
    return (q->con1->value & 1) ? FIFO : 1;     /* ENHBUF */
}

static void status(struct unit *q)
{
    u64 st = q->stat->value & ~(u64)(STAT_SPIRBF | STAT_SPITBF | STAT_SPIRBE);
    st |= STAT_SPITBE | STAT_SRMT;
    if (!q->n)
        st |= STAT_SPIRBE;
    if (q->n >= depth(q))
        st |= STAT_SPIRBF;
    rf_hw(q->stat, st);
}

/* the unit whose SPIxBUFL the access touches, or -1 */
static int buf_unit(const struct spi *c, u32 off)
{
    for (int k = 0; k < NUNIT; k++) {
        const struct rf_reg *r = c->u[k].bufl;
        if (r && off >= rf_off(r) && off < rf_off(r) + 2)
            return k;
    }
    return -1;
}

static u32 spi_read(void *ctx, u32 off, int n)
{
    struct spi *c = ctx;
    int k = buf_unit(c, off);
    if (k >= 0 && !c->s->bus.debug) {
        struct unit *q = &c->u[k];
        u16 v = q->n ? q->rx[q->head] : (u16)q->bufl->value;
        if (q->n) {
            q->head = (q->head + 1) % FIFO;
            q->n--;
        }
        rf_hw(q->bufl, v);
        status(q);
    }
    return rf_read(c->p, off, n);
}

static void spi_write(void *ctx, u32 off, int n, u32 v)
{
    struct spi *c = ctx;
    rf_write(c->p, off, n, v);
    int k = buf_unit(c, off);
    if (k < 0)
        return;
    struct unit *q = &c->u[k];
    u64 con = q->con1->value;
    if (!(con & 0x8000) || !(con & 0x20))       /* SPIEN, MSTEN */
        return;
    struct board_io *io = &c->s->io;
    int wide = (con & 0x0400) != 0;             /* MODE16 */
    u32 out = (u32)q->bufl->value & (wide ? 0xffff : 0xff), in;
    if (wide) {
        int hi = io->spi ? io->spi(io->ctx, k + 1, (int)(out >> 8)) : -1;
        int lo = io->spi ? io->spi(io->ctx, k + 1, (int)(out & 0xff)) : -1;
        in = (u32)(hi < 0 ? 0xff : hi) << 8 | (u32)(lo < 0 ? 0xff : lo);
    } else {
        int b = io->spi ? io->spi(io->ctx, k + 1, (int)out) : -1;
        in = b < 0 ? 0xff : (u32)b;
    }
    if (q->n >= depth(q))
        rf_hw(q->stat, q->stat->value | STAT_SPIROV);
    else
        q->rx[(q->head + q->n++) % FIFO] = (u16)in;
    status(q);
}

static void spi_reset(void *ctx)
{
    struct spi *c = ctx;
    for (int k = 0; k < NUNIT; k++) {
        c->u[k].head = c->u[k].n = 0;
        if (c->u[k].stat)
            status(&c->u[k]);
    }
}

const struct dev_ops pic24_spi_ops = {
    "pic24-spi", spi_read, spi_write, spi_reset, 0, 0,
};

void *pic24_spi_create(struct sim *s, struct rf_periph *p)
{
    struct spi *c = calloc(1, sizeof *c);
    char nm[24];
    int any = 0;
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    for (int k = 0; k < NUNIT; k++) {
        struct unit *q = &c->u[k];
#define REG(f, name) snprintf(nm, sizeof nm, "SPI%d" name, k + 1); q->f = rf_reg(p, nm)
        REG(con1, "CON1L");
        REG(stat, "STATL");
        REG(bufl, "BUFL");
        REG(bufh, "BUFH");
#undef REG
        if (!q->con1 || !q->stat || !q->bufl) {
            memset(q, 0, sizeof *q);
            continue;
        }
        any = 1;
        rf_set_acc(q->stat, STAT_SPIRBF | STAT_SPITBF | STAT_SPITBE | STAT_SPIRBE |
                            STAT_SRMT, ACC_RO);
        status(q);
        q->stat->reset = q->stat->value;
    }
    if (!any) {
        free(c);
        return 0;
    }
    return c;
}
