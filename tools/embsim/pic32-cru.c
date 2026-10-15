/* pic32-cru.c -- a PIC32MZ's system configuration (CFG) and oscillator
 * (CRU) registers, over the ATDF's.
 *
 *   - SYSKEY: writing 0xAA996655 then 0x556699AA unlocks the registers
 *     that need it, and any other write locks them again; SYSKEY reads
 *     as 0. While locked, a write to OSCCON, SPLLCON or a PBxDIV is
 *     ignored, as on the part, and the first such write is a warning.
 *   - OSCCON: COSC and NOSC start as the configuration words say
 *     (DEVCFG1.FNOSC, in boot flash); setting OSWEN switches at once to
 *     NOSC and clears it. CLKSTAT says every clock is ready.
 *   - PBxDIV: each bus's divider, at reset 2 for PB1-PB5 and PB8 and 1 for
 *     PB7 (the CPU's), with PBDIVRDY set; the timers and the other models
 *     divide the core's cycles by them (pic32_pb_div).
 *   - DEVID reads the part's ID and silicon revision, which atdf.c gives
 *     it from the ATDF (the latest revision the pack lists), and ignores
 *     writes.
 * EmbSim's time is the core's cycles whatever the PLL is set to: the
 * dividers are what changes a peripheral's rate. Not modelled: the clock
 * fail-safe monitor, the reference clock outputs, sleep and idle. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

struct cfg {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *syskey;
    int stage;                      /* 0, 1: the first key written, 2: unlocked */
};

struct cru {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *osccon, *spllcon, *clkstat;
    struct rf_reg *pbdiv[9];        /* PB1DIV... ; [0] a PIC32MX's OSCCON */
    int booted;                     /* OSCCON has the configuration's clock */
    int warned;
};

static struct cfg *cfg_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "CFG") : 0;
    return p && p->ops == &pic32_cfg_ops ? p->ctx : 0;
}

/* the oscillator's peripheral: a PIC32MZ's CRU, a PIC32MX's OSC */
static struct rf_periph *osc_periph(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "CRU") : 0;
    return p ? p : s->svd ? rf_periph(s->svd, "OSC") : 0;
}

static struct cru *cru_of(struct sim *s)
{
    struct rf_periph *p = osc_periph(s);
    return p && p->ops == &pic32_cru_ops ? p->ctx : 0;
}

int pic32_unlocked(struct sim *s)
{
    struct cfg *c = cfg_of(s);
    return !c || c->stage == 2;
}

struct rf_reg *pic32_pb_reg(struct sim *s, int bus)
{
    char nm[8];
    struct rf_periph *p = osc_periph(s);
    snprintf(nm, sizeof nm, "PB%dDIV", bus);
    struct rf_reg *r = p ? rf_reg(p, nm) : 0;
    return r ? r : p ? rf_reg(p, "OSCCON") : 0;     /* a PIC32MX's one bus */
}

u32 pic32_pb_div(struct sim *s, int bus)
{
    struct cru *c = cru_of(s);
    if (!c || bus < 1 || bus > 8)
        return 1;
    return pic32_pb_of(c->pbdiv[bus] ? c->pbdiv[bus] : c->pbdiv[0]);
}

/* ---- CFG ------------------------------------------------------------------ */

static u32 cfg_read(void *ctx, u32 off, int n)
{
    struct cfg *c = ctx;
    if (off == rf_off(c->syskey))
        return 0;
    return rf_read(c->p, off, n);
}

static void cfg_write(void *ctx, u32 off, int n, u32 v)
{
    struct cfg *c = ctx;
    rf_write(c->p, off, n, v);
    if (off != rf_off(c->syskey) || n != 4)
        return;
    if (v == 0xaa996655u)
        c->stage = 1;
    else if (v == 0x556699aau && c->stage == 1)
        c->stage = 2;
    else
        c->stage = 0;
}

static void cfg_reset(void *ctx)
{
    ((struct cfg *)ctx)->stage = 0;
}

const struct dev_ops pic32_cfg_ops = {
    "pic32-cfg", cfg_read, cfg_write, cfg_reset, 0, 0,
};

void *pic32_cfg_create(struct sim *s, struct rf_periph *p)
{
    struct rf_reg *k = rf_reg(p, "SYSKEY");
    if (!k)
        return 0;
    struct cfg *c = calloc(1, sizeof *c);
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    c->syskey = k;
    return c;
}

/* ---- CRU ------------------------------------------------------------------ */

/* OSCCON out of reset: the configuration words' FNOSC (DEVCFG1), and a
 * PIC32MX's peripheral bus divider, FPBDIV, in PBDIV */
static void boot(struct cru *c)
{
    u32 w;
    if (c->booted)
        return;
    c->booted = 1;
    u32 at = rf_config_addr(c->s->svd, "DEVCFG1", 0x1fc0ffc8u);
    if (bus_debug_read(&c->s->bus, at, 4, &w))
        w = 0xffffffffu;
    u64 fnosc = w & 7, v = (c->osccon->value & ~0x7700ull) | fnosc << 12 | fnosc << 8;
    if (c->pbdiv[0])
        v = (v & ~(3ull << 19)) | (u64)(w >> 12 & 3) << 19;
    rf_hw(c->osccon, v);
}

static int locked_reg(const struct cru *c, u32 off)
{
    if (off == rf_off(c->osccon) || (c->spllcon && off == rf_off(c->spllcon)))
        return 1;
    for (int i = 1; i <= 8; i++)
        if (c->pbdiv[i] && off == rf_off(c->pbdiv[i]))
            return 1;
    return 0;
}

static u32 cru_read(void *ctx, u32 off, int n)
{
    struct cru *c = ctx;
    boot(c);
    return rf_read(c->p, off, n);
}

static void cru_write(void *ctx, u32 off, int n, u32 v)
{
    struct cru *c = ctx;
    struct sim *s = c->s;
    boot(c);
    if (locked_reg(c, off) && !pic32_unlocked(s)) {
        if (!c->warned) {
            c->warned = 1;
            fflush(stdout);
            fprintf(stderr, "embsim: warning: a write at pc 0x%08x to "
                    "%s+0x%03x is ignored: it is locked (SYSKEY's unlock "
                    "sequence was not written)\n", s->cpu->ops->pc(s->cpu),
                    c->p->name, off);
        }
        return;
    }
    rf_write(c->p, off, n, v);
    u64 o = c->osccon->value;
    if (off == rf_off(c->osccon) && (o & 1)) {
        u64 nosc = (o >> 8) & 7;
        rf_hw(c->osccon, (o & ~0x7001ull) | nosc << 12);
    }
}

static void cru_reset(void *ctx)
{
    struct cru *c = ctx;
    c->booted = 0;
    c->warned = 0;
}

const struct dev_ops pic32_cru_ops = {
    "pic32-cru", cru_read, cru_write, cru_reset, 0, 0,
};

void *pic32_cru_create(struct sim *s, struct rf_periph *p)
{
    struct cru c;
    char nm[8];
    memset(&c, 0, sizeof c);
    c.s = s;
    c.p = p;
    c.osccon = rf_reg(p, "OSCCON");
    c.spllcon = rf_reg(p, "SPLLCON");
    c.clkstat = rf_reg(p, "CLKSTAT");
    for (int i = 1; i <= 8; i++) {
        snprintf(nm, sizeof nm, "PB%dDIV", i);
        c.pbdiv[i] = rf_reg(p, nm);
    }
    if (!c.osccon)
        return 0;
    if (!c.pbdiv[1])
        c.pbdiv[0] = c.osccon;          /* a PIC32MX: PBDIV is OSCCON's */
    for (int i = 1; i <= 8; i++)
        if (c.pbdiv[i]) {
            rf_set_reset(c.pbdiv[i], 0x8800u | (i == 7 ? 0u : 1u));
            rf_set_acc(c.pbdiv[i], 1u << 11, ACC_RO);           /* PBDIVRDY */
        }
    if (c.pbdiv[1])
        rf_set_acc(c.pbdiv[1], 1u << 15, ACC_RO);               /* PB1 is on */
    rf_set_acc(c.osccon, 0x7000u, ACC_RO);                      /* COSC */
    if (c.clkstat) {
        rf_set_reset(c.clkstat, rf_fields(c.clkstat));
        rf_set_acc(c.clkstat, ~0ull, ACC_RO);
    }
    struct cru *k = malloc(sizeof *k);
    if (!k)
        die("out of memory");
    *k = c;
    return k;
}
