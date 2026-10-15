/* rp2040.c -- the RP2040's boot, its resets, its clocks and its watchdog,
 * over RP2040.svd's registers (rp2040.h says what the models share).
 *
 * The boot. EmbSim does not run the bootrom's code: its ROM (rp2040-rom,
 * at 0) does what the bootrom does to boot from flash (the datasheet's
 * 2.8.1), and then hands over as it does. The first 256 bytes of flash
 * are boot2: their CRC-32 (polynomial 0x04c11db7, seed 0xffffffff, not
 * reflected, no final XOR) over the first 252 must be the last word, or
 * the part would go on to its USB boot -- the run ends there, saying so.
 * A good boot2 is copied to the top 256 bytes of SRAM (0x20041f00) and
 * entered at 0x20041f01 with lr 0, with the peripherals as the bootrom
 * leaves them: PADS_BANK0, IO_QSPI and PADS_QSPI out of reset, GPIO26-29's
 * input enables off, the rest in reset. The ROM's vector table gives the
 * bootrom's stack top (0x20041f00), its header the bootrom's magic and
 * version (B2's, 3), and its table lookup finds no function: the
 * bootrom's functions (flash programming, the float library, memcpy,
 * reset_usb_boot) are not there.
 *
 * RESETS. RESET's bits hold their peripherals in reset (each field is
 * named for its peripheral; USBCTRL is USB's): while held, the
 * peripheral's registers read their reset values and ignore writes, with
 * a warning, as svd-map.c's gates do; a peripheral put into reset has its
 * registers and its model reset. RESET_DONE is ~RESET at once.
 *
 * The clocks. XOSC is the Pico's 12 MHz crystal: enabled (CTRL.ENABLE
 * anything but DISABLE, 0xd1e, as the datasheet says), it is STABLE once
 * STARTUP.DELAY x 256 (x 4 with X4) of its cycles have passed; COUNT
 * counts down at its frequency. ROSC runs at 6.5 MHz. A PLL runs when
 * PWR's PD and VCOPD are clear, at XOSC / REFDIV x FBDIV / (POSTDIV1 x
 * POSTDIV2) once POSTDIVPD is clear too, and is LOCKed at once when its
 * feedback divider is in range (16-320). The clock generators: clk_ref and
 * clk_sys switch glitchlessly (SELECTED is one-hot of SRC at once), the
 * others run while ENABLE is set; each divides by DIV (INT and FRAC,
 * 0 for 2^16), and FC0 measures any of them at once. clk_sys is the
 * core's clock: the board's time (rp2040_now_ns) is its cycles at it.
 *
 * The watchdog. TICK's generator divides clk_ref by CYCLES into the
 * microsecond tick the timer counts (rp2040-timer.c); its counter, loaded
 * by LOAD, goes down by two each tick while CTRL.ENABLE (the RP2040-E1
 * erratum), and at zero -- or on CTRL.TRIGGER -- resets the chip, which
 * ends the run (EmbSim does not boot again). SCRATCH0-7 keep what is
 * written.
 *
 * And where RP2040.svd's reset values are not the part's: SYSINFO's
 * CHIP_ID (0x20002927, a B2) and PLATFORM (ASIC); XOSC's CTRL (0x00d1eaa0:
 * DISABLE, 1-15 MHz, as the datasheet has it); ROSC's STATUS (running);
 * PSM's DONE (every domain powered, less those FRCE_OFF holds off);
 * CHIP_RESET's HAD_POR.
 *
 * Not modelled: the ROSC's frequency controls (FREQA, FREQB, DIV), the
 * PLLs' lock time, the clock generators' stop and start cycles, GPOUT0-3,
 * GPIN, the resus, DORMANT and SLEEP, the watchdog's PSM and RESETS
 * selections (WDSEL) and the reboot itself, core 1 (it never leaves the
 * bootrom's wait for its vector). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"
#include "svd-map.h"

static struct rf_periph *periph(struct sim *s, const char *name)
{
    return s->svd ? rf_periph(s->svd, name) : 0;
}

/* a model's context, when the periph has that model */
static void *model(struct sim *s, const char *name, const struct dev_ops *ops)
{
    struct rf_periph *p = periph(s, name);
    return p && p->ops == ops ? p->ctx : 0;
}

static struct rf_reg *need(struct rf_periph *p, const char *name)
{
    struct rf_reg *r = rf_reg(p, name);
    if (!r)
        die("%s: %s has no register %s", rf_device_name(p->sim->svd), p->name, name);
    return r;
}

/* the field's value, by its mask */
static u32 fld(const struct rf_reg *r, u64 mask)
{
    if (!mask)
        return 0;
    int sh = 0;
    while (!(mask >> sh & 1))
        sh++;
    return (u32)((r->value & mask) >> sh);
}

static u64 put_fld(u64 v, u64 mask, u32 x)
{
    int sh = 0;
    while (mask && !(mask >> sh & 1))
        sh++;
    return (v & ~mask) | (((u64)x << sh) & mask);
}

/* ---- XOSC ------------------------------------------------------------------ */

#define XOSC_DISABLE 0xd1eu
#define XOSC_ENABLE 0xfabu

struct xosc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *ctrl, *status, *startup, *count;
    u64 m_enable, m_stable, m_enabled, m_badwrite, m_delay, m_x4;
    u64 stable_at;                  /* the cycle STABLE sets, once enabled */
    u64 count_at;                   /* COUNT was written at this cycle */
    u32 count_from;
};

static int xosc_on(struct xosc *x)
{
    return fld(x->ctrl, x->m_enable) != XOSC_DISABLE;
}

static int xosc_stable(struct xosc *x)
{
    return xosc_on(x) && rp2040_cycles(x->s) >= x->stable_at;
}

/* crystal cycles to core cycles, at the clk_sys now */
static u64 xtal_to_cycles(struct sim *s, u64 n)
{
    u64 hz = rp2040_hz(s, RP_CLK_SYS);
    return (n * hz + RP_XOSC_HZ - 1) / RP_XOSC_HZ;
}

static void xosc_status(struct xosc *x)
{
    u64 v = x->status->value & ~(x->m_stable | x->m_enabled);
    if (xosc_on(x))
        v |= x->m_enabled;
    if (xosc_stable(x))
        v |= x->m_stable;
    if (v != x->status->value)
        rf_hw(x->status, v);
}

static u32 xosc_read(void *ctx, u32 off, int n)
{
    struct xosc *x = ctx;
    if (off == rf_off(x->status))
        xosc_status(x);
    if (off == rf_off(x->count) && !x->s->bus.debug) {
        /* it counts down at the crystal's frequency */
        u64 hz = rp2040_hz(x->s, RP_CLK_SYS), gone = 0;
        if (hz && xosc_stable(x))
            gone = (rp2040_cycles(x->s) - x->count_at) * RP_XOSC_HZ / hz;
        x->count->value = gone >= x->count_from ? 0 : x->count_from - gone;
    }
    return rf_read(x->p, off, n);
}

static void xosc_write(void *ctx, u32 off, int n, u32 v)
{
    struct xosc *x = ctx;
    int was = xosc_on(x);
    rf_write(x->p, off, n, v);
    if (off == rf_off(x->ctrl)) {
        u32 e = fld(x->ctrl, x->m_enable);
        if (e != XOSC_ENABLE && e != XOSC_DISABLE)
            rf_hw(x->status, x->status->value | x->m_badwrite);
        /* the range resets to 1-15 MHz and cannot be changed */
        rf_hw(x->ctrl, (x->ctrl->value & x->m_enable) | 0xaa0u);
        if (!was && xosc_on(x)) {
            u64 d = (u64)fld(x->startup, x->m_delay) * 256;
            if (fld(x->startup, x->m_x4))
                d *= 4;
            x->stable_at = rp2040_cycles(x->s) + xtal_to_cycles(x->s, d);
        }
        rp2040_clock_change(x->s);
    }
    if (off == rf_off(x->count)) {
        x->count_at = rp2040_cycles(x->s);
        x->count_from = (u32)x->count->value;
    }
    xosc_status(x);
}

static void xosc_reset(void *ctx)
{
    struct xosc *x = ctx;
    x->stable_at = 0;
    x->count_from = 0;
    xosc_status(x);
}

const struct dev_ops rp2040_xosc_ops = {
    "rp2040-xosc", xosc_read, xosc_write, xosc_reset, 0, 0,
};

void *rp2040_xosc_create(struct sim *s, struct rf_periph *p)
{
    struct xosc *x = calloc(1, sizeof *x);
    if (!x)
        die("out of memory");
    x->s = s;
    x->p = p;
    x->ctrl = need(p, "CTRL");
    x->status = need(p, "STATUS");
    x->startup = need(p, "STARTUP");
    x->count = need(p, "COUNT");
    x->m_enable = rf_mask(x->ctrl, "ENABLE");
    x->m_stable = rf_mask(x->status, "STABLE");
    x->m_enabled = rf_mask(x->status, "ENABLED");
    x->m_badwrite = rf_mask(x->status, "BADWRITE");
    x->m_delay = rf_mask(x->startup, "DELAY");
    x->m_x4 = rf_mask(x->startup, "X4");
    /* CTRL out of reset: DISABLE, 1-15 MHz (the datasheet's; the file
     * says 0) */
    rf_set_reset(x->ctrl, put_fld(0xaa0u, x->m_enable, XOSC_DISABLE));
    return x;
}

/* ---- the PLLs ---------------------------------------------------------------- */

static u32 xosc_hz(struct sim *s);

struct pll {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *cs, *pwr, *fbdiv, *prim;
    u64 m_lock, m_bypass, m_refdiv, m_pd, m_vcopd, m_postdivpd, m_fbdiv, m_pd1, m_pd2;
};

static int pll_running(const struct pll *q)
{
    u32 fb = fld(q->fbdiv, q->m_fbdiv), ref = fld(q->cs, q->m_refdiv);
    return !fld(q->pwr, q->m_pd) && !fld(q->pwr, q->m_vcopd) && ref &&
           fb >= 16 && fb <= 320 && xosc_hz(q->s);
}

/* its output, in Hz */
static u32 pll_hz(const struct pll *q)
{
    if (!q)
        return 0;
    u64 xosc = xosc_hz(q->s);
    u32 ref = fld(q->cs, q->m_refdiv);
    if (fld(q->cs, q->m_bypass))
        return ref ? (u32)(xosc / ref) : 0;
    if (!pll_running(q) || fld(q->pwr, q->m_postdivpd))
        return 0;
    u32 d1 = fld(q->prim, q->m_pd1), d2 = fld(q->prim, q->m_pd2);
    if (!d1 || !d2)
        return 0;
    return (u32)(xosc / ref * fld(q->fbdiv, q->m_fbdiv) / (d1 * d2));
}

static void pll_status(struct pll *q)
{
    u64 v = q->cs->value & ~q->m_lock;
    if (pll_running(q))
        v |= q->m_lock;
    if (v != q->cs->value)
        rf_hw(q->cs, v);
}

static u32 pll_read(void *ctx, u32 off, int n)
{
    struct pll *q = ctx;
    pll_status(q);
    return rf_read(q->p, off, n);
}

static void pll_write(void *ctx, u32 off, int n, u32 v)
{
    struct pll *q = ctx;
    rf_write(q->p, off, n, v);
    pll_status(q);
    rp2040_clock_change(q->s);
}

static void pll_reset(void *ctx)
{
    pll_status(ctx);
}

const struct dev_ops rp2040_pll_ops = {
    "rp2040-pll", pll_read, pll_write, pll_reset, 0, 0,
};

void *rp2040_pll_create(struct sim *s, struct rf_periph *p)
{
    struct pll *q = calloc(1, sizeof *q);
    if (!q)
        die("out of memory");
    q->s = s;
    q->p = p;
    q->cs = need(p, "CS");
    q->pwr = need(p, "PWR");
    q->fbdiv = need(p, "FBDIV_INT");
    q->prim = need(p, "PRIM");
    q->m_lock = rf_mask(q->cs, "LOCK");
    q->m_bypass = rf_mask(q->cs, "BYPASS");
    q->m_refdiv = rf_mask(q->cs, "REFDIV");
    q->m_pd = rf_mask(q->pwr, "PD");
    q->m_vcopd = rf_mask(q->pwr, "VCOPD");
    q->m_postdivpd = rf_mask(q->pwr, "POSTDIVPD");
    q->m_fbdiv = rf_mask(q->fbdiv, "FBDIV_INT");
    q->m_pd1 = rf_mask(q->prim, "POSTDIV1");
    q->m_pd2 = rf_mask(q->prim, "POSTDIV2");
    return q;
}

/* ---- the clock generators -------------------------------------------------- */

static const char *const clk_names[RP_NCLK] = {
    "GPOUT0", "GPOUT1", "GPOUT2", "GPOUT3", "REF", "SYS", "PERI", "USB", "ADC", "RTC",
};

struct clocks {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *ctrl[RP_NCLK], *div[RP_NCLK], *sel[RP_NCLK];
    u64 m_src[RP_NCLK], m_aux[RP_NCLK], m_en[RP_NCLK], m_int[RP_NCLK], m_frac[RP_NCLK];
    struct rf_reg *fc0_src, *fc0_status, *fc0_result;
    u64 m_done, m_pass, m_khz, m_frac_khz;
    /* the board's time: ns up to cycle `at`, and clk_sys since; the
     * cycles the devices have been told of (dev), which leave out the
     * core's taken-branch cycles as its devices' time does */
    double ns;
    u64 at, dev;
    u32 sys_hz;
    int in_change, running;
};

static struct clocks *clocks_of(struct sim *s);
static u32 rosc_hz(struct sim *s);
void rp2040_tick_clock(struct sim *s);

static u32 xosc_hz(struct sim *s)
{
    struct xosc *x = model(s, "XOSC", &rp2040_xosc_ops);
    return x && xosc_stable(x) ? RP_XOSC_HZ : 0;
}

static u32 pll_out(struct sim *s, const char *name)
{
    return pll_hz(model(s, name, &rp2040_pll_ops));
}

/* the frequency a clock generator's divider gives from `src` */
static u32 divided(const struct clocks *c, int k, u64 src)
{
    if (!c->div[k])
        return (u32)src;
    u64 d = (u64)fld(c->div[k], c->m_int[k]) << 8 | fld(c->div[k], c->m_frac[k]);
    if (!(d >> 8))
        d = (u64)1 << 24 | (d & 0xff);       /* INT 0: divide by 2^16 */
    return (u32)(src * 256 / d);
}

/* the auxiliary sources, as each generator's AUXSRC numbers them */
static u32 aux_hz(struct sim *s, const struct clocks *c, int k, int depth)
{
    u32 a = fld(c->ctrl[k], c->m_aux[k]);
    switch (k) {
    case RP_CLK_REF:
        return a == 0 ? pll_out(s, "PLL_USB") : 0;
    case RP_CLK_SYS:
        return a == 0 ? pll_out(s, "PLL_SYS") : a == 1 ? pll_out(s, "PLL_USB")
             : a == 2 ? rosc_hz(s) : a == 3 ? xosc_hz(s) : 0;
    case RP_CLK_PERI:
        return a == 0 ? (depth ? 0 : rp2040_hz(s, RP_CLK_SYS)) : a == 1 ? pll_out(s, "PLL_SYS")
             : a == 2 ? pll_out(s, "PLL_USB") : a == 3 ? rosc_hz(s)
             : a == 4 ? xosc_hz(s) : 0;
    case RP_CLK_USB: case RP_CLK_ADC: case RP_CLK_RTC:
        return a == 0 ? pll_out(s, "PLL_USB") : a == 1 ? pll_out(s, "PLL_SYS")
             : a == 2 ? rosc_hz(s) : a == 3 ? xosc_hz(s) : 0;
    }
    return 0;
}

static u32 clk_hz(struct sim *s, const struct clocks *c, int k)
{
    if (k == RP_CLK_REF) {
        u32 src = fld(c->ctrl[k], c->m_src[k]);
        u32 f = src == 0 ? rosc_hz(s) : src == 1 ? aux_hz(s, c, k, 0)
              : src == 2 ? xosc_hz(s) : 0;
        return divided(c, k, f);
    }
    if (k == RP_CLK_SYS) {
        u32 f = fld(c->ctrl[k], c->m_src[k]) ? aux_hz(s, c, k, 0)
                                             : clk_hz(s, c, RP_CLK_REF);
        return divided(c, k, f);
    }
    if (k >= RP_CLK_PERI && c->ctrl[k] && fld(c->ctrl[k], c->m_en[k]))
        return divided(c, k, aux_hz(s, c, k, 0));
    return 0;
}

u32 rp2040_hz(struct sim *s, int clk)
{
    struct clocks *c = clocks_of(s);
    if (!c)
        return clk == RP_CLK_SYS || clk == RP_CLK_REF ? RP_ROSC_HZ : 0;
    return clk_hz(s, c, clk);
}

u64 rp2040_cycles(struct sim *s)
{
    struct clocks *c = clocks_of(s);
    return c ? c->dev : s->cycles;
}

double rp2040_now_ns(struct sim *s)
{
    struct clocks *c = clocks_of(s);
    if (!c)
        return (double)s->cycles * 1e9 / RP_ROSC_HZ;
    return c->ns + (c->sys_hz ? (double)(c->dev - c->at) * 1e9 / c->sys_hz : 0);
}

void rp2040_clock_change(struct sim *s)
{
    struct clocks *c = clocks_of(s);
    if (!c || c->in_change)
        return;
    c->in_change = 1;
    u32 hz = clk_hz(s, c, RP_CLK_SYS);
    if (hz != c->sys_hz) {
        c->ns = rp2040_now_ns(s);
        c->at = c->dev;
        c->sys_hz = hz;
    }
    rp2040_tick_clock(s);
    rp2040_uart_kick(s);
    c->in_change = 0;
}

static void selected(struct clocks *c)
{
    for (int k = RP_CLK_REF; k <= RP_CLK_SYS; k++) {
        u64 v = 1ull << fld(c->ctrl[k], c->m_src[k]);
        if (c->sel[k]->value != v)
            rf_hw(c->sel[k], v);
    }
}

/* FC0: the frequency of the source picked, in kHz and 32nds of one */
static void fc0(struct clocks *c)
{
    static const int src_clk[] = { -1, -1, -1, -1, -1, -1, -1, -1, RP_CLK_REF,
                                   RP_CLK_SYS, RP_CLK_PERI, RP_CLK_USB,
                                   RP_CLK_ADC, RP_CLK_RTC };
    u32 sel = (u32)c->fc0_src->value, hz = 0;
    struct sim *s = c->s;
    if (!sel)
        return;
    if (sel == 1)
        hz = pll_out(s, "PLL_SYS");
    else if (sel == 2)
        hz = pll_out(s, "PLL_USB");
    else if (sel == 3 || sel == 4)
        hz = rosc_hz(s);
    else if (sel == 5)
        hz = xosc_hz(s);
    else if (sel < sizeof src_clk / sizeof src_clk[0] && src_clk[sel] >= 0)
        hz = rp2040_hz(s, src_clk[sel]);
    u64 r = put_fld(0, c->m_khz, hz / 1000) |
            put_fld(0, c->m_frac_khz, (u32)((u64)(hz % 1000) * 32 / 1000));
    rf_hw(c->fc0_result, r);
    rf_hw(c->fc0_status, c->m_done | (hz ? c->m_pass : 0));
}

static u32 clocks_read(void *ctx, u32 off, int n)
{
    struct clocks *c = ctx;
    return rf_read(c->p, off, n);
}

static void clocks_write(void *ctx, u32 off, int n, u32 v)
{
    struct clocks *c = ctx;
    rf_write(c->p, off, n, v);
    selected(c);
    if (off == rf_off(c->fc0_src))
        fc0(c);
    rp2040_clock_change(c->s);
}

/* the time: every cycle the devices are told of */
static void clocks_tick(void *ctx, u32 cycles)
{
    ((struct clocks *)ctx)->dev += cycles;
}

static void clocks_reset(void *ctx)
{
    struct clocks *c = ctx;
    selected(c);
    c->ns = 0;
    c->at = c->dev = 0;
    c->sys_hz = RP_ROSC_HZ;
    c->running = 0;
}

const struct dev_ops rp2040_clocks_ops = {
    "rp2040-clocks", clocks_read, clocks_write, clocks_reset, clocks_tick, 0,
};

/* the clocks' model, its time running: started at the first ask, since
 * a model's clock cannot start while it is being created */
static struct clocks *clocks_of(struct sim *s)
{
    struct clocks *c = model(s, "CLOCKS", &rp2040_clocks_ops);
    if (c && !c->running)
        rf_clock(c->p, &c->running, 1);
    return c;
}

void *rp2040_clocks_create(struct sim *s, struct rf_periph *p)
{
    struct clocks *c = calloc(1, sizeof *c);
    char nm[32];
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    for (int k = 0; k < RP_NCLK; k++) {
        snprintf(nm, sizeof nm, "CLK_%s_CTRL", clk_names[k]);
        c->ctrl[k] = need(p, nm);
        snprintf(nm, sizeof nm, "CLK_%s_DIV", clk_names[k]);
        c->div[k] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "CLK_%s_SELECTED", clk_names[k]);
        c->sel[k] = need(p, nm);
        c->m_src[k] = rf_mask(c->ctrl[k], "SRC");
        c->m_aux[k] = rf_mask(c->ctrl[k], "AUXSRC");
        c->m_en[k] = rf_mask(c->ctrl[k], "ENABLE");
        c->m_int[k] = c->div[k] ? rf_mask(c->div[k], "INT") : 0;
        c->m_frac[k] = c->div[k] ? rf_mask(c->div[k], "FRAC") : 0;
    }
    c->fc0_src = need(p, "FC0_SRC");
    c->fc0_status = need(p, "FC0_STATUS");
    c->fc0_result = need(p, "FC0_RESULT");
    c->m_done = rf_mask(c->fc0_status, "DONE");
    c->m_pass = rf_mask(c->fc0_status, "PASS");
    c->m_khz = rf_mask(c->fc0_result, "KHZ");
    c->m_frac_khz = rf_mask(c->fc0_result, "FRAC");
    c->sys_hz = RP_ROSC_HZ;
    s->now_ns = rp2040_now_ns;
    return c;
}

/* ---- ROSC: running at its nominal frequency while enabled ---------------- */

struct rosc {
    struct rf_periph *p;
    struct rf_reg *ctrl, *status;
    u64 m_enable, m_stable, m_enabled;
};

static u32 rosc_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct rosc *)ctx)->p, off, n);
}

static void rosc_status(struct rosc *r)
{
    int on = fld(r->ctrl, r->m_enable) != XOSC_DISABLE;
    u64 v = r->status->value & ~(r->m_stable | r->m_enabled);
    if (on)
        v |= r->m_stable | r->m_enabled;
    if (v != r->status->value)
        rf_hw(r->status, v);
}

static void rosc_write(void *ctx, u32 off, int n, u32 v)
{
    struct rosc *r = ctx;
    rf_write(r->p, off, n, v);
    rosc_status(r);
    rp2040_clock_change(r->p->sim);
}

static void rosc_reset(void *ctx)
{
    rosc_status(ctx);
}

const struct dev_ops rp2040_rosc_ops = {
    "rp2040-rosc", rosc_read, rosc_write, rosc_reset, 0, 0,
};

void *rp2040_rosc_create(struct sim *s, struct rf_periph *p)
{
    struct rosc *r = calloc(1, sizeof *r);
    (void)s;
    if (!r)
        die("out of memory");
    r->p = p;
    r->ctrl = need(p, "CTRL");
    r->status = need(p, "STATUS");
    r->m_enable = rf_mask(r->ctrl, "ENABLE");
    r->m_stable = rf_mask(r->status, "STABLE");
    r->m_enabled = rf_mask(r->status, "ENABLED");
    rf_set_reset(r->status, r->status->reset | r->m_stable | r->m_enabled);
    return r;
}

static u32 rosc_hz(struct sim *s)
{
    struct rosc *r = model(s, "ROSC", &rp2040_rosc_ops);
    if (r && fld(r->ctrl, r->m_enable) == XOSC_DISABLE)
        return 0;
    return RP_ROSC_HZ;
}

/* ---- RESETS ------------------------------------------------------------------ */

struct resets {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *reset, *done;
    u64 mask;                       /* the bits with a peripheral */
    struct rf_periph *held[32][2];
};

static void resets_apply(struct resets *r, u64 was)
{
    u64 now = r->reset->value & r->mask;
    for (int b = 0; b < 32; b++)
        if ((now & ~was) >> b & 1)
            for (int k = 0; k < 2 && r->held[b][k]; k++)
                rf_periph_reset(r->held[b][k]);
    u64 d = ~now & r->mask;
    if (r->done->value != d)
        rf_hw(r->done, d);
}

static u32 resets_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct resets *)ctx)->p, off, n);
}

static void resets_write(void *ctx, u32 off, int n, u32 v)
{
    struct resets *r = ctx;
    u64 was = r->reset->value & r->mask;
    rf_write(r->p, off, n, v);
    resets_apply(r, was);
}

static void resets_reset(void *ctx)
{
    resets_apply(ctx, ~0ull);
}

const struct dev_ops rp2040_resets_ops = {
    "rp2040-resets", resets_read, resets_write, resets_reset, 0, 0,
};

/* where RP2040.svd's reset values are not the part's */
static void corrections(struct sim *s)
{
    struct rf_periph *p = periph(s, "SYSINFO");
    struct rf_reg *r = p ? rf_reg(p, "CHIP_ID") : 0;
    if (r)
        rf_set_reset(r, 0x20002927u);       /* revision 2 (B2), part 2, 0x927 */
    r = p ? rf_reg(p, "PLATFORM") : 0;
    if (r)
        rf_set_reset(r, rf_mask(r, "ASIC"));
    p = periph(s, "VREG_AND_CHIP_RESET");
    r = p ? rf_reg(p, "CHIP_RESET") : 0;
    if (r)
        rf_set_reset(r, rf_mask(r, "HAD_POR"));
}

void *rp2040_resets_create(struct sim *s, struct rf_periph *p)
{
    struct resets *r = calloc(1, sizeof *r);
    if (!r)
        die("out of memory");
    r->s = s;
    r->p = p;
    r->reset = need(p, "RESET");
    r->done = need(p, "RESET_DONE");
    for (int i = 0; i < r->reset->nf; i++) {
        const struct rf_field *f = &r->reset->f[i];
        if (!f->name || f->width != 1)
            continue;
        const char *names[2] = { f->name, 0 };
        if (!strcmp(f->name, "USBCTRL"))
            names[0] = "USB", names[1] = "USB_DPRAM";
        for (int k = 0; k < 2 && names[k]; k++) {
            struct rf_periph *q = periph(s, names[k]);
            if (!q)
                continue;
            size_t l = strlen(f->name) + 16;
            char *g = malloc(l);
            if (!g)
                die("out of memory");
            snprintf(g, l, "RESETS.RESET.%s", f->name);
            q->gate = r->reset;
            q->gate_mask = 1ull << f->lsb;
            q->gate_held = 1;
            q->gate_name = g;
            r->held[f->lsb][k] = q;
            r->mask |= 1ull << f->lsb;
        }
    }
    corrections(s);
    resets_apply(r, ~0ull);
    return r;
}

/* the bootrom's flash boot leaves these out of reset */
static void bootrom_resets(struct sim *s)
{
    struct resets *r = model(s, "RESETS", &rp2040_resets_ops);
    if (!r)
        return;
    u64 out = rf_mask(r->reset, "IO_QSPI") | rf_mask(r->reset, "PADS_QSPI") |
              rf_mask(r->reset, "PADS_BANK0");
    u64 was = r->reset->value & r->mask;
    r->reset->value &= ~out;
    resets_apply(r, was);
}

/* ---- PSM: every domain powered but those forced off ------------------------ */

struct psm {
    struct rf_periph *p;
    struct rf_reg *off, *done;
};

static void psm_done(struct psm *m)
{
    u64 v = rf_fields(m->done) & ~m->off->value;
    if (m->done->value != v)
        rf_hw(m->done, v);
}

static u32 psm_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct psm *)ctx)->p, off, n);
}

static void psm_write(void *ctx, u32 off, int n, u32 v)
{
    struct psm *m = ctx;
    rf_write(m->p, off, n, v);
    psm_done(m);
}

static void psm_reset(void *ctx)
{
    psm_done(ctx);
}

const struct dev_ops rp2040_psm_ops = {
    "rp2040-psm", psm_read, psm_write, psm_reset, 0, 0,
};

void *rp2040_psm_create(struct sim *s, struct rf_periph *p)
{
    struct psm *m = calloc(1, sizeof *m);
    (void)s;
    if (!m)
        die("out of memory");
    m->p = p;
    m->off = need(p, "FRCE_OFF");
    m->done = need(p, "DONE");
    rf_set_reset(m->done, rf_fields(m->done));
    return m;
}

/* ---- the watchdog and its tick generator --------------------------------- */

struct watchdog {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *ctrl, *load, *reason, *tick;
    u64 m_trigger, m_enable, m_time, m_tick_en, m_running, m_cycles, m_force, m_timer;
    u32 counter;                    /* CTRL.TIME */
    u64 acc;                        /* clk_ref edges, in units of 1/clk_sys */
    int running;                    /* its clock: the tick generator runs */
};

void rp2040_timer_ticks(struct sim *s, u32 n);
int rp2040_timer_next(struct sim *s, u32 *ticks);

u32 rp2040_tick_cycles(struct sim *s)
{
    struct watchdog *w = model(s, "WATCHDOG", &rp2040_watchdog_ops);
    if (!w || !fld(w->tick, w->m_tick_en))
        return 0;
    return fld(w->tick, w->m_cycles);
}

static void wd_fire(struct watchdog *w, int forced)
{
    struct sim *s = w->s;
    rf_hw(w->reason, forced ? w->m_force : w->m_timer);
    if (s->state == RUN) {
        s->exit_status = 0;
        sim_end(s, END_EXIT, forced ? "the watchdog's CTRL.TRIGGER reset the chip"
                                    : "the watchdog's counter reached 0 and reset the chip");
    }
}

static void wd_time(struct watchdog *w)
{
    u64 v = put_fld(w->ctrl->value, w->m_time, w->counter);
    if (v != w->ctrl->value)
        w->ctrl->value = v;         /* every tick: not traced */
}

static void wd_clock(struct watchdog *w)
{
    u32 n = rp2040_tick_cycles(w->s);
    rf_clock(w->p, &w->running, n && rp2040_hz(w->s, RP_CLK_REF));
}

static void wd_tick(void *ctx, u32 cycles)
{
    struct watchdog *w = ctx;
    struct sim *s = w->s;
    u64 ref = rp2040_hz(s, RP_CLK_REF), sys = rp2040_hz(s, RP_CLK_SYS);
    u32 per = rp2040_tick_cycles(s);
    if (!ref || !sys || !per)
        return;
    w->acc += (u64)cycles * ref;
    u64 one = sys * per;
    if (w->acc < one)
        return;
    u32 n = (u32)(w->acc / one);
    w->acc %= one;
    rp2040_timer_ticks(s, n);
    if (fld(w->ctrl, w->m_enable) && w->counter) {
        u64 dec = 2ull * n;         /* twice a tick: RP2040-E1 */
        w->counter = dec >= w->counter ? 0 : w->counter - (u32)dec;
        wd_time(w);
        if (!w->counter)
            wd_fire(w, 0);
    }
}

static int wd_next_event(void *ctx, u32 *cycles)
{
    struct watchdog *w = ctx;
    struct sim *s = w->s;
    u64 ref = rp2040_hz(s, RP_CLK_REF), sys = rp2040_hz(s, RP_CLK_SYS);
    u32 per = rp2040_tick_cycles(s), ticks = 0;
    int any = rp2040_timer_next(s, &ticks);
    if (fld(w->ctrl, w->m_enable) && w->counter) {
        u32 t = (w->counter + 1) / 2;
        if (!any || t < ticks)
            ticks = t;
        any = 1;
    }
    if (!any || !ref || !sys || !per)
        return 0;
    /* the cycles until `ticks` ticks have come */
    u64 need_acc = (u64)ticks * sys * per - w->acc;
    u64 c = (need_acc + ref - 1) / ref;
    *cycles = c > 0xffffffffu ? 0xffffffffu : (u32)c;
    return 1;
}

static u32 wd_read(void *ctx, u32 off, int n)
{
    struct watchdog *w = ctx;
    return rf_read(w->p, off, n);
}

static void wd_write(void *ctx, u32 off, int n, u32 v)
{
    struct watchdog *w = ctx;
    rf_write(w->p, off, n, v);
    if (off == rf_off(w->load))
        w->counter = v & 0xffffff;
    if (off == rf_off(w->ctrl) && (v & w->m_trigger))
        wd_fire(w, 1);
    wd_time(w);
    u64 t = put_fld(w->tick->value, w->m_running, fld(w->tick, w->m_tick_en));
    if (t != w->tick->value)
        rf_hw(w->tick, t);
    wd_clock(w);
}

static void wd_reset(void *ctx)
{
    struct watchdog *w = ctx;
    w->counter = 0;
    w->acc = 0;
    w->running = 0;
    rf_hw(w->tick, put_fld(w->tick->value, w->m_running, fld(w->tick, w->m_tick_en)));
    wd_clock(w);
}

const struct dev_ops rp2040_watchdog_ops = {
    "rp2040-watchdog", wd_read, wd_write, wd_reset, wd_tick, wd_next_event,
};

void *rp2040_watchdog_create(struct sim *s, struct rf_periph *p)
{
    struct watchdog *w = calloc(1, sizeof *w);
    if (!w)
        die("out of memory");
    w->s = s;
    w->p = p;
    w->ctrl = need(p, "CTRL");
    w->load = need(p, "LOAD");
    w->reason = need(p, "REASON");
    w->tick = need(p, "TICK");
    w->m_trigger = rf_mask(w->ctrl, "TRIGGER");
    w->m_enable = rf_mask(w->ctrl, "ENABLE");
    w->m_time = rf_mask(w->ctrl, "TIME");
    w->m_tick_en = rf_mask(w->tick, "ENABLE");
    w->m_running = rf_mask(w->tick, "RUNNING");
    w->m_cycles = rf_mask(w->tick, "CYCLES");
    w->m_force = rf_mask(w->reason, "FORCE");
    w->m_timer = rf_mask(w->reason, "TIMER");
    rf_set_reset(w->tick, w->tick->reset | (w->tick->reset & w->m_tick_en ? w->m_running : 0));
    return w;
}

/* a clock the watchdog's generator runs on may have started or stopped */
void rp2040_tick_clock(struct sim *s)
{
    struct watchdog *w = model(s, "WATCHDOG", &rp2040_watchdog_ops);
    if (w)
        wd_clock(w);
}

/* ---- the ROM ------------------------------------------------------------------ */

/* EmbSim's ROM, assembled from this (llvm-mc --triple=thumbv6m; the test
 * holds the bytes to it):
 *
 *     .word 0x20041f00          @ 0x00: the bootrom's stack top
 *     .word reset + 1           @ 0x04
 *     .word dead + 1            @ 0x08: NMI
 *     .word dead + 1            @ 0x0c: HardFault
 *     .byte 'M', 'u', 1, 3      @ 0x10: magic, version 1, ROM version 3 (B2)
 *     .hword table, table, lookup + 1, 0
 * lookup:                       @ 0x1c: rom_table_lookup finds nothing
 *     movs r0, #0
 *     bx lr
 * reset:                        @ 0x20: enter boot2, as the bootrom does
 *     ldr r0, =0x20041f01
 *     movs r1, #0
 *     mov lr, r1
 *     bx r0
 * dead:                         @ 0x28: _dead
 *     wfi
 *     b dead
 * table:                        @ 0x2c: the function and data tables, empty
 *     .hword 0, 0
 *     .word 0x20041f01          @ 0x30: the literal pool */
static const u8 rom_image[] = {
    0x00, 0x1f, 0x04, 0x20, 0x21, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,
    0x29, 0x00, 0x00, 0x00, 0x4d, 0x75, 0x01, 0x03, 0x2c, 0x00, 0x2c, 0x00,
    0x1d, 0x00, 0x00, 0x00, 0x00, 0x20, 0x70, 0x47, 0x03, 0x48, 0x00, 0x21,
    0x8e, 0x46, 0x00, 0x47, 0x30, 0xbf, 0xfd, 0xe7, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x1f, 0x04, 0x20,
};
#define ROM_DEAD 0x29u

#define BOOT2_AT 0x20041f00u

struct rom {
    struct sim *s;
    int booted;                     /* since the reset */
    int bad;                        /* boot2's CRC failed */
};

/* boot2's CRC: CRC-32/MPEG-2, the bootrom's crc32_small */
static u32 crc32_mpeg2(const u8 *p, u32 n)
{
    u32 c = 0xffffffffu;
    for (u32 i = 0; i < n; i++) {
        c ^= (u32)p[i] << 24;
        for (int k = 0; k < 8; k++)
            c = c & 0x80000000u ? c << 1 ^ 0x04c11db7u : c << 1;
    }
    return c;
}

/* the bootrom's flash boot: boot2 checked, copied, and the peripherals as
 * the bootrom leaves them */
static void boot(struct rom *r)
{
    struct sim *s = r->s;
    struct region *f = bus_region(&s->bus, 0x10000000u, 256);
    struct region *m = bus_region(&s->bus, BOOT2_AT, 256);
    r->booted = 1;
    r->bad = 0;
    if (!f || !m)
        die("the RP2040's flash or SRAM is not on the bus");
    const u8 *b2 = f->mem + (0x10000000u - f->base);
    u32 sum = crc32_mpeg2(b2, 252), want = mem_rd_le(b2 + 252, 4);
    if (sum != want) {
        r->bad = 1;
        fflush(stdout);
        fprintf(stderr, "embsim: the RP2040's bootrom finds no boot2 in flash: "
                "the CRC-32 of its first 252 bytes is 0x%08x, the word after "
                "them 0x%08x\n", sum, want);
        sim_end(s, END_LOCKUP, "no boot2 in flash: the part would wait for "
                "USB (its BOOTSEL mode)");
        return;
    }
    memcpy(m->mem + (BOOT2_AT - m->base), b2, 256);
    bootrom_resets(s);
    /* connect_internal_flash: the QSPI pins' function is the XIP's */
    struct rf_periph *p = periph(s, "IO_QSPI");
    for (int i = 0; p && i < p->nreg; i++)
        if (strstr(p->reg[i]->name, "_CTRL"))
            p->reg[i]->value = 0;
    p = periph(s, "PADS_BANK0");
    for (int g = 26; p && g <= 29; g++) {
        char nm[16];
        snprintf(nm, sizeof nm, "GPIO%d", g);
        struct rf_reg *q = rf_reg(p, nm);
        if (q)
            q->value &= ~rf_mask(q, "IE");
    }
}

static u32 rom_read(void *ctx, u32 off, int n)
{
    struct rom *r = ctx;
    if (off == 0 && !r->booted && !r->s->bus.debug)
        boot(r);
    u32 v = 0;
    for (int i = n - 1; i >= 0; i--) {
        u32 a = off + (u32)i;
        u8 b = a < sizeof rom_image ? rom_image[a] : 0;
        if (r->bad && a >= 4 && a < 8)  /* the reset vector: _dead */
            b = (u8)(a == 4 ? ROM_DEAD : 0);
        v = v << 8 | b;
    }
    return v;
}

static void rom_reset(void *ctx)
{
    ((struct rom *)ctx)->booted = 0;
}

const struct dev_ops rp2040_rom_ops = {
    "rp2040-rom", rom_read, 0, rom_reset, 0, 0,
};

void *rp2040_rom_create(struct sim *s, const struct dev_desc *d)
{
    struct rom *r = calloc(1, sizeof *r);
    (void)d;
    if (!r)
        die("out of memory");
    r->s = s;
    return r;
}
