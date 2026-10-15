/* malta.c -- what QEMU's malta board gives a MIPS image besides RAM and
 * the UART: the FPGA's registers at 0x1f000000, where SOFTRES (0x500)
 * written with 0x42 resets the board -- which QEMU run with -no-reboot
 * makes its exit, and tests/harness/mips's _exit writes after the
 * sentinel -- and the boot ROM at 0x1fc00000, where the core starts
 * (0xbfc00000 in kseg1): QEMU's -kernel puts a loader there that jumps
 * to the image's entry, and so does this, in four instructions (lui,
 * ori, jr and its delay slot's nop). The rest of the FPGA's space reads
 * as zero and ignores writes. */
#include <stdlib.h>

#include "devices.h"

struct malta {
    struct sim *sim;
};

static void fpga_write(void *ctx, u32 off, int n, u32 v)
{
    struct sim *s = ((struct malta *)ctx)->sim;
    (void)n;
    if (off == 0x500 && (v & 0xff) == 0x42 && s->state == RUN) {
        s->exit_status = 0;
        sim_end(s, END_EXIT, "the board's reset (SOFTRES)");
    }
}

static u32 fpga_read(void *ctx, u32 off, int n)
{
    (void)ctx;
    (void)off;
    (void)n;
    return 0;
}

const struct dev_ops malta_fpga_ops = {
    "malta-fpga", fpga_read, fpga_write, 0, 0, 0,
};

void *malta_fpga_create(struct sim *s, const struct dev_desc *d)
{
    struct malta *m = calloc(1, sizeof *m);
    (void)d;
    if (!m)
        die("out of memory");
    m->sim = s;
    return m;
}

/* the loader: lui $25, %hi(entry); ori $25, $25, %lo(entry); jr $25; nop */
static u32 rom_read(void *ctx, u32 off, int n)
{
    struct sim *s = ((struct malta *)ctx)->sim;
    u32 e = (u32)s->entry, w;
    (void)n;
    switch (off & ~3u) {
    case 0: w = 0x3c190000u | e >> 16; break;
    case 4: w = 0x37390000u | (e & 0xffff); break;
    case 8: w = 0x03200008u; break;
    default: w = 0; break;
    }
    return w >> (8 * (off & 3));
}

const struct dev_ops malta_rom_ops = {
    "malta-rom", rom_read, 0, 0, 0, 0,
};

void *malta_rom_create(struct sim *s, const struct dev_desc *d)
{
    return malta_fpga_create(s, d);
}
