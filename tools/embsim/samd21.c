/* samd21.c -- the SAM D21's peripherals EmbSim models, over Microchip's
 * ATSAMD21G18A.svd, the "Atmel SAM D21E / SAM D21G / SAM D21J" datasheet
 * (Atmel-42181G, 09/2015) the reference:
 *
 *   - SYSCTRL (16): the oscillators. OSC8M runs out of reset at 8 MHz /
 *     2^PRESC (PRESC 3: 1 MHz); OSCULP32K always at 32.768 kHz; OSC32K
 *     and XOSC32K (the Arduino Zero's crystal) once ENABLE is set, ready
 *     (PCLKSR's OSC32KRDY, XOSC32KRDY) after their STARTUP time (tables
 *     16-10 and 16-9) -- and their 32 kHz output only with EN32K; the
 *     DFLL48M (DFLLCTRL.ENABLE): open loop at 48 MHz, closed loop (MODE)
 *     at DFLLMUL.MUL times its reference, generic clock 0, locking
 *     (DFLLLCKC, DFLLLCKF) 200 us after it is enabled with the reference
 *     running (table 36-50's typical) -- never without one, which EmbSim
 *     warns of. DFLLRDY and OSC8MRDY are always set: a write takes at
 *     once. INTFLAG has PCLKSR's rises, cleared by writing 1;
 *   - GCLK (14): the generators 0-8 (GENCTRL, GENDIV) and the generic
 *     clocks (CLKCTRL), each register reached through its ID -- a write
 *     of the whole register sets one, an 8-bit write of the ID selects
 *     the one a read returns; their reset values are tables 14-5, 14-9
 *     and 14-13's (generator 0 on OSC8M, 2 on OSCULP32K). A generator is
 *     its source / GENDIV.DIV (or 2^(DIV+1) with DIVSEL), the DIV bits
 *     each has (table 14-11); a source not running yet leaves it on the
 *     one before until it is (14.6.2.6). STATUS.SYNCBUSY is never set;
 *   - PM (15): CPUSEL's divider of generator 0, the CPU's clock and the
 *     board's time; the APBA, APBB and APBC masks, which gate the
 *     peripherals' registers (a read gives 0 and a write is lost, with a
 *     warning: "its registers cannot be read or written", 15.6.2.6); out
 *     of reset the SERCOMs and TCs are masked (table 15-1). INTFLAG.CKRDY
 *     on a write of CPUSEL or an APBxSEL. RCAUSE says POR;
 *   - NVMCTRL (21): INTFLAG.READY is set (the controller is idle) and
 *     PARAM gives 4096 pages of 64 bytes; CTRLB.RWS is kept, and a CPU
 *     clock above what its wait states allow at 3.3 V (24 MHz with none,
 *     48 with one: table 36-39) is warned of, once;
 *   - PORT and PORT_IOBUS (22), one model: per group (PA, PB) DIR, OUT
 *     and their CLR, SET and TGL; PINCFG and PMUX, also through WRCONFIG.
 *     A pin with DIR set and no PMUXEN drives OUT's bit; else the board
 *     drives it, or PINCFG.PULLEN pulls it to OUT's bit, or it floats (0).
 *     IN shows the pins whose PINCFG.INEN is set (table 22-1). Each pin's
 *     level goes to the board as it changes;
 *   - SERCOM0-5 as USARTs (25; CTRLA.MODE 0 or 1): CTRLA, CTRLB and BAUD
 *     are enable-protected (a write while enabled is lost). Enabled with
 *     TXEN, with the APB clock and its generic clock running, a byte
 *     written to DATA goes out at once (to stdout), and DRE and TXC are
 *     set; the first SERCOM whose receiver is enabled takes the run's
 *     input (--input) into its two-level buffer: RXC while it holds a
 *     byte, read from DATA, BUFOVF when a third arrives. INTENSET,
 *     INTENCLR, INTFLAG and the interrupt while a flag set is enabled.
 *     SYNCBUSY is never set. The other modes keep what is written;
 *   - TC3-5 (29): COUNT16 and COUNT8 (and COUNT32 on TC4, TC5 its slave),
 *     counting GCLK_TCx / PRESCALER while enabled and not stopped, up or
 *     down (CTRLB.DIR), to the top -- the maximum, COUNT8's PER, or CC0
 *     in MFRQ and MPWM -- and round, with OVF; MCx as the counter leaves
 *     CCx; ONESHOT stops at the wrap; CMD's RETRIGGER and STOP. COUNT
 *     reads the value of the last read request (READREQ.RREQ, or RCONT
 *     continuously), as the part's synchronised read does; CTRLA is
 *     enable-protected; STATUS.STOP. INTFLAG, INTENSET, INTENCLR and the
 *     interrupt.
 *
 * Not modelled: the XOSC and FDPLL96M (0 Hz), the DFLL's USB recovery,
 * fine and coarse steps and its frequency before it locks; generators'
 * GCLK_IO pins; WRTLOCK; the clocks' sleep and on-demand behaviour; the
 * WDT, RTC, EIC, EVSYS, DMAC, USB, ADC, DAC, AC, TCC, I2S, PTC, NVMCTRL's
 * commands and the SERCOMs' SPI and I2C modes (their registers keep what
 * is written); the TCs' waveform outputs, captures and events; the
 * USARTs' baud timing, frame format, errors but BUFOVF, flow control. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define OSC32_HZ 32768u
#define DFLL_HZ 48000000u
#define NGEN 9
#define NCLK 0x25

static struct rf_reg *reg(struct rf_periph *p, u32 off)
{
    return rf_reg_at(p->sim->svd, p->base + off);
}

static u32 rd(struct rf_periph *p, u32 off)
{
    struct rf_reg *r = reg(p, off);
    return r ? (u32)r->value : 0;
}

static void hw(struct rf_periph *p, u32 off, u32 v)
{
    struct rf_reg *r = reg(p, off);
    if (r && r->value != v)
        rf_hw(r, v);
}

/* the bytes [off, off + n) of a value v placed at `at` in a register
 * file of bytes: little-endian */
static void put_bytes(u8 *b, u32 off, int n, u32 v)
{
    for (int i = 0; i < n; i++)
        b[off + (u32)i] = (u8)(v >> (8 * i));
}

static u32 get_bytes(const u8 *b, u32 off, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u32)b[off + (u32)i] << (8 * i);
    return v;
}

/* ---- the part: its clock tree, and the board's time ------------------------ */

struct samd {
    struct sim *s;
    struct rf_periph *sysctrl, *gclk, *pm, *nvm;
    /* GCLK's indirect registers, and the IDs a read returns */
    u32 genctrl[NGEN], gendiv[NGEN];
    u32 clkctrl[NCLK];
    int sel_gen, sel_div, sel_clk;
    int eff_src[NGEN];              /* the source each generator runs on */
    /* SYSCTRL: when each oscillator is (or was) ready, in board ns; the
     * DFLL's lock */
    double xosc32k_at, osc32k_at, lock_at;
    int xosc32k_on, osc32k_on, dfll_closed_on;
    u32 pclksr;                     /* as last worked out: INTFLAG's rises */
    /* the CPU's clock and the board's time */
    u64 dev, at;
    double ns;
    u32 hz;
    int running, warned_rws, warned_ref;
};

static struct samd *part_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "SYSCTRL") : 0;
    return p && p->ops == &samd21_sysctrl_ops ? p->ctx : 0;
}

static double now_ns(struct samd *d)
{
    return d->ns + (d->hz ? (double)(d->dev - d->at) * 1e9 / d->hz : 0);
}

static double samd_now_ns(struct sim *s)
{
    struct samd *d = part_of(s);
    return d ? now_ns(d) : 0;
}

static u32 gen_hz(struct samd *d, int g, int depth);
void samd21_clocks_changed(struct sim *s);

/* a source's frequency, 0 while it is not running (table 14-7) */
static u32 src_hz(struct samd *d, int src, int depth)
{
    struct rf_periph *sc = d->sysctrl;
    double t = now_ns(d);
    switch (src) {
    case 2:                                     /* GCLKGEN1 */
        return gen_hz(d, 1, depth + 1);
    case 3:                                     /* OSCULP32K */
        return OSC32_HZ;
    case 4:                                     /* OSC32K, with EN32K */
        return d->osc32k_on && t >= d->osc32k_at && (rd(sc, 0x18) >> 2 & 1) ? OSC32_HZ : 0;
    case 5:                                     /* XOSC32K, with EN32K */
        return d->xosc32k_on && t >= d->xosc32k_at && (rd(sc, 0x14) >> 3 & 1) ? OSC32_HZ : 0;
    case 6: {                                   /* OSC8M */
        u32 o = rd(sc, 0x20);
        return o >> 1 & 1 ? 8000000u >> (o >> 8 & 3) : 0;
    }
    case 7: {                                   /* DFLL48M */
        u32 c = rd(sc, 0x24);
        if (!(c >> 1 & 1))
            return 0;
        if (!(c >> 2 & 1))
            return DFLL_HZ;                     /* open loop */
        u32 ref = d->clkctrl[0] >> 14 & 1 ? gen_hz(d, (int)(d->clkctrl[0] >> 8 & 15), depth + 1) : 0;
        return (rd(sc, 0x2c) & 0xffff) * ref;
    }
    }
    return 0;                                   /* XOSC, GCLKIN, FDPLL96M */
}

/* a generator's output: its source divided (table 14-11's DIV bits) */
static u32 gen_hz(struct samd *d, int g, int depth)
{
    static const u32 divmask[NGEN] = { 0xff, 0xffff, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (g < 0 || g >= NGEN || depth > 3 || !(d->genctrl[g] >> 16 & 1))
        return 0;
    u32 f = src_hz(d, d->eff_src[g], depth);
    u32 div = d->gendiv[g] >> 8 & divmask[g];
    if (d->genctrl[g] >> 20 & 1)                /* DIVSEL: 2^(DIV+1) */
        return div + 1 >= 32 ? 0 : f >> (div + 1);
    return div > 1 ? f / div : f;
}

/* a generic clock's frequency (table 14-4's IDs), 0 when it is off */
u32 samd21_gclk_hz(struct sim *s, int id)
{
    struct samd *d = part_of(s);
    if (!d || id < 0 || id >= NCLK || !(d->clkctrl[id] >> 14 & 1))
        return 0;
    return gen_hz(d, (int)(d->clkctrl[id] >> 8 & 15), 0);
}

/* a generator whose new source was not running switches when it is */
static void switch_sources(struct samd *d)
{
    for (int g = 0; g < NGEN; g++) {
        int want = (int)(d->genctrl[g] >> 8 & 31);
        if (want != d->eff_src[g] && src_hz(d, want, 0))
            d->eff_src[g] = want;
    }
}

static void check_rws(struct samd *d)
{
    u32 rws = d->nvm ? rd(d->nvm, 0x4) >> 1 & 15 : 0;
    u32 max = rws >= 1 ? 48000000u : 24000000u;
    if (d->hz > max && !d->warned_rws) {
        d->warned_rws = 1;
        fflush(stdout);
        fprintf(stderr, "embsim: warning: the CPU runs at %.3f MHz with NVMCTRL's "
                "CTRLB.RWS %u: at 3.3 V the flash needs %s wait state above %u MHz "
                "(table 36-39), and the part would read it wrong\n",
                d->hz / 1e6, (unsigned)rws, rws ? "two" : "a", (unsigned)(max / 1000000));
    }
}

/* the clocks changed: the CPU's (generator 0 / 2^CPUDIV) and the time */
static void clocks(struct samd *d)
{
    switch_sources(d);
    u32 h = gen_hz(d, 0, 0);
    if (d->pm)
        h >>= rd(d->pm, 0x8) & 7;
    if (!h) {
        if (d->s->state == RUN)
            sim_end(d->s, END_LOCKUP, "the CPU's clock stopped: generator 0 "
                    "(GCLK_MAIN) has no running source");
        return;
    }
    if (h != d->hz) {
        d->ns = now_ns(d);
        d->at = d->dev;
        d->hz = h;
        check_rws(d);
    }
    samd21_clocks_changed(d->s);
}

u32 samd21_cpu_hz(struct sim *s)
{
    struct samd *d = part_of(s);
    return d ? d->hz : 1000000u;
}

/* ---- SYSCTRL ------------------------------------------------------------------ */

#define S_INTENCLR 0x00u
#define S_INTENSET 0x04u
#define S_INTFLAG 0x08u
#define S_PCLKSR 0x0cu

/* STARTUP's OSCULP32K cycles (table 16-9) and OSC32K's (table 16-10) */
static const u32 xosc32k_start[8] = { 1, 32, 2048, 4096, 16384, 32768, 65536, 131072 };
static const u32 osc32k_start[8] = { 3, 4, 6, 10, 18, 34, 66, 130 };

static u32 pclksr(struct samd *d)
{
    double t = now_ns(d);
    u32 v = 1u << 3 | 1u << 4;                  /* OSC8MRDY, DFLLRDY */
    if (d->xosc32k_on && t >= d->xosc32k_at)
        v |= 1u << 1;
    if (d->osc32k_on && t >= d->osc32k_at)
        v |= 1u << 2;
    if (d->dfll_closed_on && t >= d->lock_at)
        v |= 1u << 6 | 1u << 7;                 /* DFLLLCKF, DFLLLCKC */
    return v;
}

static void sys_status(struct samd *d)
{
    u32 v = pclksr(d);
    hw(d->sysctrl, S_PCLKSR, v);
    u32 rise = v & ~d->pclksr;
    d->pclksr = v;
    if (rise) {
        hw(d->sysctrl, S_INTFLAG, rd(d->sysctrl, S_INTFLAG) | rise);
        if (rd(d->sysctrl, S_INTENSET) & rise)
            rf_interrupt(d->sysctrl, rf_irq(d->sysctrl, "SYSCTRL"));
    }
}

/* the DFLL in closed loop: it locks once its reference runs */
static void dfll(struct samd *d)
{
    u32 c = rd(d->sysctrl, 0x24);
    int closed = (c >> 1 & 1) && (c >> 2 & 1);
    if (!closed) {
        d->dfll_closed_on = 0;
        return;
    }
    u32 ref = d->clkctrl[0] >> 14 & 1 ? gen_hz(d, (int)(d->clkctrl[0] >> 8 & 15), 1) : 0;
    if (!ref) {
        if (!d->warned_ref) {
            d->warned_ref = 1;
            fflush(stdout);
            fprintf(stderr, "embsim: warning: the DFLL48M is in closed loop and its "
                    "reference, generic clock 0 (GCLK_DFLL48M_REF), is not running: "
                    "it will not lock\n");
        }
        d->dfll_closed_on = 0;
        return;
    }
    if (!d->dfll_closed_on) {
        d->dfll_closed_on = 1;
        d->lock_at = now_ns(d) + 200000.0;
    }
}

static void sys_tick(void *ctx, u32 cycles)
{
    struct samd *d = ctx;
    u32 before = d->pclksr;
    d->dev += cycles;
    if (pclksr(d) != before) {
        sys_status(d);
        clocks(d);
    }
}

static u32 sys_read(void *ctx, u32 off, int n)
{
    struct samd *d = ctx;
    sys_status(d);
    return rf_read(d->sysctrl, off, n);
}

static void sys_write(void *ctx, u32 off, int n, u32 v)
{
    struct samd *d = ctx;
    struct rf_periph *p = d->sysctrl;
    u32 flag = rd(p, S_INTFLAG);
    rf_write(p, off, n, v);
    if (off == S_INTFLAG)                       /* write 1 to clear */
        hw(p, S_INTFLAG, flag & ~(v << 8 * (off & 3)));
    if (off == S_INTENSET || off == S_INTENCLR) {
        struct rf_reg *set = reg(p, S_INTENSET), *clr = reg(p, S_INTENCLR);
        u32 en = (u32)set->value;
        en = off == S_INTENSET ? en | v : (u32)clr->value & ~v;
        set->value = clr->value = en;
    }
    if (off == 0x14) {                          /* XOSC32K */
        int on = (int)(rd(p, 0x14) >> 1 & 1);
        if (on && !d->xosc32k_on)
            d->xosc32k_at = now_ns(d) + 1e9 * (xosc32k_start[rd(p, 0x14) >> 8 & 7] + 3) / OSC32_HZ;
        d->xosc32k_on = on;
    }
    if (off == 0x18) {                          /* OSC32K */
        int on = (int)(rd(p, 0x18) >> 1 & 1);
        if (on && !d->osc32k_on)
            d->osc32k_at = now_ns(d) + 1e9 * (osc32k_start[rd(p, 0x18) >> 8 & 7] + 2) / OSC32_HZ;
        d->osc32k_on = on;
    }
    dfll(d);
    sys_status(d);
    clocks(d);
}

static void sys_reset(void *ctx)
{
    struct samd *d = ctx;
    static const u32 genctrl[NGEN] = { 0x00010600, 1, 0x00010302, 3, 4, 5, 6, 7, 8 };
    for (int g = 0; g < NGEN; g++) {
        d->genctrl[g] = genctrl[g];
        d->gendiv[g] = (u32)g;
        d->eff_src[g] = (int)(genctrl[g] >> 8 & 31);
    }
    memset(d->clkctrl, 0, sizeof d->clkctrl);
    for (int i = 0; i < NCLK; i++)
        d->clkctrl[i] = (u32)i;
    d->clkctrl[3] |= 2u << 8;                   /* GCLK_WDT on generator 2 */
    d->sel_gen = d->sel_div = d->sel_clk = 0;
    d->xosc32k_on = d->osc32k_on = d->dfll_closed_on = 0;
    d->pclksr = 0;
    d->dev = d->at = 0;
    d->ns = 0;
    d->hz = 1000000u;
    d->running = 0;
    rf_clock(d->sysctrl, &d->running, 1);
    sys_status(d);
}

const struct dev_ops samd21_sysctrl_ops = {
    "samd21-sysctrl", sys_read, sys_write, sys_reset, sys_tick, 0,
};

void *samd21_sysctrl_create(struct sim *s, struct rf_periph *p)
{
    struct samd *d = calloc(1, sizeof *d);
    if (!d)
        die("out of memory");
    d->s = s;
    d->sysctrl = p;
    s->now_ns = samd_now_ns;
    sys_reset(d);
    return d;
}

/* ---- GCLK ----------------------------------------------------------------------- */

static u32 gclk_read(void *ctx, u32 off, int n)
{
    struct samd *d = ctx;
    struct rf_periph *p = d->gclk;
    hw(p, 0x2, d->clkctrl[d->sel_clk]);
    hw(p, 0x4, d->genctrl[d->sel_gen]);
    hw(p, 0x8, d->gendiv[d->sel_div]);
    return rf_read(p, off, n);
}

static void gclk_write(void *ctx, u32 off, int n, u32 v)
{
    struct samd *d = ctx;
    struct rf_periph *p = d->gclk;
    if (off == 0x0 && (v & 1)) {                /* CTRL.SWRST */
        sys_reset(d);
        clocks(d);
        return;
    }
    if (off == 0x2 && n == 1) {                 /* CLKCTRL.ID: select */
        d->sel_clk = (int)(v & 0x3f) < NCLK ? (int)(v & 0x3f) : 0;
        return;
    }
    if (off == 0x2 && n >= 2) {
        u32 id = v & 0x3f;
        if (id < NCLK) {
            d->clkctrl[id] = v & 0xcf3f;
            d->sel_clk = (int)id;
        }
    } else if (off == 0x4 && n == 1) {          /* GENCTRL.ID */
        d->sel_gen = (int)(v & 15) < NGEN ? (int)(v & 15) : 0;
        return;
    } else if (off == 0x4 && n == 4) {
        u32 id = v & 15;
        if (id < NGEN && !(id == 1 && (v >> 8 & 31) == 2)) {   /* not itself */
            d->genctrl[id] = v & 0x003f1f0f;
            d->sel_gen = (int)id;
        }
    } else if (off == 0x8 && n == 1) {          /* GENDIV.ID */
        d->sel_div = (int)(v & 15) < NGEN ? (int)(v & 15) : 0;
        return;
    } else if (off == 0x8 && n == 4) {
        u32 id = v & 15;
        if (id < NGEN) {
            d->gendiv[id] = v & 0x00ffff0f;
            d->sel_div = (int)id;
        }
    } else {
        rf_write(p, off, n, v);
    }
    dfll(d);
    sys_status(d);
    clocks(d);
}

const struct dev_ops samd21_gclk_ops = {
    "samd21-gclk", gclk_read, gclk_write, 0, 0, 0,
};

void *samd21_gclk_create(struct sim *s, struct rf_periph *p)
{
    struct samd *d = part_of(s);
    if (!d)
        die("GCLK: the SAM D21's models need SYSCTRL's first");
    d->gclk = p;
    return d;
}

/* ---- PM: the CPU's clock, and the peripherals' APB clocks ----------------------- */

/* the peripherals each mask gates, bit by bit (table 15-1, the masks'
 * registers) */
static const char *const apba[] = { "PAC0", "PM", "SYSCTRL", "GCLK", "WDT", "RTC", "EIC", 0 };
static const char *const apbb[] = { "PAC1", "DSU", "NVMCTRL", "PORT", "DMAC", "USB", "HMATRIX", 0 };
static const char *const apbc[] = { "PAC2", "EVSYS", "SERCOM0", "SERCOM1", "SERCOM2", "SERCOM3",
                                    "SERCOM4", "SERCOM5", "TCC0", "TCC1", "TCC2", "TC3", "TC4",
                                    "TC5", "TC6", "TC7", "ADC", "AC", "DAC", "PTC", "I2S", 0 };

static void gate(struct samd *d, u32 off, const char *mask, const char *const *names)
{
    struct rf_reg *r = reg(d->pm, off);
    for (int b = 0; names[b] && r; b++) {
        struct rf_periph *q = rf_periph(d->s->svd, names[b]);
        if (!q || q == d->pm)               /* PM's own mask cannot stop PM */
            continue;
        size_t l = strlen(mask) + strlen(names[b]) + 8;
        char *g = malloc(l);
        if (!g)
            die("out of memory");
        snprintf(g, l, "PM.%s.%s", mask, names[b]);
        q->gate = r;
        q->gate_mask = 1ull << b;
        q->gate_name = g;
    }
}

static u32 pm_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct samd *)ctx)->pm, off, n);
}

static void pm_write(void *ctx, u32 off, int n, u32 v)
{
    struct samd *d = ctx;
    struct rf_periph *p = d->pm;
    u32 flag = rd(p, 0x36);
    rf_write(p, off, n, v);
    if (off <= 0xb && off + (u32)n > 0x8)       /* CPUSEL, APBxSEL: CKRDY */
        hw(p, 0x36, flag | 1);
    if (off == 0x36)
        hw(p, 0x36, flag & ~v);
    if (off == 0x34 || off == 0x35) {           /* INTENCLR, INTENSET */
        u32 en = rd(p, 0x35);
        en = off == 0x35 ? en | v : en & ~v;
        hw(p, 0x34, en);
        hw(p, 0x35, en);
    }
    clocks(d);
}

const struct dev_ops samd21_pm_ops = {
    "samd21-pm", pm_read, pm_write, 0, 0, 0,
};

void *samd21_pm_create(struct sim *s, struct rf_periph *p)
{
    struct samd *d = part_of(s);
    if (!d)
        die("PM: the SAM D21's models need SYSCTRL's first");
    d->pm = p;
    gate(d, 0x18, "APBAMASK", apba);
    gate(d, 0x1c, "APBBMASK", apbb);
    gate(d, 0x20, "APBCMASK", apbc);
    return d;
}

/* ---- NVMCTRL ---------------------------------------------------------------------- */

static u32 nvm_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct samd *)ctx)->nvm, off, n);
}

static void nvm_write(void *ctx, u32 off, int n, u32 v)
{
    struct samd *d = ctx;
    rf_write(d->nvm, off, n, v);
    if (off == 0x4)
        check_rws(d);
}

const struct dev_ops samd21_nvmctrl_ops = {
    "samd21-nvmctrl", nvm_read, nvm_write, 0, 0, 0,
};

void *samd21_nvmctrl_create(struct sim *s, struct rf_periph *p)
{
    struct samd *d = part_of(s);
    if (!d)
        die("NVMCTRL: the SAM D21's models need SYSCTRL's first");
    d->nvm = p;
    struct rf_reg *r = reg(p, 0x14);            /* INTFLAG.READY: idle */
    if (r) {
        rf_set_reset(r, 1);
        r->value = 1;
    }
    r = reg(p, 0x8);                            /* PARAM: PSZ 64 bytes, 4096 pages */
    if (r) {
        rf_set_reset(r, 0x00031000);
        r->value = 0x00031000;
    }
    return d;
}

/* ---- PORT ------------------------------------------------------------------------- */

#define NGROUP 2

struct port {
    struct sim *s;
    struct rf_periph *p;
    int out[NGROUP][32];
    int known;
};

static struct port *port_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "PORT") : 0;
    return p && p->ops == &samd21_port_ops ? p->ctx : 0;
}

static void port_update(struct port *pt)
{
    struct sim *s = pt->s;
    for (int g = 0; g < NGROUP; g++) {
        u32 base = 0x80u * (u32)g, dir = rd(pt->p, base), out = rd(pt->p, base + 0x10), in = 0;
        for (int i = 0; i < 32; i++) {
            u32 cfg = rd(pt->p, base + 0x40 + (u32)i);
            int o = -1, lvl = 0, ext = 0;
            if ((dir >> i & 1) && !(cfg & 1))   /* DIR, not PMUXEN */
                o = (int)(out >> i & 1);
            if (o >= 0)
                lvl = o;
            else if (s->io.pin_in && s->io.pin_in(s->io.ctx, g, i, &ext))
                lvl = ext != 0;
            else if (cfg >> 2 & 1)              /* PULLEN: OUT's way */
                lvl = (int)(out >> i & 1);
            if (cfg >> 1 & 1)                   /* INEN */
                in |= (u32)lvl << i;
            if (s->io.pin_out && (!pt->known || pt->out[g][i] != o))
                s->io.pin_out(s->io.ctx, g, i, o);
            pt->out[g][i] = o;
        }
        hw(pt->p, base + 0x20, in);
    }
    pt->known = 1;
}

static u32 port_read(void *ctx, u32 off, int n)
{
    struct port *pt = ctx;
    if ((off & 0x7f) >= 0x20 && (off & 0x7f) < 0x24)
        port_update(pt);
    return rf_read(pt->p, off, n);
}

/* WRCONFIG: PINMASK's pins (the group's upper half with HWSEL) take its
 * PINCFG bits with WRPINCFG, its PMUX with WRPMUX (22.8.10) */
static void wrconfig(struct port *pt, u32 base, u32 v)
{
    int hi = (int)(v >> 31 & 1);
    for (int i = 0; i < 16; i++) {
        if (!(v >> i & 1))
            continue;
        u32 pin = (u32)(i + (hi ? 16 : 0));
        if (v >> 30 & 1) {                      /* WRPINCFG */
            u32 cfg = (v >> 16 & 1) | (v >> 17 & 1) << 1 | (v >> 18 & 1) << 2 | (v >> 22 & 1) << 6;
            hw(pt->p, base + 0x40 + pin, cfg);
        }
        if (v >> 28 & 1) {                      /* WRPMUX */
            u32 a = base + 0x30 + pin / 2, m = rd(pt->p, a), f = v >> 24 & 15;
            m = pin & 1 ? (m & 0x0f) | f << 4 : (m & 0xf0) | f;
            hw(pt->p, a, m);
        }
    }
}

static void port_write(void *ctx, u32 off, int n, u32 v)
{
    struct port *pt = ctx;
    u32 base = off & ~0x7fu, r = off & 0x7f;
    if (r < 0x20 && n < 4)
        v <<= 8 * (r & 3);                      /* a byte or halfword's lanes */
    u32 dir = rd(pt->p, base), out = rd(pt->p, base + 0x10);
    switch (r & ~3u) {
    case 0x04: hw(pt->p, base, dir & ~v); break;
    case 0x08: hw(pt->p, base, dir | v); break;
    case 0x0c: hw(pt->p, base, dir ^ v); break;
    case 0x14: hw(pt->p, base + 0x10, out & ~v); break;
    case 0x18: hw(pt->p, base + 0x10, out | v); break;
    case 0x1c: hw(pt->p, base + 0x10, out ^ v); break;
    case 0x28: wrconfig(pt, base, v); break;
    default:
        rf_write(pt->p, off, n, r < 0x20 && n < 4 ? v >> 8 * (r & 3) : v);
    }
    port_update(pt);
}

static void port_reset(void *ctx)
{
    struct port *pt = ctx;
    pt->known = 0;
    port_update(pt);
}

const struct dev_ops samd21_port_ops = {
    "samd21-port", port_read, port_write, port_reset, 0, 0,
};

/* PORT_IOBUS: the same registers, at 0x60000000 (22.5.10) */
static u32 iobus_read(void *ctx, u32 off, int n)
{
    struct port *pt = ctx;
    return port_read(pt, off, n);
}

static void iobus_write(void *ctx, u32 off, int n, u32 v)
{
    port_write(ctx, off, n, v);
}

const struct dev_ops samd21_iobus_ops = {
    "samd21-iobus", iobus_read, iobus_write, 0, 0, 0,
};

void *samd21_port_create(struct sim *s, struct rf_periph *p)
{
    if (!strcmp(p->name, "PORT_IOBUS")) {
        struct port *pt = port_of(s);
        if (!pt)
            die("PORT_IOBUS: the SAM D21's PORT model comes first");
        p->ops = &samd21_iobus_ops;
        return pt;
    }
    struct port *pt = calloc(1, sizeof *pt);
    if (!pt)
        die("out of memory");
    pt->s = s;
    pt->p = p;
    return pt;
}

/* the board drove a pin or let it go */
void samd21_pins_changed(struct sim *s)
{
    struct port *pt = port_of(s);
    if (pt)
        port_update(pt);
}

/* ---- SERCOM as a USART -------------------------------------------------------------- */

#define U_CTRLA 0x00u
#define U_CTRLB 0x04u
#define U_BAUD 0x0cu
#define U_INTENCLR 0x14u
#define U_INTENSET 0x16u
#define U_INTFLAG 0x18u
#define U_STATUS 0x1au
#define U_SYNCBUSY 0x1cu
#define U_DATA 0x28u
#define F_DRE 0x01u
#define F_TXC 0x02u
#define F_RXC 0x04u

struct sercom {
    struct sim *s;
    struct rf_periph *p;
    int n, irq;
    u8 r[0x40];                     /* the registers, as the USART has them */
    u16 rx[2];
    int nrx, line;
};

static int usart(const struct sercom *u)
{
    return (u->r[U_CTRLA] >> 2 & 7) <= 1;       /* MODE 0 or 1 */
}

static int enabled(const struct sercom *u)
{
    return u->r[U_CTRLA] >> 1 & 1;
}

static void u_irq(struct sercom *u)
{
    int line = (u->r[U_INTFLAG] & u->r[U_INTENSET]) != 0;
    if (line && !u->line && u->irq >= 0)
        rf_interrupt(u->p, u->irq);
    u->line = line;
}

/* the flags that follow the buffers: DRE while the transmitter can take
 * a byte, RXC while the receiver holds one */
static void u_flags(struct sercom *u)
{
    u32 ctrlb = get_bytes(u->r, U_CTRLB, 4);
    u8 f = u->r[U_INTFLAG] & (u8)~(F_DRE | F_RXC);
    if (enabled(u) && usart(u) && (ctrlb >> 16 & 1))
        f |= F_DRE;
    if (u->nrx)
        f |= F_RXC;
    u->r[U_INTFLAG] = f;
    put_bytes(u->r, U_DATA, 2, u->nrx ? u->rx[0] : 0);
    u_irq(u);
}

/* the generic clock a SERCOM's core runs on: GCLK_SERCOMn_CORE */
static int u_clocked(struct sercom *u)
{
    return samd21_gclk_hz(u->s, 0x14 + u->n) != 0;
}

static int u_room(void *ctx)
{
    struct sercom *u = ctx;
    return enabled(u) && usart(u) && (get_bytes(u->r, U_CTRLB, 4) >> 17 & 1) && u_clocked(u) &&
           !rf_gated(u->p);
}

static void u_put(void *ctx, int c)
{
    struct sercom *u = ctx;
    if (u->nrx == 2) {
        u->r[U_STATUS] |= 1u << 2;              /* BUFOVF */
        u->r[U_INTFLAG] |= 0x80;                /* ERROR */
    } else {
        u->rx[u->nrx++] = (u16)(c & 0xff);
    }
    u_flags(u);
}

static u32 u_read(void *ctx, u32 off, int n)
{
    struct sercom *u = ctx;
    if (off + (u32)n > sizeof u->r)
        return 0;
    if (off == U_DATA && usart(u)) {
        u32 v = u->nrx ? u->rx[0] : 0;
        if (u->nrx && !u->s->bus.debug) {
            u->rx[0] = u->rx[1];
            u->nrx--;
            u_flags(u);
        }
        return v;
    }
    if (off >= U_INTENCLR && off < U_INTENCLR + 2)
        return get_bytes(u->r, U_INTENSET + (off - U_INTENCLR), n);
    return get_bytes(u->r, off, n);
}

static void u_reset_regs(struct sercom *u)
{
    memset(u->r, 0, sizeof u->r);
    u->nrx = 0;
}

static void u_write(void *ctx, u32 off, int n, u32 v)
{
    struct sercom *u = ctx;
    if (off + (u32)n > sizeof u->r)
        return;
    int en = enabled(u);
    for (int i = 0; i < n; i++) {
        u32 a = off + (u32)i;
        u8 b = (u8)(v >> (8 * i)), old = u->r[a];
        if (a == U_CTRLA) {                     /* SWRST; ENABLE alone while enabled */
            if (b & 1) {
                u_reset_regs(u);
                u_flags(u);
                return;
            }
            u->r[a] = en ? (u8)((old & ~2u) | (b & 2u)) : b;
        } else if (a < 4) {
            if (!en)
                u->r[a] = b;
        } else if (a >= U_CTRLB && a < U_CTRLB + 4) {
            /* enable-protected but RXEN and TXEN (bits 17, 16) */
            u8 keep = a == U_CTRLB + 2 ? 0x03 : 0x00;
            u->r[a] = en ? (u8)((old & ~keep) | (b & keep)) : b;
            if (a == U_CTRLB + 2 && !(u->r[a] >> 1 & 1)) {
                u->nrx = 0;                     /* RXEN off flushes the buffer */
                u->r[U_STATUS] &= (u8)~0x07;
            }
        } else if (a == U_BAUD || a == U_BAUD + 1) {
            if (!en)
                u->r[a] = b;
        } else if (a == U_INTENCLR || a == U_INTENCLR + 1) {
            u->r[U_INTENSET + (a - U_INTENCLR)] &= (u8)~b;
        } else if (a == U_INTENSET || a == U_INTENSET + 1) {
            u->r[a] |= b;
        } else if (a == U_INTFLAG) {
            u->r[a] &= (u8)~(b & 0xba);         /* TXC, RXS, CTSIC, RXBRK, ERROR */
        } else if (a == U_STATUS) {
            u->r[a] &= (u8)~(b & 0x37);         /* PERR, FERR, BUFOVF, ISF, COLL */
        } else if (a == U_DATA || a == U_DATA + 1) {
            /* below */
        } else if (a >= U_SYNCBUSY && a < U_SYNCBUSY + 4) {
            /* read-only */
        } else {
            u->r[a] = b;
        }
    }
    if (off == U_DATA && usart(u)) {
        u32 ctrlb = get_bytes(u->r, U_CTRLB, 4);
        if (enabled(u) && (ctrlb >> 16 & 1) && u_clocked(u)) {
            sim_out(u->s, (int)(v & 0xff));
            u->r[U_INTFLAG] |= F_TXC;
        }
    }
    if (usart(u) && enabled(u) && (get_bytes(u->r, U_CTRLB, 4) >> 17 & 1))
        sim_rx_port(u->s, u_room, u_put, u);
    u_flags(u);
}

static void u_reset(void *ctx)
{
    struct sercom *u = ctx;
    u_reset_regs(u);
    u->line = 0;
    u_flags(u);
}

const struct dev_ops samd21_sercom_ops = {
    "samd21-sercom", u_read, u_write, u_reset, 0, 0,
};

void *samd21_sercom_create(struct sim *s, struct rf_periph *p)
{
    if (strncmp(p->name, "SERCOM", 6) || p->name[6] < '0' || p->name[6] > '5' || p->name[7])
        return 0;
    struct sercom *u = calloc(1, sizeof *u);
    if (!u)
        die("out of memory");
    u->s = s;
    u->p = p;
    u->n = p->name[6] - '0';
    u->irq = rf_irq(p, p->name);
    u_reset(u);
    return u;
}

/* ---- TC3-5 -------------------------------------------------------------------------- */

#define T_CTRLA 0x00u
#define T_READREQ 0x02u
#define T_CTRLBCLR 0x04u
#define T_CTRLBSET 0x05u
#define T_INTENCLR 0x0cu
#define T_INTENSET 0x0du
#define T_INTFLAG 0x0eu
#define T_STATUS 0x0fu
#define T_COUNT 0x10u
#define T_PER 0x14u
#define T_CC 0x18u
#define F_OVF 0x01u
#define F_MC0 0x10u

struct tc {
    struct sim *s;
    struct rf_periph *p;
    int n, irq, gclk;
    u8 r[0x20];
    u32 count, cc[2], per;
    u32 synced;                     /* COUNT as last read-synchronised */
    u8 ctrlb;
    int stopped, line, running;
    u64 acc;                        /* cycles x f_cnt toward a count */
    u32 hz_cpu, hz_cnt;
};

static struct tc *tc_of(struct sim *s, int n)
{
    char nm[8];
    snprintf(nm, sizeof nm, "TC%d", n);
    struct rf_periph *p = s->svd ? rf_periph(s->svd, nm) : 0;
    return p && p->ops == &samd21_tc_ops ? p->ctx : 0;
}

static int t_mode(const struct tc *t)
{
    return t->r[T_CTRLA] >> 2 & 3;              /* 0 COUNT16, 1 COUNT8, 2 COUNT32 */
}

/* TC5 is TC4's slave while TC4 counts in 32 bits */
static int t_slave(const struct tc *t)
{
    struct tc *m = t->n == 5 ? tc_of(t->s, 4) : 0;
    return m && t_mode(m) == 2;
}

static u32 t_max(const struct tc *t)
{
    int m = t_mode(t);
    return m == 1 ? 0xffu : m == 2 ? 0xffffffffu : 0xffffu;
}

static u32 t_top(const struct tc *t)
{
    int wg = t->r[T_CTRLA] >> 5 & 3;
    if (wg == 1 || wg == 3)                     /* MFRQ, MPWM: CC0 */
        return t->cc[0] & t_max(t);
    return t_mode(t) == 1 ? t->per & 0xff : t_max(t);
}

static void t_irq(struct tc *t)
{
    int line = (t->r[T_INTFLAG] & t->r[T_INTENSET]) != 0;
    if (line && !t->line && t->irq >= 0)
        rf_interrupt(t->p, t->irq);
    t->line = line;
}

static int t_counting(struct tc *t)
{
    return (t->r[T_CTRLA] >> 1 & 1) && !t->stopped && !t_slave(t) && t->hz_cnt;
}

static void t_status(struct tc *t)
{
    t->r[T_STATUS] = (u8)((t->stopped || !(t->r[T_CTRLA] >> 1 & 1)) << 3 | t_slave(t) << 4);
}

static const u32 presc[8] = { 1, 2, 4, 8, 16, 64, 256, 1024 };

static void t_rates(struct tc *t)
{
    u32 cpu = samd21_cpu_hz(t->s);
    u32 cnt = samd21_gclk_hz(t->s, t->gclk) / presc[t->r[T_CTRLA + 1] & 7];
    if (t->hz_cpu && cpu != t->hz_cpu)
        t->acc = t->acc * cpu / t->hz_cpu;
    if (cnt != t->hz_cnt)
        t->acc = 0;
    t->hz_cpu = cpu;
    t->hz_cnt = cnt;
    rf_clock(t->p, &t->running, t_counting(t));
}

/* n counts: wraps with OVF (and a stop in one-shot), MCx as the counter
 * leaves CCx */
static void t_advance(struct tc *t, u64 n)
{
    int down = t->ctrlb & 1;
    while (n && !t->stopped) {
        u32 top = t_top(t), c = t->count;
        u64 room = down ? c : (u64)top - c;     /* counts before the wrap */
        u64 k = n <= room ? n : room;
        /* the CCx the counter leaves in these k counts */
        for (int x = 0; x < 2; x++) {
            u32 v = t->cc[x] & t_max(t);
            int hit = down ? (v <= c && v > c - k) : (v >= c && v < c + k);
            if (k && hit)
                t->r[T_INTFLAG] |= (u8)(F_MC0 << x);
        }
        c = down ? c - (u32)k : c + (u32)k;
        n -= k;
        t->count = c;
        if (!n)
            break;
        /* the wrap: it leaves the top (or zero) too */
        for (int x = 0; x < 2; x++)
            if ((t->cc[x] & t_max(t)) == c)
                t->r[T_INTFLAG] |= (u8)(F_MC0 << x);
        t->count = down ? top : 0;
        t->r[T_INTFLAG] |= F_OVF;
        n--;
        if (t->ctrlb >> 2 & 1) {                /* ONESHOT */
            t->stopped = 1;
            t_status(t);
        }
    }
}

static void t_tick(void *ctx, u32 cycles)
{
    struct tc *t = ctx;
    if (!t_counting(t) || !t->hz_cpu)
        return;
    t->acc += (u64)cycles * t->hz_cnt;
    u64 n = t->acc / t->hz_cpu;
    t->acc %= t->hz_cpu;
    if (!n)
        return;
    t_advance(t, n);
    if (t->r[T_READREQ + 1] >> 6 & 1)           /* RCONT */
        t->synced = t->count;
    t_irq(t);
    t_rates(t);
}

/* the cycles to the next flag an enabled interrupt waits for */
static int t_next(void *ctx, u32 *cycles)
{
    struct tc *t = ctx;
    if (!t_counting(t) || !t->hz_cpu)
        return 0;
    u32 top = t_top(t), c = t->count;
    int down = t->ctrlb & 1;
    u64 best = 0;
    int any = 0;
    if (t->r[T_INTENSET] & F_OVF) {
        best = (down ? c : (u64)top - c) + 1;
        any = 1;
    }
    for (int x = 0; x < 2; x++) {
        if (!(t->r[T_INTENSET] & (F_MC0 << x)))
            continue;
        u32 v = t->cc[x] & t_max(t);
        if (v > top)
            continue;
        u64 d = down ? (v <= c ? (u64)c - v : (u64)c + 1 + top - v) + 1
                     : (v >= c ? (u64)v - c : (u64)top - c + 1 + v) + 1;
        if (!any || d < best)
            best = d;
        any = 1;
    }
    if (!any)
        return 0;
    u64 need = best * t->hz_cpu;
    u64 cy = need > t->acc ? (need - t->acc + t->hz_cnt - 1) / t->hz_cnt : 1;
    *cycles = cy > 0xffffffffu ? 0xffffffffu : (u32)cy;
    return 1;
}

static u32 t_read(void *ctx, u32 off, int n)
{
    struct tc *t = ctx;
    if (off + (u32)n > sizeof t->r)
        return 0;
    t_status(t);
    u32 m = t_mode(t);
    if (t_slave(t))
        return 0;                               /* "not allowed" (29.6.2.4) */
    /* COUNT, PER, CCx in the mode's widths */
    if (m == 1) {
        t->r[T_COUNT] = (u8)t->synced;
        t->r[T_PER] = (u8)t->per;
        t->r[T_CC] = (u8)t->cc[0];
        t->r[T_CC + 1] = (u8)t->cc[1];
    } else if (m == 0) {
        put_bytes(t->r, T_COUNT, 2, t->synced);
        put_bytes(t->r, T_CC, 2, t->cc[0]);
        put_bytes(t->r, T_CC + 2, 2, t->cc[1]);
    } else {
        put_bytes(t->r, T_COUNT, 4, t->synced);
        put_bytes(t->r, T_CC, 4, t->cc[0]);
        put_bytes(t->r, T_CC + 4, 4, t->cc[1]);
    }
    t->r[T_CTRLBCLR] = t->r[T_CTRLBSET] = t->ctrlb;
    if (off >= T_INTENCLR && off < T_INTENCLR + 1)
        return t->r[T_INTENSET];
    return get_bytes(t->r, off, n);
}

static void t_reset(void *ctx);

static void t_write(void *ctx, u32 off, int n, u32 v)
{
    struct tc *t = ctx;
    if (off + (u32)n > sizeof t->r || t_slave(t))
        return;
    u32 m = t_mode(t);
    int en = t->r[T_CTRLA] >> 1 & 1;
    for (int i = 0; i < n; i++) {
        u32 a = off + (u32)i;
        u8 b = (u8)(v >> (8 * i));
        switch (a) {
        case T_CTRLA:
            if (b & 1) {                        /* SWRST */
                t_reset(t);
                return;
            }
            /* enable-protected: written with ENABLE set, not while it is */
            t->r[a] = en ? (u8)((t->r[a] & ~2u) | (b & 2u)) : b;
            if ((b >> 1 & 1) && !en)
                t->stopped = 0;                 /* enabled: it counts */
            if (!(b >> 1 & 1))
                t->stopped = 1;
            break;
        case T_CTRLA + 1:                       /* RUNSTDBY is not protected */
            t->r[a] = en ? (u8)((t->r[a] & ~0x08u) | (b & 0x08u)) : b;
            break;
        case T_READREQ + 1:
            t->r[a] = b & 0x40;                 /* RCONT kept, RREQ done */
            if (b >> 7 & 1)
                t->synced = t->count;           /* RREQ: COUNT synchronised */
            break;
        case T_CTRLBCLR:
        case T_CTRLBSET: {
            u8 bits = b & 0x05;
            t->ctrlb = a == T_CTRLBSET ? t->ctrlb | bits : t->ctrlb & (u8)~bits;
            if (a == T_CTRLBSET && (b >> 6) == 1) {         /* RETRIGGER */
                t->count = t->ctrlb & 1 ? t_top(t) : 0;
                t->stopped = 0;
            } else if (a == T_CTRLBSET && (b >> 6) == 2) {  /* STOP */
                t->stopped = 1;
            }
            break;
        }
        case T_INTENCLR:
            t->r[T_INTENSET] &= (u8)~b;
            break;
        case T_INTENSET:
            t->r[T_INTENSET] |= b;
            break;
        case T_INTFLAG:
            t->r[T_INTFLAG] &= (u8)~b;
            break;
        case T_STATUS:
            break;
        default:
            if (a < T_COUNT)
                t->r[a] = b;
        }
    }
    /* COUNT, PER and the CCs, as wide as the mode's */
    if (off >= T_COUNT) {
        u32 w = m == 1 ? 1 : m == 0 ? 2 : 4;
        for (int i = 0; i < n; i++) {
            u32 a = off + (u32)i;
            u8 b = (u8)(v >> (8 * i));
            u32 *dst = 0, at = 0;
            if (a >= T_COUNT && a < T_COUNT + w)
                dst = &t->count, at = a - T_COUNT;
            else if (m == 1 && a == T_PER)
                dst = &t->per, at = 0;
            else if (a >= T_CC && a < T_CC + w)
                dst = &t->cc[0], at = a - T_CC;
            else if (a >= T_CC + w && a < T_CC + 2 * w)
                dst = &t->cc[1], at = a - T_CC - w;
            if (dst)
                *dst = (*dst & ~(0xffu << 8 * at)) | (u32)b << 8 * at;
        }
    }
    t_status(t);
    t_rates(t);
    t_irq(t);
}

static void t_reset(void *ctx)
{
    struct tc *t = ctx;
    memset(t->r, 0, sizeof t->r);
    t->count = t->synced = 0;
    t->cc[0] = t->cc[1] = 0;
    t->per = 0xff;
    t->ctrlb = 0;
    t->stopped = 1;
    t->line = 0;
    t->acc = 0;
    t->hz_cpu = t->hz_cnt = 0;
    t_status(t);
    t_rates(t);
}

const struct dev_ops samd21_tc_ops = {
    "samd21-tc", t_read, t_write, t_reset, t_tick, t_next,
};

void *samd21_tc_create(struct sim *s, struct rf_periph *p)
{
    if (strncmp(p->name, "TC", 2) || p->name[2] < '3' || p->name[2] > '5' || p->name[3])
        return 0;
    struct tc *t = calloc(1, sizeof *t);
    if (!t)
        die("out of memory");
    t->s = s;
    t->p = p;
    t->n = p->name[2] - '0';
    t->irq = rf_irq(p, p->name);
    t->gclk = t->n == 3 ? 0x1b : 0x1c;          /* GCLK_TCC2_TC3, GCLK_TC4_TC5 */
    t_reset(t);
    return t;
}

/* the clocks changed: the TCs' rates follow */
void samd21_clocks_changed(struct sim *s)
{
    for (int n = 3; n <= 5; n++) {
        struct tc *t = tc_of(s, n);
        if (t)
            t_rates(t);
    }
}
