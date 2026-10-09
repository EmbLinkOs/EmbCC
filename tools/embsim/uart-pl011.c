/* uart-pl011.c -- ARM's PL011 UART, as on the lm3s6965: a write to DR
 * sends the byte; FR always says the transmitter is empty.
 *
 * Its receiver is connected only when the run has an input (--input, or
 * the replay of one; s->rx_on): a byte arriving waits in DR, FR's RXFE
 * clears (RXFF sets: the FIFO is off), RIS's RXRIS sets, and with IMSC's
 * RXIM the UART's interrupt (the lm3s6965's IRQ 5) is raised; reading DR
 * takes the byte and clears RXRIS, as does ICR. Without an input the
 * receiver is empty and those registers read as zero, as before. */
#include <stdlib.h>

#include "devices.h"

#define RXIM 0x10                   /* IMSC, RIS, MIS, ICR: the receive interrupt */

struct pl011 {
    struct sim *s;
    u32 imsc, ris;
    int full;                       /* DR holds a received byte */
    u8 rx;
};

static void irq(struct pl011 *u)
{
    if (u->ris & u->imsc & RXIM)
        u->s->cpu->ops->interrupt(u->s->cpu, 16 + 5);
}

static u32 pl011_read(void *ctx, u32 off, int n)
{
    struct pl011 *u = ctx;
    (void)n;
    if (!u->s->rx_on) {
        if (off == 0x18)
            return 0x90;                /* FR: TX empty, RX empty */
        return 0;
    }
    switch (off) {
    case 0x00:
        if (u->full && !u->s->bus.debug) {
            u->full = 0;
            u->ris &= ~(u32)RXIM;
            return u->rx;
        }
        return u->full ? u->rx : 0;
    case 0x18: return u->full ? 0xc0 : 0x90;    /* FR: TXFE, and RXFF or RXFE */
    case 0x38: return u->imsc;
    case 0x3c: return u->ris;
    case 0x40: return u->ris & u->imsc;
    }
    return 0;
}

static void pl011_write(void *ctx, u32 off, int n, u32 v)
{
    struct pl011 *u = ctx;
    (void)n;
    if (off == 0)
        sim_out(u->s, (int)(v & 0xff));
    else if (u->s->rx_on && off == 0x38) {
        u->imsc = v & 0x7ff;
        irq(u);
    } else if (u->s->rx_on && off == 0x44)
        u->ris &= ~v;
}

static void pl011_reset(void *ctx)
{
    struct pl011 *u = ctx;
    u->imsc = u->ris = 0;
    u->full = 0;
}

static int pl011_room(void *ctx)
{
    return !((struct pl011 *)ctx)->full;
}

static void pl011_put(void *ctx, int c)
{
    struct pl011 *u = ctx;
    u->rx = (u8)c;
    u->full = 1;
    u->ris |= RXIM;
    irq(u);
}

const struct dev_ops pl011_ops = { "pl011", pl011_read, pl011_write, pl011_reset, 0, 0 };

void *pl011_create(struct sim *s, const struct dev_desc *d)
{
    struct pl011 *u = calloc(1, sizeof *u);
    (void)d;
    if (!u)
        die("out of memory");
    u->s = s;
    sim_rx_port(s, pl011_room, pl011_put, u);
    return u;
}
