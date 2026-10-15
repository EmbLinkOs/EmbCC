/* pic32-pmp.c -- a PIC32's parallel master port, in its master modes
 * (PMMODE.MODE 2 and 3), over the ATDF's registers; and a dsPIC33E's or
 * PIC24E's, the same port over its EDC's (PMDIN1 the data register, its
 * wait states instruction cycles).
 *
 *   - With PMCON.ON set, a write to PMDIN is a write cycle on the bus:
 *     the board (struct board_io's bus_write) gets the address PMADDR
 *     drives on the PMA lines PMAEN enables, and the data, 8 bits (or 16
 *     with MODE16). A read of PMDIN gives what the previous read cycle
 *     latched and starts the next (bus_read), as on the part.
 *   - A cycle keeps PMMODE.BUSY set for its wait states, (WAITB + 1) +
 *     (WAITM + 1) + (WAITE + 1) clocks of peripheral bus 2; at its end
 *     the PMP interrupt is flagged when PMMODE.IRQM is 01, and PMADDR
 *     steps by one when INCM is 01 or 10 (up, or down).
 * Not modelled: the slave modes, the dual buffers, chip-select polarity
 * (the board sees the address lines as written). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "pic32.h"
#include "svd-map.h"

struct pmp {
    struct sim *s;
    int pic24;                      /* a PIC24E's or dsPIC33E's */
    struct rf_periph *p;
    struct rf_reg *con, *mode, *addr, *din, *aen;
    int irq;
    u64 busy_left;                  /* cycles of the cycle in progress */
    u32 latch;                      /* what the last read cycle read */
    int running;
};

#define MODE_BUSY (1u << 15)

static u32 address(const struct pmp *m)
{
    u32 en = m->aen ? (u32)m->aen->value & 0xffff : 0xffff;
    return (u32)m->addr->value & 0xffff & (en | 0xc000u);
}

static void end_cycle(struct pmp *m)
{
    u64 mode = m->mode->value;
    rf_hw(m->mode, mode & ~(u64)MODE_BUSY);
    u32 inc = (u32)(mode >> 11) & 3, a = (u32)m->addr->value;
    if (inc == 1 || inc == 2) {
        u32 lo = (a + (inc == 1 ? 1u : 0x3fffu)) & 0x3fff;
        rf_hw(m->addr, (m->addr->value & ~0x3fffull) | lo);
    }
    if (((mode >> 13) & 3) == 1) {
        if (m->pic24)
            pic24_irq(m->s, m->irq);
        else
            pic32_irq(m->s, m->irq);
    }
    rf_clock(m->p, &m->running, 0);
}

/* a bus cycle starts: BUSY for its wait states */
static void start_cycle(struct pmp *m)
{
    u64 mode = m->mode->value;
    u32 wb = (u32)(mode >> 6) & 3, wm = (u32)(mode >> 2) & 15, we = (u32)mode & 3;
    m->busy_left = (u64)(wb + 1 + wm + 1 + we + 1) * (m->pic24 ? 1 : pic32_pb_div(m->s, 2));
    rf_hw(m->mode, mode | MODE_BUSY);
    rf_clock(m->p, &m->running, 1);
}

static int master(const struct pmp *m)
{
    return (m->con->value >> 15 & 1) && ((m->mode->value >> 8) & 3) >= 2;
}

static u32 pmp_read(void *ctx, u32 off, int n)
{
    struct pmp *m = ctx;
    if (off != rf_off(m->din) || m->s->bus.debug || !master(m))
        return rf_read(m->p, off, n);
    u32 v = m->latch;
    struct board_io *io = &m->s->io;
    u32 w = io->bus_read ? io->bus_read(io->ctx, address(m)) : 0;
    m->latch = (m->mode->value >> 10 & 1) ? w & 0xffff : w & 0xff;
    m->din->value = m->latch;
    start_cycle(m);
    return v;
}

static void pmp_write(void *ctx, u32 off, int n, u32 v)
{
    struct pmp *m = ctx;
    u64 busy = m->mode->value & MODE_BUSY;
    rf_write(m->p, off, n, v);
    if (off == rf_off(m->mode))         /* BUSY is the hardware's */
        m->mode->value = (m->mode->value & ~(u64)MODE_BUSY) | busy;
    if (off != rf_off(m->din) || !master(m))
        return;
    struct board_io *io = &m->s->io;
    u32 d = (m->mode->value >> 10 & 1) ? v & 0xffff : v & 0xff;
    if (io->bus_write)
        io->bus_write(io->ctx, address(m), d);
    start_cycle(m);
}

static void pmp_tick(void *ctx, u32 cycles)
{
    struct pmp *m = ctx;
    if (cycles >= m->busy_left) {
        m->busy_left = 0;
        end_cycle(m);
    } else {
        m->busy_left -= cycles;
    }
}

static int pmp_next(void *ctx, u32 *cycles)
{
    struct pmp *m = ctx;
    int en = m->pic24 ? pic24_irq_enabled(m->s, m->irq) : pic32_irq_enabled(m->s, m->irq);
    if (!((m->mode->value >> 13 & 3) == 1 && en))
        return 0;
    *cycles = (u32)m->busy_left;
    return 1;
}

static void pmp_reset(void *ctx)
{
    struct pmp *m = ctx;
    m->busy_left = 0;
    m->latch = 0;
    m->running = 0;
}

const struct dev_ops pic32_pmp_ops = {
    "pic32-pmp", pmp_read, pmp_write, pmp_reset, pmp_tick, pmp_next,
};

const struct dev_ops pic24e_pmp_ops = {
    "pic24e-pmp", pmp_read, pmp_write, pmp_reset, pmp_tick, pmp_next,
};

void *pic32_pmp_create(struct sim *s, struct rf_periph *p)
{
    struct pmp m;
    memset(&m, 0, sizeof m);
    m.s = s;
    m.p = p;
    m.con = rf_reg(p, "PMCON");
    m.mode = rf_reg(p, "PMMODE");
    m.addr = rf_reg(p, "PMADDR");
    m.pic24 = pic24_is(s->cpu);
    m.din = rf_reg(p, m.pic24 ? "PMDIN1" : "PMDIN");
    m.aen = rf_reg(p, "PMAEN");
    if (!m.con || !m.mode || !m.addr || !m.din)
        return 0;
    m.irq = m.pic24 ? pic24_irq_named(s, "PMP") : pic32_irq_named(s, "PMP");
    struct pmp *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = m;
    return c;
}
