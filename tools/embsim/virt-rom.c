/* virt-rom.c -- the reset ROM of QEMU's virt board, at 0x1000, where a
 * RISC-V core starts. QEMU puts six instructions there that load a0 with
 * the hart's ID, a1 with the device tree's address and a2 with its
 * firmware information, and jump to the start of RAM; and after them
 * the addresses they load, and the information (OpenSBI's fw_dynamic:
 * a magic number, a version, the next stage's address and mode). This is
 * the same ROM, word for word, for the image loaded: so a run on EmbSim
 * executes what one on QEMU does from the first instruction, and their
 * counts agree. The device tree itself is not there. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"

struct virt_rom {
    struct sim *sim;
    u8 b[0x60];
    int built;                      /* for the image loaded */
};

/* the RAM the image runs in: the board's main memory */
static void ram(struct sim *s, u32 *base, u32 *end)
{
    for (int i = 0; i < s->bus.nrg; i++)
        if (s->bus.rg[i].kind == MEM_RAM && s->bus.rg[i].base >= 0x80000000u) {
            *base = s->bus.rg[i].base;
            *end = s->bus.rg[i].base + s->bus.rg[i].size;
            return;
        }
    *base = 0x80000000u;
    *end = 0x88000000u;
}

static void put(u8 *p, u64 v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (u8)(v >> (8 * i));
}

static void build(struct virt_rom *r)
{
    struct sim *s = r->sim;
    int x64 = s->elf64;
    u32 base, end;
    ram(s, &base, &end);
    /* QEMU's riscv_compute_fdt_addr: below the end of RAM (or 3 GiB),
     * on a 2 MiB boundary */
    u64 top = end ? end : 0x100000000ull;
    if (base < 0xc0000000u && top > 0xc0000000u)
        top = 0xc0000000u;
    u32 fdt = (u32)(top - 0x2000) & ~0x1fffffu;
    static const u32 code[6] = { 0x00000297, 0x02828613, 0xf1402573,
                                 0, 0, 0x00028067 };
    memset(r->b, 0, sizeof r->b);
    for (int i = 0; i < 6; i++)
        put(r->b + 4 * i, code[i], 4);
    put(r->b + 12, x64 ? 0x0202b583 : 0x0202a583, 4);   /* l[dw] a1, 32(t0) */
    put(r->b + 16, x64 ? 0x0182b283 : 0x0182a283, 4);   /* l[dw] t0, 24(t0) */
    put(r->b + 24, base, 8);
    put(r->b + 32, fdt, 8);
    int w = x64 ? 8 : 4;
    put(r->b + 40, 0x4942534f, w);                       /* "OSBI" */
    put(r->b + 40 + w, 2, w);                            /* version */
    put(r->b + 40 + 2 * w, s->entry, w);                 /* next_addr */
    put(r->b + 40 + 3 * w, 1, w);                        /* next_mode */
}

static u32 rom_read(void *ctx, u32 off, int n)
{
    struct virt_rom *r = ctx;
    if (!r->built) {
        build(r);
        r->built = 1;
    }
    u32 v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | (off + (u32)i < sizeof r->b ? r->b[off + (u32)i] : 0);
    return v;
}

static void rom_reset(void *ctx)
{
    ((struct virt_rom *)ctx)->built = 0;
}

const struct dev_ops virt_rom_ops = {
    "virt-rom", rom_read, 0, rom_reset, 0, 0,
};

void *virt_rom_create(struct sim *s, const struct dev_desc *d)
{
    struct virt_rom *r = calloc(1, sizeof *r);
    (void)d;
    if (!r)
        die("out of memory");
    r->sim = s;
    return r;
}
