/* pic24-epmp.c -- a PIC24FJ's Enhanced Parallel Master Port in its
 * master mode (PMCON1.MODE 11), over the EDC's registers.
 *
 * In master mode the EPMP's external space is the Extended Data Space
 * above the RAM: the core reads and writes it through the EDS window
 * (DSRPAG and DSWPAG pages 1 to 0x1FF), and this model is that part of
 * the bus (from EDS address 0x008800 up). Chip select 1's region starts
 * at PMCS1BS's base address (BASE<23:15> and BASE11), chip select 2's at
 * PMCS2BS's; CS1's ends where CS2's starts (or at the top when PMCS2BS
 * is 0), CS2's at PMCON2.RADDR<<16 (or the top). An access in an enabled
 * region (PMCON1.PMPEN, PMCSxCF.CSDIS clear) is one bus cycle to the
 * board (struct board_io's bus_write and bus_read): the address on the
 * PMA lines -- the EDS address's word offset, as the EDS is
 * word-addressed, on the lines PMCON4 enables -- and the data, the low 8
 * bits on an 8-bit port (PTSZ 00, a word access a single cycle) or 16
 * bits. Microchip's own Explorer 16/32 LCD driver depends on this: its
 * command register at the region's base and its data register two bytes
 * up, PMA0 the LCD's RS. With PMCON1.IRQM 01 the PMP interrupt is
 * flagged at the end of each cycle.
 *
 * Not modelled: the slave modes (PMDOUT, PMDIN, PMSTAT keep what is
 * written), address/data multiplexing, the wait states' time (the cycle
 * takes none of the core's), PMACK. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

/* the EDS space the EPMP answers on the bus: above the RAM */
#define EPMP_LO 0x8800u
#define EPMP_HI 0x1000000u

struct epmp {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con1, *con2, *con4, *cf[2], *bs[2];
    int irq;
};

static u32 base_of(const struct rf_reg *bs)
{
    u32 v = bs ? (u32)bs->value : 0;
    return (v >> 8 & 0xff) << 16 | (v >> 7 & 1) << 15 | (v >> 3 & 1) << 11;
}

/* the chip select whose region holds EDS address a: 0 or 1, or -1 */
static int region(const struct epmp *e, u32 a)
{
    u32 b1 = base_of(e->bs[0]), b2 = base_of(e->bs[1]);
    u32 end2 = e->con2 && (e->con2->value & 0xff) ? (u32)(e->con2->value & 0xff) << 16
                                                  : EPMP_HI;
    int cs2_off = !e->bs[1] || e->bs[1]->value == 0;
    u32 end1 = cs2_off || b2 <= b1 ? EPMP_HI : b2;
    if (a >= b1 && a < end1)
        return 0;
    if (!cs2_off && a >= b2 && a < end2)
        return 1;
    return -1;
}

static int cycle(struct epmp *e, u32 a, int *cs, u32 *pma, int *bits)
{
    if (!(e->con1->value & 0x8000) || (e->con1->value >> 8 & 3) != 3)
        return 0;                               /* off, or a slave */
    *cs = region(e, a);
    if (*cs < 0 || !e->cf[*cs] || (e->cf[*cs]->value & 0x8000))
        return 0;
    u32 en = e->con4 ? (u32)e->con4->value & 0xffff : 0xffff;
    *pma = (a >> 1) & (0x7f0000u | en);
    u32 sz = (u32)(e->cf[*cs]->value >> 5 & 3);
    *bits = sz == 2 ? 16 : sz == 1 ? 4 : 8;
    return 1;
}

static void done(struct epmp *e)
{
    if ((e->con1->value & 3) == 1)
        pic24_irq(e->s, e->irq);
}

static u32 eds_read(void *ctx, u32 off, int n)
{
    struct epmp *e = ctx;
    struct board_io *io = &e->s->io;
    int cs, bits;
    u32 pma, a = EPMP_LO + off;
    (void)n;
    if (e->s->bus.debug || !cycle(e, a, &cs, &pma, &bits))
        return 0;
    u32 v = io->bus_read ? io->bus_read(io->ctx, pma) : 0;
    done(e);
    return v & ((1u << bits) - 1);
}

static void eds_write(void *ctx, u32 off, int n, u32 v)
{
    struct epmp *e = ctx;
    struct board_io *io = &e->s->io;
    int cs, bits;
    u32 pma, a = EPMP_LO + off;
    (void)n;
    if (e->s->bus.debug || !cycle(e, a, &cs, &pma, &bits))
        return;
    if (io->bus_write)
        io->bus_write(io->ctx, pma, v & ((1u << bits) - 1));
    done(e);
}

static const struct dev_ops eds_ops = {
    "pic24-epmp-eds", eds_read, eds_write, 0, 0, 0,
};

static u32 epmp_read(void *ctx, u32 off, int n)
{
    struct epmp *e = ctx;
    return rf_read(e->p, off, n);
}

static void epmp_write(void *ctx, u32 off, int n, u32 v)
{
    struct epmp *e = ctx;
    rf_write(e->p, off, n, v);
}

const struct dev_ops pic24_epmp_ops = {
    "pic24-epmp", epmp_read, epmp_write, 0, 0, 0,
};

void *pic24_epmp_create(struct sim *s, struct rf_periph *p)
{
    struct epmp *e = calloc(1, sizeof *e);
    if (!e)
        die("out of memory");
    e->s = s;
    e->p = p;
    e->con1 = rf_reg(p, "PMCON1");
    e->con2 = rf_reg(p, "PMCON2");
    e->con4 = rf_reg(p, "PMCON4");
    e->cf[0] = rf_reg(p, "PMCS1CF");
    e->cf[1] = rf_reg(p, "PMCS2CF");
    e->bs[0] = rf_reg(p, "PMCS1BS");
    e->bs[1] = rf_reg(p, "PMCS2BS");
    if (!e->con1 || !e->cf[0] || !e->bs[0]) {
        free(e);
        return 0;
    }
    /* the base addresses at power-on, the data sheet's (PMCSxBS) */
    rf_set_reset(e->bs[0], 0x0080);
    if (e->bs[1])
        rf_set_reset(e->bs[1], 0x8080);
    e->irq = pic24_irq_named(s, "PMP");
    bus_add_device(&s->bus, PIC24_DATA + EPMP_LO, EPMP_HI - EPMP_LO, &eds_ops, e);
    return e;
}
