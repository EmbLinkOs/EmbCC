/* uart-pl011.c -- ARM's PL011 UART, as on the lm3s6965: a write to DR
 * sends the byte; FR always says the transmitter is empty and there is
 * nothing to receive. */
#include "devices.h"

static u32 pl011_read(void *ctx, u32 off, int n)
{
    (void)ctx;
    (void)n;
    if (off == 0x18)
        return 0x90;                    /* FR: TX empty, RX empty */
    return 0;
}

static void pl011_write(void *ctx, u32 off, int n, u32 v)
{
    (void)n;
    if (off == 0)
        sim_out(ctx, (int)(v & 0xff));
}

const struct dev_ops pl011_ops = { "pl011", pl011_read, pl011_write, 0, 0, 0 };

void *pl011_create(struct sim *s, const struct dev_desc *d)
{
    (void)d;
    return s;
}
