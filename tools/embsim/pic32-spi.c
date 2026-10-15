/* pic32-spi.c -- a PIC32's SPI units as masters, over the device file's
 * registers (SPIxCON, SPIxSTAT, SPIxBUF, SPIxBRG).
 *
 *   - With SPIxCON.ON and MSTEN, a word written to SPIxBUF is exchanged at
 *     once with what the board has on the unit's pins (struct board_io's
 *     spi, a byte at a time, the most significant first: one byte, two
 *     with MODE16, four with MODE32; 0xFF a byte where nothing answers);
 *     what came back is read from SPIxBUF, SPIRBF saying it is there --
 *     reading it clears SPIRBF, and a word received with SPIRBF still set
 *     sets SPIROV -- and the receive interrupt (SPIn_RX) is flagged. The
 *     transmitter is always empty (SPITBE set, SPITBF and SPIBUSY clear).
 * Not modelled: the slave mode, the enhanced buffer (ENHBUF), framed
 * modes, the bit rate (SPIxBRG: a word takes no time), the transmit and
 * error interrupts. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

#define CON_ON      (1u << 15)
#define CON_MODE32  (1u << 11)
#define CON_MODE16  (1u << 10)
#define CON_MSTEN   (1u << 5)
#define STAT_SPIROV (1u << 6)
#define STAT_SPITBE (1u << 3)
#define STAT_SPIRBF 1u

struct spi {
    struct sim *s;
    struct rf_periph *p;
    int n;
    struct rf_reg *con, *stat, *buf;
    int irq_rx;
    u32 rx;                         /* the word received */
};

static u32 spi_read(void *ctx, u32 off, int n)
{
    struct spi *u = ctx;
    if ((off & ~3u) == rf_off(u->buf)) {
        rf_hw(u->buf, u->rx);
        if (!u->s->bus.debug)
            rf_hw(u->stat, u->stat->value & ~(u64)STAT_SPIRBF);
    }
    return rf_read(u->p, off, n);
}

static void spi_write(void *ctx, u32 off, int n, u32 v)
{
    struct spi *u = ctx;
    if ((off & ~3u) != rf_off(u->buf)) {
        rf_write(u->p, off, n, v);
        return;
    }
    u32 con = (u32)u->con->value;
    if (!(con & CON_ON) || !(con & CON_MSTEN))
        return;
    struct board_io *io = &u->s->io;
    int bytes = con & CON_MODE32 ? 4 : con & CON_MODE16 ? 2 : 1;
    u32 in = 0;
    for (int k = bytes - 1; k >= 0; k--) {
        int b = io->spi ? io->spi(io->ctx, u->n, (int)(v >> (8 * k) & 0xff)) : -1;
        in = in << 8 | (u32)(b < 0 ? 0xff : b & 0xff);
    }
    u32 st = (u32)u->stat->value | STAT_SPITBE;
    if (st & STAT_SPIRBF)
        st |= STAT_SPIROV;
    u->rx = in;
    rf_hw(u->stat, st | STAT_SPIRBF);
    if (u->irq_rx >= 0)
        pic32_irq(u->s, u->irq_rx);
}

static void spi_reset(void *ctx)
{
    struct spi *u = ctx;
    u->rx = 0;
}

const struct dev_ops pic32_spi_ops = {
    "pic32-spi", spi_read, spi_write, spi_reset, 0, 0,
};

void *pic32_spi_create(struct sim *s, struct rf_periph *p)
{
    char nm[16];
    int n = 0;
    if (sscanf(p->name, "SPI%d", &n) != 1)
        return 0;
    struct spi *u = calloc(1, sizeof *u);
    if (!u)
        die("out of memory");
    u->s = s;
    u->p = p;
    u->n = n;
    snprintf(nm, sizeof nm, "SPI%dCON", n);
    u->con = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "SPI%dSTAT", n);
    u->stat = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "SPI%dBUF", n);
    u->buf = rf_reg(p, nm);
    if (!u->con || !u->stat || !u->buf) {
        free(u);
        return 0;
    }
    rf_set_reset(u->stat, STAT_SPITBE);
    rf_set_acc(u->stat, STAT_SPIRBF | STAT_SPITBE, ACC_RO);
    snprintf(nm, sizeof nm, "SPI%d_RX", n);
    u->irq_rx = pic32_irq_named(s, nm);
    return u;
}
