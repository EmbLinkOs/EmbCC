/* systick.c -- the Cortex-M's SysTick timer, at 0xE000E010.
 *
 * It counts the core's estimated cycles (the processor clock), not the
 * host's time, so a timed run gives the same answer every time. When the
 * count reaches zero it sets COUNTFLAG and, with TICKINT, pends the
 * SysTick exception (15). Its next event is what lets a WFI skip ahead
 * instead of stepping through the wait.
 *
 * Its registers are word registers inside the system control space: a
 * narrower access reads the word and shifts it, or writes the word with
 * the other bytes zero, as the SCS's do. */
#include <stdlib.h>

#include "devices.h"

struct systick {
    struct sim *sim;
    u32 csr, rvr, cvr;
    int running;                    /* CSR.ENABLE, as sim_clock knows it */
};

static u32 reg_read(struct systick *t, u32 o)
{
    u32 v;
    switch (o) {
    case 0x0: v = t->csr; t->csr &= ~(1u << 16); return v;
    case 0x4: return t->rvr;
    case 0x8: return t->cvr;
    case 0xc: return 0x80000000u;       /* CALIB: no reference */
    }
    return 0;
}

static void reg_write(struct systick *t, u32 o, u32 v)
{
    switch (o) {
    case 0x0:
        t->csr = (t->csr & (1u << 16)) | (v & 7);
        sim_clock(t->sim, &t->running, t->csr & 1);
        return;
    case 0x4: t->rvr = v & 0xffffff; return;
    case 0x8: t->cvr = 0; t->csr &= ~(1u << 16); return;
    }
}

static u32 st_read(void *ctx, u32 off, int n)
{
    (void)n;
    return reg_read(ctx, off & ~3u) >> (8 * (off & 3));
}

static void st_write(void *ctx, u32 off, int n, u32 v)
{
    if (n == 4)
        reg_write(ctx, off, v);
    else
        reg_write(ctx, off & ~3u, v << (8 * (off & 3)));
}

static void st_tick(void *ctx, u32 c)
{
    struct systick *t = ctx;
    if (!(t->csr & 1))
        return;
    while (c--) {
        if (t->cvr == 0) {
            t->cvr = t->rvr;
            continue;
        }
        if (--t->cvr == 0) {
            t->csr |= 1u << 16;
            if (t->csr & 2)
                t->sim->cpu->ops->interrupt(t->sim->cpu, 15);
        }
    }
}

static int st_next_event(void *ctx, u32 *cycles)
{
    struct systick *t = ctx;
    if ((t->csr & 3) != 3)
        return 0;
    *cycles = t->cvr ? t->cvr : t->rvr + 1;
    return 1;
}

static void st_reset(void *ctx)
{
    struct systick *t = ctx;
    t->csr = t->rvr = t->cvr = 0;
    t->running = 0;
}

const struct dev_ops systick_ops = {
    "systick", st_read, st_write, st_reset, st_tick, st_next_event,
};

void *systick_create(struct sim *s, const struct dev_desc *d)
{
    struct systick *t = calloc(1, sizeof *t);
    (void)d;
    if (!t)
        die("out of memory");
    t->sim = s;
    return t;
}
