/* uart-nrf51.c -- the nRF51's UART, as on the micro:bit: TXD sends the
 * byte only once ENABLE is 4 and STARTTX has run (and STOPTX has not
 * since); EVENTS_TXDRDY always reads as set. */
#include <stdlib.h>

#include "devices.h"

struct nrf51 {
    struct sim *sim;
    u32 enable;
    int tx_started;
};

static u32 nrf51_read(void *ctx, u32 off, int n)
{
    (void)ctx;
    (void)n;
    if (off == 0x11c)
        return 1;                       /* EVENTS_TXDRDY */
    return 0;
}

static void nrf51_write(void *ctx, u32 off, int n, u32 v)
{
    struct nrf51 *u = ctx;
    (void)n;
    if (off == 0x008)
        u->tx_started = 1;
    else if (off == 0x00c)
        u->tx_started = 0;
    else if (off == 0x500)
        u->enable = v;
    else if (off == 0x51c && u->enable == 4 && u->tx_started)
        sim_out(u->sim, (int)(v & 0xff));
}

static void nrf51_reset(void *ctx)
{
    struct nrf51 *u = ctx;
    u->enable = 0;
    u->tx_started = 0;
}

const struct dev_ops nrf51_uart_ops = {
    "nrf51-uart", nrf51_read, nrf51_write, nrf51_reset, 0, 0,
};

void *nrf51_uart_create(struct sim *s, const struct dev_desc *d)
{
    struct nrf51 *u = calloc(1, sizeof *u);
    (void)d;
    if (!u)
        die("out of memory");
    u->sim = s;
    return u;
}
