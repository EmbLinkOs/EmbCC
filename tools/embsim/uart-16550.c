/* uart-16550.c -- the NS16550A of QEMU's virt board, at 0x10000000: a
 * byte written to THR is sent, LSR says the transmitter is always empty,
 * and the receiver never has anything. The other registers keep what is
 * written to them, and LCR's DLAB switches the first two to the divisor
 * latch, as on the part, so a driver's initialisation runs unchanged. */
#include <stdlib.h>

#include "devices.h"

struct u16550 {
    struct sim *sim;
    u8 ier, lcr, mcr, scr, dll, dlm, fcr;
};

static u32 u16550_read(void *ctx, u32 off, int n)
{
    struct u16550 *u = ctx;
    int dlab = (u->lcr & 0x80) != 0;
    (void)n;
    switch (off) {
    case 0: return dlab ? u->dll : 0;               /* RBR: nothing */
    case 1: return dlab ? u->dlm : u->ier;
    case 2: return (u->fcr & 1) ? 0xc1 : 0x01;      /* IIR: none pending */
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: return 0x60;                            /* LSR: THRE, TEMT */
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
    return u;
}
