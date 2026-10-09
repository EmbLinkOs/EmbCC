/* dwt.c -- the Cortex-M's data watchpoint and trace unit, at 0xE0001000:
 * only its cycle counter, CYCCNT, which counts the cost model's estimate
 * (tools/bench/cost.h) while CTRL.CYCCNTENA is set. The rest reads as
 * zero. */
#include <stdlib.h>

#include "devices.h"

struct dwt {
    struct sim *sim;
    u32 ctrl, base;                 /* CYCCNT is cycles - base when on */
};

static u32 dwt_read(void *ctx, u32 off, int n)
{
    struct dwt *d = ctx;
    (void)n;
    if (off == 0)
        return d->ctrl;
    if (off == 4)
        return (d->ctrl & 1) ? (u32)d->sim->cycles - d->base : d->base;
    return 0;
}

static void dwt_write(void *ctx, u32 off, int n, u32 v)
{
    struct dwt *d = ctx;
    u32 cycles = (u32)d->sim->cycles;
    (void)n;
    if (off == 0) {
        u32 now = (d->ctrl & 1) ? cycles - d->base : d->base;
        d->ctrl = v & 1;
        /* keep CYCCNT where it was across the switch */
        d->base = (d->ctrl & 1) ? cycles - now : now;
    } else if (off == 4) {
        d->base = (d->ctrl & 1) ? cycles - v : v;
    }
}

static void dwt_reset(void *ctx)
{
    struct dwt *d = ctx;
    d->ctrl = d->base = 0;
}

const struct dev_ops dwt_ops = { "dwt", dwt_read, dwt_write, dwt_reset, 0, 0 };

void *dwt_create(struct sim *s, const struct dev_desc *desc)
{
    struct dwt *d = calloc(1, sizeof *d);
    (void)desc;
    if (!d)
        die("out of memory");
    d->sim = s;
    return d;
}
