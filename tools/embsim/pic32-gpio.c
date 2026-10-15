/* pic32-gpio.c -- a PIC32MZ's I/O ports, A to K, over the ATDF's GPIO
 * registers (ANSELx, TRISx, PORTx, LATx, ODCx, CNPUx, CNPDx...).
 *
 *   - Out of reset every pin is an input (TRISx all ones) and every pin
 *     that has an analog function is analog (ANSELx: the bits the file
 *     gives it), as on the part.
 *   - PORTx reads the pins: an output (TRISx 0) what LATx drives -- an
 *     open-drain one (ODCx) at 1 lets go, and reads as an input does; an
 *     analog pin 0 (its digital input is off); an input what the board
 *     drives on it (a button), else its pull-up (CNPUx) as 1, else 0.
 *   - A write to PORTx writes LATx, as on the part.
 *   - The board is told each pin whose driven level changes (an LED),
 *     and asked for the pins it drives (struct board_io).
 * PORTx's value shows in --trace-periph when a read changes it. Not
 * modelled: the change notification interrupts (CNx), the slew rate. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

#define NPORTS 11                   /* A to K */

struct port {
    struct rf_reg *ansel, *tris, *port, *lat, *odc, *cnpu;
    u32 pins;                       /* the pins the port has */
    u32 driven;                     /* the levels the board was last told */
};

struct gpio {
    struct sim *s;
    struct rf_periph *p;
    struct port pt[NPORTS];
};

/* the bits driven high: outputs at 1 (not an open drain let go) */
static u32 drives(const struct port *q)
{
    u32 out = ~(u32)q->tris->value & q->pins;
    u32 lat = (u32)q->lat->value;
    u32 od = q->odc ? (u32)q->odc->value : 0;
    return out & lat & ~od;
}

static u32 pins_read(struct gpio *g, int i)
{
    struct port *q = &g->pt[i];
    struct board_io *io = &g->s->io;
    u32 tris = (u32)q->tris->value, lat = (u32)q->lat->value;
    u32 od = q->odc ? (u32)q->odc->value : 0;
    u32 an = q->ansel ? (u32)q->ansel->value : 0;
    u32 up = q->cnpu ? (u32)q->cnpu->value : 0, v = 0;
    for (int b = 0; b < 32; b++) {
        u32 m = 1u << b;
        int level = 0;
        if (!(q->pins & m) || (an & m))
            continue;
        if (!(tris & m) && !((od & m) && (lat & m))) {
            level = (lat & m) != 0;
        } else if (!(io->pin_in && io->pin_in(io->ctx, i, b, &level))) {
            level = (up & m) != 0;
        }
        if (level)
            v |= m;
    }
    return v;
}

static void tell_board(struct gpio *g)
{
    struct board_io *io = &g->s->io;
    for (int i = 0; i < NPORTS; i++) {
        struct port *q = &g->pt[i];
        if (!q->port)
            continue;
        u32 d = drives(q), ch = d ^ q->driven;
        q->driven = d;
        for (int b = 0; ch && b < 32; b++)
            if ((ch >> b & 1) && io->pin_out)
                io->pin_out(io->ctx, i, b, (int)(d >> b & 1));
    }
}

static int port_of(struct gpio *g, u32 off)
{
    for (int i = 0; i < NPORTS; i++)
        if (g->pt[i].port && off == rf_off(g->pt[i].port))
            return i;
    return -1;
}

static u32 gpio_read(void *ctx, u32 off, int n)
{
    struct gpio *g = ctx;
    int i = port_of(g, off);
    if (i >= 0) {
        struct rf_reg *r = g->pt[i].port;
        u64 v = (r->value & ~(u64)g->pt[i].pins) | pins_read(g, i);
        if (v != r->value)
            rf_hw(r, v);
    }
    return rf_read(g->p, off, n);
}

static void gpio_write(void *ctx, u32 off, int n, u32 v)
{
    struct gpio *g = ctx;
    int i = port_of(g, off);
    if (i >= 0) {
        /* PORTx written is LATx written */
        struct rf_reg *lat = g->pt[i].lat;
        rf_write(g->p, rf_off(lat), n, v);
    } else {
        rf_write(g->p, off, n, v);
    }
    tell_board(g);
}

static void gpio_reset(void *ctx)
{
    struct gpio *g = ctx;
    for (int i = 0; i < NPORTS; i++)
        g->pt[i].driven = 0;
}

const struct dev_ops pic32_gpio_ops = {
    "pic32-gpio", gpio_read, gpio_write, gpio_reset, 0, 0,
};

void *pic32_gpio_create(struct sim *s, struct rf_periph *p)
{
    struct gpio *g = calloc(1, sizeof *g);
    char nm[16];
    int any = 0;
    if (!g)
        die("out of memory");
    g->s = s;
    g->p = p;
    for (int i = 0; i < NPORTS; i++) {
        struct port *q = &g->pt[i];
        char l = (char)('A' + i);
#define REG(f, name) snprintf(nm, sizeof nm, name "%c", l); q->f = rf_reg(p, nm)
        REG(ansel, "ANSEL");
        REG(tris, "TRIS");
        REG(port, "PORT");
        REG(lat, "LAT");
        REG(odc, "ODC");
        REG(cnpu, "CNPU");
#undef REG
        if (!q->port || !q->tris || !q->lat) {
            q->port = 0;
            continue;
        }
        any = 1;
        q->pins = (u32)rf_fields(q->port);
        rf_set_reset(q->tris, rf_fields(q->tris));
        if (q->ansel)
            rf_set_reset(q->ansel, rf_fields(q->ansel));
    }
    if (!any) {
        free(g);
        return 0;
    }
    return g;
}
