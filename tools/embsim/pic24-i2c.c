/* pic24-i2c.c -- a PIC24FJ's I2C1 to I2C3 in master mode (the EDC's I2C:
 * I2CxCONL/H, I2CxSTAT, I2CxTRN, I2CxRCV, I2CxBRG...), as the data
 * sheet's section 18 has it, on a bus with no slave on it -- the Explorer
 * 16/32's: its I2C lines go to the MCP2221A's master and to empty
 * headers.
 *
 *   - SEN, RSEN and PEN make a start, a repeated start and a stop: STAT.S
 *     or STAT.P, the bit cleared and the master interrupt's flag (MI2Cx)
 *     a bit time later;
 *   - a byte written to I2CxTRN goes out: TBF and TRSTAT set, then nine
 *     bit times later cleared, with ACKSTAT 1 -- no slave acknowledges --
 *     and the interrupt; written while the module is busy it is a write
 *     collision (IWCOL), the byte dropped;
 *   - RCEN receives a byte: eight bit times later I2CxRCV holds 0xFF (the
 *     bus's pull-ups), RBF is set, RCEN cleared, the interrupt; a byte
 *     received with RBF still set is an overflow (I2COV). Reading
 *     I2CxRCV clears RBF;
 *   - ACKEN sends ACKDT, a bit time;
 *   - I2CEN cleared stops the module: S, P and BCL cleared.
 * The bit time is (BRG + 2) instruction cycles, as the data sheet's BRG
 * formula has it give the clock. Not modelled: slave mode, a slave on
 * the bus, bus collisions, SMBus levels. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define NU 3

/* I2CxCONL */
#define I2CEN   (1u << 15)
#define ACKEN   (1u << 4)
#define RCEN    (1u << 3)
#define PEN     (1u << 2)
#define RSEN    (1u << 1)
#define SEN     1u
/* I2CxSTAT */
#define ACKSTAT (1u << 15)
#define TRSTAT  (1u << 14)
#define BCL     (1u << 10)
#define IWCOL   (1u << 7)
#define I2COV   (1u << 6)
#define ST_P    (1u << 4)
#define ST_S    (1u << 3)
#define RBF     (1u << 1)
#define TBF     1u

enum { OP_NONE, OP_START, OP_STOP, OP_TX, OP_RX, OP_ACK };

struct unit {
    struct rf_reg *con, *stat, *trn, *rcv, *brg;
    int irq;
    int op;                         /* the operation under way */
    u32 left;                       /* its cycles to go */
};

struct i2c {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NU + 1];
    int running;
};

static u32 bit_time(const struct unit *u)
{
    return ((u32)u->brg->value & 0xffff) + 2;
}

static void begin(struct i2c *m, struct unit *u, int op, u32 bits)
{
    u->op = op;
    u->left = bits * bit_time(u);
    rf_clock(m->p, &m->running, 1);
}

static void finish(struct i2c *m, struct unit *u)
{
    u32 st = (u32)u->stat->value, con = (u32)u->con->value;
    switch (u->op) {
    case OP_START:
        st = (st & ~ST_P) | ST_S;
        con &= ~(SEN | RSEN);
        break;
    case OP_STOP:
        st = (st & ~ST_S) | ST_P;
        con &= ~PEN;
        break;
    case OP_TX:
        st = (st & ~(TBF | TRSTAT)) | ACKSTAT;      /* no slave: a NACK */
        break;
    case OP_RX:
        if (st & RBF)
            st |= I2COV;
        rf_hw(u->rcv, 0xff);                        /* the pull-ups */
        st |= RBF;
        con &= ~RCEN;
        break;
    case OP_ACK:
        con &= ~ACKEN;
        break;
    }
    u->op = OP_NONE;
    rf_hw(u->stat, st);
    rf_hw(u->con, con);
    if (u->irq >= 0)
        pic24_irq(m->s, u->irq);
}

static void i2c_tick(void *ctx, u32 cycles)
{
    struct i2c *m = ctx;
    int any = 0;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con || u->op == OP_NONE)
            continue;
        if (cycles >= u->left)
            finish(m, u);
        else
            u->left -= cycles;
        any |= u->op != OP_NONE;
    }
    rf_clock(m->p, &m->running, any);
}

static int i2c_next(void *ctx, u32 *cycles)
{
    struct i2c *m = ctx;
    u32 best = ~0u;
    for (int k = 1; k <= NU; k++)
        if (m->u[k].con && m->u[k].op != OP_NONE && m->u[k].left < best)
            best = m->u[k].left;
    if (best == ~0u)
        return 0;
    *cycles = best;
    return 1;
}

static struct unit *unit_of(struct i2c *m, u32 off, struct rf_reg **which)
{
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con)
            continue;
        struct rf_reg *r[5] = { u->con, u->stat, u->trn, u->rcv, u->brg };
        for (int j = 0; j < 5; j++)
            if (r[j] && (off & ~1u) == rf_off(r[j])) {
                *which = r[j];
                return u;
            }
    }
    return 0;
}

static u32 i2c_read(void *ctx, u32 off, int n)
{
    struct i2c *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    u32 v = rf_read(m->p, off, n);
    if (u && r == u->rcv && !m->s->bus.debug)
        rf_hw(u->stat, u->stat->value & ~(u64)RBF);
    return v;
}

static void i2c_write(void *ctx, u32 off, int n, u32 v)
{
    struct i2c *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    if (!u) {
        rf_write(m->p, off, n, v);
        return;
    }
    if (r == u->trn) {
        if (!(u->con->value & I2CEN))
            return;
        if (u->op != OP_NONE) {
            rf_hw(u->stat, u->stat->value | IWCOL);
            return;
        }
        rf_write(m->p, off, n, v);
        rf_hw(u->stat, u->stat->value | TBF | TRSTAT);
        begin(m, u, OP_TX, 9);
        return;
    }
    if (r == u->stat) {
        /* the program clears IWCOL, I2COV and BCL; the rest is the
         * module's. A byte written leaves the other byte's bits be. */
        u32 x = n == 2 ? v : (off & 1 ? (v & 0xff) << 8 | 0xff : (v & 0xff) | 0xff00);
        u64 keep = ~(u64)((~x) & (IWCOL | I2COV | BCL));
        rf_hw(u->stat, u->stat->value & keep);
        return;
    }
    u32 was = (u32)u->con->value;
    rf_write(m->p, off, n, v);
    if (r != u->con)
        return;
    u32 con = (u32)u->con->value;
    if (!(con & I2CEN)) {
        if (was & I2CEN) {
            rf_hw(u->stat, u->stat->value & ~(u64)(ST_S | ST_P | BCL));
            rf_hw(u->con, con | (1u << 12));        /* SCLREL */
            u->op = OP_NONE;
        }
        return;
    }
    u32 now = con & ~was;
    if (u->op != OP_NONE)
        return;
    if (now & (SEN | RSEN))
        begin(m, u, OP_START, 1);
    else if (now & PEN)
        begin(m, u, OP_STOP, 1);
    else if (now & RCEN)
        begin(m, u, OP_RX, 8);
    else if (now & ACKEN)
        begin(m, u, OP_ACK, 1);
}

static void i2c_reset(void *ctx)
{
    struct i2c *m = ctx;
    for (int k = 1; k <= NU; k++)
        m->u[k].op = OP_NONE;
    m->running = 0;
}

const struct dev_ops pic24_i2c_ops = {
    "pic24-i2c", i2c_read, i2c_write, i2c_reset, i2c_tick, i2c_next,
};

void *pic24_i2c_create(struct sim *s, struct rf_periph *p)
{
    struct i2c *m = calloc(1, sizeof *m);
    char nm[16];
    int any = 0;
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
#define R(f, name) snprintf(nm, sizeof nm, "I2C%d" name, k); u->f = rf_reg(p, nm)
        R(con, "CONL"); R(stat, "STAT"); R(trn, "TRN"); R(rcv, "RCV"); R(brg, "BRG");
#undef R
        if (!u->con || !u->stat || !u->trn || !u->rcv || !u->brg) {
            memset(u, 0, sizeof *u);
            continue;
        }
        any = 1;
        snprintf(nm, sizeof nm, "MI2C%d", k);
        u->irq = pic24_irq_named(s, nm);
        /* the module's status bits: the model's; TRN reads back */
        rf_set_acc(u->stat, 0xffff, ACC_RO);
        rf_set_acc(u->trn, 0xffff, ACC_RW);
    }
    if (!any) {
        free(m);
        return 0;
    }
    return m;
}
