/* uart-cmsdk.c -- the CMSDK APB UART of the MPS2 boards: DATA sends the
 * byte once CTRL's TX enable (bit 0) is set.
 *
 * Its receiver is connected only when the run has an input (--input, or
 * the replay of one; s->rx_on): with CTRL's RX enable (bit 1) a byte
 * arriving waits in DATA and sets STATE's RX full (bit 1); with CTRL's
 * RX interrupt enable (bit 3) it sets INTSTATUS's RX bit (bit 1, cleared
 * by writing it a one) and raises UART0's RX interrupt (IRQ 0). Reading
 * DATA takes the byte. Without an input STATE and INTSTATUS read as
 * zero, as before. */
#include <stdlib.h>

#include "devices.h"

struct cmsdk {
    struct sim *sim;
    u32 ctrl, intst;
    int full;
    u8 rx;
};

static u32 cmsdk_read(void *ctx, u32 off, int n)
{
    struct cmsdk *u = ctx;
    (void)n;
    if (off == 8)
        return u->ctrl;
    if (!u->sim->rx_on)
        return 0;
    switch (off) {
    case 0:
        if (u->full && !u->sim->bus.debug) {
            u->full = 0;
            return u->rx;
        }
        return u->full ? u->rx : 0;
    case 4: return u->full ? 2u : 0u;
    case 0xc: return u->intst;
    }
    return 0;
}

static void cmsdk_write(void *ctx, u32 off, int n, u32 v)
{
    struct cmsdk *u = ctx;
    (void)n;
    if (off == 0 && (u->ctrl & 1))
        sim_out(u->sim, (int)(v & 0xff));
    else if (off == 8)
        u->ctrl = v;
    else if (off == 0xc && u->sim->rx_on)
        u->intst &= ~v;
}

static void cmsdk_reset(void *ctx)
{
    struct cmsdk *u = ctx;
    u->ctrl = 0;
    u->intst = 0;
    u->full = 0;
}

static int cmsdk_room(void *ctx)
{
    struct cmsdk *u = ctx;
    return (u->ctrl & 2) && !u->full;
}

static void cmsdk_put(void *ctx, int c)
{
    struct cmsdk *u = ctx;
    u->rx = (u8)c;
    u->full = 1;
    if (u->ctrl & 8) {
        u->intst |= 2;
        u->sim->cpu->ops->interrupt(u->sim->cpu, 16 + 0);
    }
}

const struct dev_ops cmsdk_uart_ops = {
    "cmsdk-uart", cmsdk_read, cmsdk_write, cmsdk_reset, 0, 0,
};

void *cmsdk_uart_create(struct sim *s, const struct dev_desc *d)
{
    struct cmsdk *u = calloc(1, sizeof *u);
    (void)d;
    if (!u)
        die("out of memory");
    u->sim = s;
    sim_rx_port(s, cmsdk_room, cmsdk_put, u);
    return u;
}
