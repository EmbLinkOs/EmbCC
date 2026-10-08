/* scs.c -- the Cortex-M's system control space at 0xE000E000: the NVIC's
 * enable, pending, active and priority registers, and the SCB's (CPUID,
 * ICSR, VTOR, AIRCR, SCR, CCR, the handler priorities, SHCSR, the fault
 * status and address registers, CPACR, DEMCR, STIR and the FP context
 * registers). SysTick, inside this space, is systick.c's device and is
 * added to the bus first.
 *
 * The registers are the core's state (cortexm.h): the device's context
 * is the core it belongs to. A register reads as zero and ignores a
 * write where nothing is modelled. */
#include "cortexm.h"
#include "devices.h"

static u32 icsr_read(void)
{
    u32 v = cs->ipsr & 0x1ff;
    int pending = 0;
    for (int n = 2; n < NEXC; n++)
        if (cs->pend[n]) {
            pending = n;
            break;
        }
    v |= (u32)pending << 12;
    if (cs->pend[EXC_PENDSV])
        v |= 1u << 28;
    if (cs->pend[EXC_SYSTICK])
        v |= 1u << 26;
    if (cs->pend[EXC_NMI])
        v |= 1u << 31;
    int nact = 0;
    for (int n = 1; n < NEXC; n++)
        nact += cs->active[n];
    if (nact <= 1)
        v |= 1u << 11;              /* RETTOBASE */
    return v;
}

static int scs_read(u32 o, u32 *v)
{
    *v = 0;
    if (o >= 0x100 && o < 0x140) {          /* ISER */
        u32 w = (o - 0x100) / 4;
        for (int b = 0; b < 32; b++)
            if (16 + w * 32 + b < NEXC && cs->irq_en[16 + w * 32 + b])
                *v |= 1u << b;
        return 1;
    }
    if (o >= 0x180 && o < 0x1c0) {          /* ICER */
        return scs_read(o - 0x80, v);
    }
    if ((o >= 0x200 && o < 0x240) || (o >= 0x280 && o < 0x2c0)) {
        u32 w = (o & 0x7f) / 4;             /* ISPR, ICPR */
        for (int b = 0; b < 32; b++)
            if (16 + w * 32 + b < NEXC && cs->pend[16 + w * 32 + b])
                *v |= 1u << b;
        return 1;
    }
    if (o >= 0x300 && o < 0x340) {          /* IABR */
        u32 w = (o - 0x300) / 4;
        for (int b = 0; b < 32; b++)
            if (16 + w * 32 + b < NEXC && cs->active[16 + w * 32 + b])
                *v |= 1u << b;
        return 1;
    }
    if (o >= 0x400 && o < 0x400 + NEXC - 16) {  /* IPR, by word */
        u32 n = 16 + (o - 0x400);
        for (int b = 0; b < 4; b++)
            if (n + b < NEXC)
                *v |= (u32)(cs->prio[n + b] & cm_prio_mask()) << (8 * b);
        return 1;
    }
    switch (o) {
    case 0x004: *v = 7; return 1;           /* ICTR: 256 lines */
    case 0xd00: *v = cs->model->cpuid; return 1;
    case 0xd04: *v = icsr_read(); return 1;
    case 0xd08: *v = cs->vtor; return 1;
    case 0xd0c: *v = 0xFA050000u | cs->aircr_prigroup << 8; return 1;
    case 0xd10: *v = cs->scr; return 1;
    case 0xd14: *v = cs->ccr; return 1;
    case 0xd18: case 0xd1c: case 0xd20: {
        u32 n = 4 + (o - 0xd18);
        for (int b = 0; b < 4; b++)
            *v |= (u32)(cs->prio[n + b] & cm_prio_mask()) << (8 * b);
        return 1;
    }
    case 0xd24: *v = cs->shcsr; return 1;
    case 0xd28: *v = cs->cfsr; return 1;
    case 0xd2c: *v = cs->hfsr; return 1;
    case 0xd34: *v = cs->mmfar; return 1;
    case 0xd38: *v = cs->bfar; return 1;
    case 0xd88: *v = cs->model->fpu ? cs->cpacr : 0; return 1;
    case 0xdfc: *v = cs->demcr; return 1;
    case 0xf34: *v = cs->fpccr; return 1;
    case 0xf38: *v = cs->fpcar; return 1;
    case 0xf3c: *v = cs->fpdscr; return 1;
    }
    return 1;                               /* reads as zero */
}

static void scs_write(u32 o, u32 v)
{
    if (o >= 0x100 && o < 0x140) {
        u32 w = (o - 0x100) / 4;
        for (int b = 0; b < 32; b++)
            if ((v >> b & 1) && 16 + w * 32 + b < NEXC)
                cs->irq_en[16 + w * 32 + b] = 1;
        return;
    }
    if (o >= 0x180 && o < 0x1c0) {
        u32 w = (o - 0x180) / 4;
        for (int b = 0; b < 32; b++)
            if ((v >> b & 1) && 16 + w * 32 + b < NEXC)
                cs->irq_en[16 + w * 32 + b] = 0;
        return;
    }
    if (o >= 0x200 && o < 0x240) {
        u32 w = (o - 0x200) / 4;
        for (int b = 0; b < 32; b++)
            if (v >> b & 1)
                cm_set_pend(16 + w * 32 + b);
        return;
    }
    if (o >= 0x280 && o < 0x2c0) {
        u32 w = (o - 0x280) / 4;
        for (int b = 0; b < 32; b++)
            if (v >> b & 1)
                cm_clr_pend(16 + w * 32 + b);
        return;
    }
    if (o >= 0x400 && o < 0x400 + NEXC - 16) {
        u32 n = 16 + (o - 0x400);
        for (int b = 0; b < 4; b++)
            if (n + b < NEXC)
                cs->prio[n + b] = (u8)(v >> (8 * b));
        return;
    }
    switch (o) {
    case 0xd04:
        if (v & (1u << 31)) cm_set_pend(EXC_NMI);
        if (v & (1u << 28)) cm_set_pend(EXC_PENDSV);
        if (v & (1u << 27)) cm_clr_pend(EXC_PENDSV);
        if (v & (1u << 26)) cm_set_pend(EXC_SYSTICK);
        if (v & (1u << 25)) cm_clr_pend(EXC_SYSTICK);
        return;
    case 0xd08: cs->vtor = v & 0xffffff80u; return;
    case 0xd0c:
        if ((v >> 16) != 0x05FA)
            return;
        if (cs->model->arch == ARCH_V7M)
            cs->aircr_prigroup = (v >> 8) & 7;
        if (v & 4) {                        /* SYSRESETREQ */
            cs->sim->exit_status = 0;
            sim_end(cs->sim, END_EXIT, "the image requested a reset");
        }
        return;
    case 0xd10: cs->scr = v & 0x16; return;
    case 0xd14: cs->ccr = (cs->ccr & ~0x31Bu) | (v & 0x31Bu); return;
    case 0xd18: case 0xd1c: case 0xd20: {
        u32 n = 4 + (o - 0xd18);
        for (int b = 0; b < 4; b++)
            cs->prio[n + b] = (u8)(v >> (8 * b));
        return;
    }
    case 0xd24: cs->shcsr = v & 0x7ffffu; return;
    case 0xd28: cs->cfsr &= ~v; return;
    case 0xd2c: cs->hfsr &= ~v; return;
    case 0xd88: cs->cpacr = v & 0x00f00000u; return;
    case 0xdfc: cs->demcr = v; return;
    case 0xf00: cm_set_pend(16 + (int)(v & 0x1ff)); return;   /* STIR */
    case 0xf34: cs->fpccr = (cs->fpccr & ~0xC0000000u) | (v & 0xC0000000u); return;
    case 0xf38: cs->fpcar = v & ~7u; return;
    case 0xf3c: cs->fpdscr = v & 0x07C00000u; return;
    }
}

/* The bus's accesses. A read is of the word, shifted to the byte asked
 * for; a word write goes to its address as it is; a narrower one is
 * written to the word with the other bytes zero, except in the priority
 * registers, which are byte-accessible (a CMSIS NVIC_SetPriority writes
 * one byte). */
static u32 scs_rd(void *ctx, u32 off, int n)
{
    u32 v;
    (void)n;
    cs = ctx;
    scs_read(off & ~3u, &v);
    return v >> (8 * (off & 3));
}

static void scs_wr(void *ctx, u32 off, int n, u32 v)
{
    u32 o = off & 0xfff;
    cs = ctx;
    if (n == 4)
        scs_write(off, v);
    else if ((o >= 0x400 && o < 0x5f0) || (o >= 0xd18 && o < 0xd24)) {
        for (int k = 0; k < n; k++) {
            u32 e = (o >= 0xd18 ? 4 + (o - 0xd18) : 16 + (o - 0x400)) + (u32)k;
            if (e < NEXC)
                cs->prio[e] = (u8)(v >> (8 * k));
        }
    } else
        scs_write(off & ~3u, v << (8 * (off & 3)));
}

const struct dev_ops scs_ops = { "scs", scs_rd, scs_wr, 0, 0, 0 };

/* the core is created first, and is the one the device belongs to */
void *scs_create(struct sim *s, const struct dev_desc *d)
{
    (void)d;
    return s->cpu;
}
