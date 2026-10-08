/* uart-cmsdk.c -- the CMSDK APB UART of the MPS2 boards: DATA sends the
 * byte once CTRL's TX enable (bit 0) is set. */
#include <stdlib.h>

#include "devices.h"

struct cmsdk {
    struct sim *sim;
    u32 ctrl;
};

static u32 cmsdk_read(void *ctx, u32 off, int n)
{
    struct cmsdk *u = ctx;
    (void)n;
    if (off == 8)
        return u->ctrl;
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
}

static void cmsdk_reset(void *ctx)
{
    ((struct cmsdk *)ctx)->ctrl = 0;
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
    return u;
}
