/* uart-16550.c -- the NS16550A of QEMU's virt board, at 0x10000000: a
 * byte written to THR is sent, LSR says the transmitter is always empty,
 * and the receiver never has anything. The other registers keep what is
 * written to them, and LCR's DLAB switches the first two to the divisor
 * latch, as on the part, so a driver's initialisation runs unchanged.
 *
 * Its receiver is connected only when the run has an input (--input, or
 * the replay of one; s->rx_on): a byte arriving waits in RBR, LSR's DR
 * (bit 0) says so, and reading RBR takes it. There is no PLIC, so no
 * interrupt: a driver polls LSR. Without an input LSR says only that
 * the transmitter is empty, as before. */
#include <stdlib.h>

#include "devices.h"

struct u16550 {
    struct sim *sim;
    u8 ier, lcr, mcr, scr, dll, dlm, fcr;
    u8 rx;
    int full;
};

static u32 u16550_read(void *ctx, u32 off, int n)
{
    struct u16550 *u = ctx;
    int dlab = (u->lcr & 0x80) != 0;
    (void)n;
    switch (off) {
    case 0:
        if (dlab)
            return u->dll;
        if (u->sim->rx_on && u->full) {             /* RBR */
            if (!u->sim->bus.debug)
                u->full = 0;
            return u->rx;
        }
        return 0;
    case 1: return dlab ? u->dlm : u->ier;
    case 2: return (u->fcr & 1) ? 0xc1 : 0x01;      /* IIR: none pending */
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: return 0x60u | (u->sim->rx_on && u->full ? 1u : 0u);    /* LSR: THRE, TEMT, DR */
    case 6: return 0xb0;                            /* MSR: DCD, DSR, CTS */
    case 7: return u->scr;
    }
    return 0;
}

static void u16550_write(void *ctx, u32 off, int n, u32 v)
{
    struct u16550 *u = ctx;
    int dlab = (u->lcr & 0x80) != 0;
    (void)n;
    switch (off) {
    case 0:
        if (dlab)
            u->dll = (u8)v;
        else
            sim_out(u->sim, (int)(v & 0xff));
        return;
    case 1:
        if (dlab)
            u->dlm = (u8)v;
        else
            u->ier = (u8)(v & 0x0f);
        return;
    case 2: u->fcr = (u8)v; return;
    case 3: u->lcr = (u8)v; return;
    case 4: u->mcr = (u8)(v & 0x1f); return;
    case 7: u->scr = (u8)v; return;
    }
}

static void u16550_reset(void *ctx)
{
    struct u16550 *u = ctx;
    struct sim *s = u->sim;
    *u = (struct u16550){ 0 };
    u->sim = s;
}

static int u16550_room(void *ctx)
{
    return !((struct u16550 *)ctx)->full;
}

static void u16550_put(void *ctx, int c)
{
    struct u16550 *u = ctx;
    u->rx = (u8)c;
    u->full = 1;
}

const struct dev_ops ns16550a_ops = {
    "ns16550a", u16550_read, u16550_write, u16550_reset, 0, 0,
};

void *ns16550a_create(struct sim *s, const struct dev_desc *d)
{
    struct u16550 *u = calloc(1, sizeof *u);
    (void)d;
    if (!u)
        die("out of memory");
    u->sim = s;
    sim_rx_port(s, u16550_room, u16550_put, u);
    return u;
}
