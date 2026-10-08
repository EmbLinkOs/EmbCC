/* clint.c -- the RISC-V core's CLINT, at 0x2000000 on the virt board:
 * msip (0x0), mtimecmp (0x4000) and mtime (0xbff8), the machine
 * software and timer interrupts of the one hart.
 *
 * mtime counts the core's estimated cycles, as SysTick does a
 * Cortex-M's: one tick per cycle, so the 10 MHz the board's device tree
 * gives as the timebase is the core's clock too, and a timed run gives
 * the same answer every time. Its state is the core's (riscv.h); its
 * next event lets a WFI skip ahead to mtimecmp. */
#include <stdlib.h>

#include "devices.h"
#include "riscv.h"

struct clint {
    struct rv_state *core;
};

static u32 half(u64 v, u32 off)
{
    return (off & 4) ? (u32)(v >> 32) : (u32)v;
}

static u64 set_half(u64 old, u32 off, u32 v)
{
    return (off & 4) ? (old & 0xffffffffull) | (u64)v << 32
                     : (old & ~0xffffffffull) | v;
}

static u32 clint_read(void *ctx, u32 off, int n)
{
    struct clint *k = ctx;
    struct rv_state *save = rs;
    u32 v = 0, sh = 8 * (off & 3);
    rs = k->core;
    if (off < 4)
        v = rs->msip;
    else if (off >= 0x4000 && off < 0x4008)
        v = half(rs->mtimecmp, off);
    else if (off >= 0xbff8 && off < 0xc000)
        v = half(rv_mtime(), off);
    rs = save;
    (void)n;
    return v >> sh;
}

static void clint_write(void *ctx, u32 off, int n, u32 v)
{
    struct clint *k = ctx;
    struct rv_state *save = rs;
    u32 sh = 8 * (off & 3);
    u32 m = n == 4 ? 0xffffffffu : ((1u << (8 * n)) - 1) << sh;
    rs = k->core;
    v <<= sh;
    if (off < 4) {
        rs->msip = (rs->msip & ~m) | (v & m & 1);
    } else if (off >= 0x4000 && off < 0x4008) {
        u32 old = half(rs->mtimecmp, off);
        rs->mtimecmp = set_half(rs->mtimecmp, off, (old & ~m) | (v & m));
    } else if (off >= 0xbff8 && off < 0xc000) {
        u64 t = rv_mtime();
        u32 old = half(t, off);
        u64 nt = set_half(t, off, (old & ~m) | (v & m));
        rs->mtime_off += nt - t;
    }
    rs = save;
}

/* the cycles until mtime reaches mtimecmp, while the timer interrupt is
 * enabled in mie */
static int clint_next(void *ctx, u32 *cycles)
{
    struct clint *k = ctx;
    struct rv_state *save = rs;
    int any = 0;
    rs = k->core;
    if (rs->msip & 1) {
        *cycles = 0;
        any = 1;
    } else if (rs->mie & MIP_MTIP) {
        u64 t = rv_mtime();
        u64 left = rs->mtimecmp > t ? rs->mtimecmp - t : 0;
        *cycles = left > 0xffffffffu ? 0xffffffffu : (u32)left;
        any = 1;
    }
    rs = save;
    return any;
}

const struct dev_ops clint_ops = {
    "clint", clint_read, clint_write, 0, 0, clint_next,
};

void *clint_create(struct sim *s, const struct dev_desc *d)
{
    struct clint *k = calloc(1, sizeof *k);
    (void)d;
    if (!k)
        die("out of memory");
    if (!riscv_is(s->cpu))
        die("the CLINT is a RISC-V core's");
    k->core = (struct rv_state *)s->cpu;
    return k;
}
