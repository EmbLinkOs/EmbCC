/* bus.c -- memory, bit-band aliases and memory-mapped devices.
 *
 * An access goes to the first of these that answers at its address:
 *   1. a memory region holding all of its bytes (RAM, or flash, which
 *      ignores the core's stores once the image is loaded);
 *   2. a bit-band alias: one word per bit of the target, a read giving
 *      the bit and a write setting or clearing it;
 *   3. a device, in the order the board and the core added them, so a
 *      device inside a wider one (SysTick inside the system control
 *      space, a UART inside the peripheral space that reads as zero) is
 *      added first.
 * Nothing answering is a bus error, which the core turns into its fault
 * (bus->fail_addr says where); so is a device refusing the access
 * (bus_fault: the SVD's register file, where no register is). */
#include <stdlib.h>
#include <string.h>

#include "sim.h"

void bus_add_region(struct bus *b, u32 base, u32 size, int kind)
{
    if (!size)
        return;
    if (b->nrg == BUS_REGIONS)
        die("too many memory regions");
    struct region *r = &b->rg[b->nrg];
    r->base = base;
    r->size = size;
    r->rom = 0;
    r->kind = kind;
    r->mem = calloc(size, 1);
    if (!r->mem)
        die("out of memory for %u bytes at 0x%08x", size, base);
    b->nrg++;
}

void bus_add_device(struct bus *b, u32 base, u32 size,
                    const struct dev_ops *ops, void *ctx)
{
    if (b->ndev == BUS_DEVICES)
        die("too many devices");
    b->dev[b->ndev].base = base;
    b->dev[b->ndev].size = size;
    b->dev[b->ndev].ops = ops;
    b->dev[b->ndev].ctx = ctx;
    b->ndev++;
}

void bus_add_mirror(struct bus *b, u32 base, u32 target)
{
    struct region *t = bus_region(b, target, 1);
    if (!t)
        die("no memory at 0x%08x for a mirror at 0x%08x", target, base);
    if (b->nrg == BUS_REGIONS)
        die("too many memory regions");
    b->rg[b->nrg] = *t;
    b->rg[b->nrg].base = base;
    b->nrg++;
}

void bus_add_alias(struct bus *b, u32 base, u32 size, u32 target)
{
    if (!size)
        return;
    if (b->nbb == BUS_ALIASES)
        die("too many bit-band aliases");
    b->bb[b->nbb].base = base;
    b->bb[b->nbb].size = size;
    b->bb[b->nbb].target = target;
    b->nbb++;
}

static int alias_of(struct bus *b, u32 a, u32 *target, int *bit)
{
    for (int i = 0; i < b->nbb; i++)
        if (a - b->bb[i].base < b->bb[i].size) {
            *target = b->bb[i].target + ((a - b->bb[i].base) >> 5);
            *bit = (int)((a >> 2) & 7);
            return 1;
        }
    return 0;
}

static struct device *device_of(struct bus *b, u32 a)
{
    for (int i = 0; i < b->ndev; i++)
        if (a - b->dev[i].base < b->dev[i].size)
            return &b->dev[i];
    return 0;
}

static int rd(struct bus *b, u32 a, int n, u32 *v)
{
    struct region *r = bus_region(b, a, (u32)n);
    if (r) {
        *v = mem_rd_le(r->mem + (a - r->base), n);
        return 0;
    }
    u32 t, x;
    int bit;
    if (alias_of(b, a, &t, &bit)) {
        if (rd(b, t, 1, &x))
            return -1;
        *v = (x >> bit) & 1;
        return 0;
    }
    struct device *d = device_of(b, a);
    if (d) {
        x = d->ops->read ? d->ops->read(d->ctx, a - d->base, n) : 0;
        if (b->dev_fault) {
            b->dev_fault = 0;
            b->fail_addr = a;
            return -1;
        }
        *v = n == 4 ? x : x & ((1u << (8 * n)) - 1);
        return 0;
    }
    b->fail_addr = a;
    return -1;
}

static int wr(struct bus *b, u32 a, int n, u32 v, int debug)
{
    if (!debug && b->snoop)
        b->snoop(b->snoop_ctx, a);
    struct region *r = bus_region(b, a, (u32)n);
    if (r) {
        if (!r->rom || debug)
            mem_wr_le(r->mem + (a - r->base), n, v);
        return 0;
    }
    u32 t, byte;
    int bit;
    if (alias_of(b, a, &t, &bit)) {
        if (rd(b, t, 1, &byte))
            return -1;
        return wr(b, t, 1, (v & 1) ? byte | 1u << bit : byte & ~(1u << bit),
                  debug);
    }
    struct device *d = device_of(b, a);
    if (d) {
        if (d->ops->write)
            d->ops->write(d->ctx, a - d->base, n, v);
        if (b->dev_fault) {
            b->dev_fault = 0;
            b->fail_addr = a;
            return -1;
        }
        return 0;
    }
    b->fail_addr = a;
    return -1;
}

/* Whether an access of the core's touches a watchpoint; the first one
 * touched is recorded. As QEMU's, the address reported is the higher of
 * the access's and the watchpoint's. */
int bus_watch_check(struct bus *b, u32 a, int n, int write)
{
    for (int i = 0; i < b->nwatch; i++) {
        struct watch *w = &b->watch[i];
        int k = w->kind;
        if (k != WATCH_ACCESS && k != (write ? WATCH_WRITE : WATCH_READ))
            continue;
        /* the two ranges overlap */
        if (a - w->addr < w->len || w->addr - a < (u32)n) {
            if (!b->watch_hit) {
                b->watch_hit = 1;
                b->watch_kind = k;
                b->watch_addr = a > w->addr ? a : w->addr;
            }
            return 1;
        }
    }
    return 0;
}

int bus_read(struct bus *b, u32 a, int n, u32 *v)
{
    return rd(b, a, n, v);
}

int bus_write(struct bus *b, u32 a, int n, u32 v)
{
    return wr(b, a, n, v, 0);
}

/* b->debug tells a device the access is the debugger's: no side effect
 * of a read, no trace */
int bus_debug_read(struct bus *b, u32 a, int n, u32 *v)
{
    b->debug = 1;
    int r = rd(b, a, n, v);
    b->debug = 0;
    return r;
}

int bus_debug_write(struct bus *b, u32 a, int n, u32 v)
{
    b->debug = 1;
    int r = wr(b, a, n, v, 1);
    b->debug = 0;
    return r;
}

int bus_watch_add(struct bus *b, int kind, u32 addr, u32 len)
{
    if (b->nwatch == BUS_WATCHES || !len)
        return -1;
    b->watch[b->nwatch].kind = kind;
    b->watch[b->nwatch].addr = addr;
    b->watch[b->nwatch].len = len;
    b->nwatch++;
    return 0;
}

int bus_watch_remove(struct bus *b, int kind, u32 addr, u32 len)
{
    for (int i = 0; i < b->nwatch; i++) {
        struct watch *w = &b->watch[i];
        if (w->kind == kind && w->addr == addr && w->len == len) {
            *w = b->watch[--b->nwatch];
            return 0;
        }
    }
    return -1;
}
