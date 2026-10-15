/* stm32-rcc.c -- the STM32's reset and clock control, over the SVD's RCC
 * registers. What it does is found by the fields' names, so it holds
 * across the families whose RCC is laid out this way (F1, F2, F4, F7):
 *
 *   - a ready bit follows its enable: each field XRDY of a register with
 *     a field XON (CR's HSIRDY, HSERDY, PLLRDY and PLLI2SRDY, BDCR's
 *     LSERDY, CSR's LSIRDY). An oscillator or a PLL is ready the moment it
 *     is turned on, and not ready when off;
 *   - CFGR's system clock switch status follows the switch: SWS from SW
 *     (ST's F4 file splits both into SW0, SW1 and SWS0, SWS1), at once;
 *   - the clock gates: a peripheral with an enable bit NAMEEN in one of the
 *     ENR registers (AHB1ENR, APB1ENR, ...; not the low-power LPENRs)
 *     reads as 0 and ignores writes while the bit is clear, as the part's
 *     peripherals do (RM0090 6.3.10: the peripheral's clock is off, so are
 *     its registers). The first such access is a warning on stderr, so a
 *     driver that forgot its clock says so instead of hanging.
 *
 * The F1's (an RCC whose CFGR has PLLMUL: RM0008's) more:
 *   - a clock source is used once it is ready (RM0008, "System clock
 *     (SYSCLK) selection"): CFGR's SWS takes SW's value when that
 *     source's ready bit is set, and else keeps the source in use,
 *     switching when it becomes ready; PLLRDY
 *     sets when PLLON is set and its source (PLLSRC: HSI / 2, or HSE,
 *     halved with PLLXTPRE) is ready;
 *   - the clocks' frequencies: HSI 8 MHz, HSE the board's 8 MHz crystal
 *     (the Blue Pill's), the PLL its source times PLLMUL's factor (2 to
 *     16), SYSCLK the source SWS says, HCLK SYSCLK over HPRE's divisor.
 *     The core runs at HCLK: the board's time (s->now_ns) is its cycles at
 *     it. A GPIO port's clock enable is IOPxEN.
 *
 * Not modelled: the F2-F7's clock frequencies (EmbSim's time is the
 * core's cycles, whatever the PLL says: a timer reads its APB prescaler
 * from CFGR, stm32-tim.c), the clock security system, the interrupts of
 * CIR, the reset registers (a peripheral is not reset by them), RMVF, the
 * oscillators' and the PLL's start-up times. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define MAX_FOLLOW 16

struct follow {
    struct rf_reg *r;
    u64 from, to;                   /* `to` takes the bits of `from` */
    int shift;                      /* to's position less from's */
};

struct rcc {
    struct sim *s;
    struct rf_periph *p;
    struct follow f[MAX_FOLLOW];
    int nf;
    /* the F1's */
    int f1;
    struct rf_reg *cr, *cfgr;
    u64 hsion, hsirdy, hseon, hserdy, pllon, pllrdy, sw, sws;
    u64 pllsrc, pllxtpre, pllmul, hpre;
    double ns;                      /* the board's time up to cycle `at` */
    u64 at, dev;                    /* the cycles the devices were told of */
    u32 hclk;
    int running;
};

/* the Blue Pill's crystal, and the F1's internal oscillator */
#define F1_HSE_HZ 8000000u
#define F1_HSI_HZ 8000000u

static void add_follow(struct rcc *c, struct rf_reg *r, u64 from, u64 to)
{
    if (c->nf == MAX_FOLLOW || !from || !to)
        return;
    int a = 0, b = 0;
    while (!(from >> a & 1))
        a++;
    while (!(to >> b & 1))
        b++;
    c->f[c->nf].r = r;
    c->f[c->nf].from = from;
    c->f[c->nf].to = to;
    c->f[c->nf++].shift = b - a;
}

/* every ready and status bit as its enable or switch has it now */
static void settle(struct rcc *c)
{
    for (int i = 0; i < c->nf; i++) {
        struct follow *f = &c->f[i];
        u64 v = f->r->value & f->from;
        v = f->shift >= 0 ? v << f->shift : v >> -f->shift;
        u64 nv = (f->r->value & ~f->to) | (v & f->to);
        if (nv != f->r->value)
            rf_hw(f->r, nv);
    }
}

static u32 fld(const struct rf_reg *r, u64 mask)
{
    int sh = 0;
    if (!mask)
        return 0;
    while (!(mask >> sh & 1))
        sh++;
    return (u32)((r->value & mask) >> sh);
}

/* the F1's PLL output and its sources' readiness */
static u32 f1_pll_hz(const struct rcc *c)
{
    u32 src = fld(c->cfgr, c->pllsrc) ? F1_HSE_HZ >> fld(c->cfgr, c->pllxtpre)
                                       : F1_HSI_HZ / 2;
    u32 m = fld(c->cfgr, c->pllmul) + 2;
    return src * (m > 16 ? 16 : m);
}

static int f1_ready(const struct rcc *c, u32 src)
{
    u64 cr = c->cr->value;
    return src == 0 ? (cr & c->hsirdy) != 0 : src == 1 ? (cr & c->hserdy) != 0
         : src == 2 ? (cr & c->pllrdy) != 0 : 0;
}

static u32 f1_hclk(const struct rcc *c)
{
    static const u32 hdiv[8] = { 2, 4, 8, 16, 64, 128, 256, 512 };
    u32 sws = fld(c->cfgr, c->sws);
    u32 f = sws == 0 ? F1_HSI_HZ : sws == 1 ? F1_HSE_HZ : sws == 2 ? f1_pll_hz(c) : 0;
    u32 h = fld(c->cfgr, c->hpre);
    return h & 8 ? f / hdiv[h & 7] : f;
}

static double f1_now_ns(struct sim *s);

/* the F1's ready bits, the clock in use, the time at the old one */
static void f1_settle(struct rcc *c)
{
    u64 cr = c->cr->value;
    cr = (cr & ~c->hsirdy) | (cr & c->hsion ? c->hsirdy : 0);
    cr = (cr & ~c->hserdy) | (cr & c->hseon ? c->hserdy : 0);
    int src_ok = fld(c->cfgr, c->pllsrc) ? (cr & c->hserdy) != 0 : (cr & c->hsirdy) != 0;
    cr = (cr & ~c->pllrdy) | (cr & c->pllon && src_ok ? c->pllrdy : 0);
    if (cr != c->cr->value)
        rf_hw(c->cr, cr);
    u32 sw = fld(c->cfgr, c->sw);
    if (sw != fld(c->cfgr, c->sws) && f1_ready(c, sw)) {
        int sh = 0;
        while (!(c->sws >> sh & 1))
            sh++;
        rf_hw(c->cfgr, (c->cfgr->value & ~c->sws) | ((u64)sw << sh & c->sws));
    }
    u32 h = f1_hclk(c);
    if (h != c->hclk) {
        c->ns = f1_now_ns(c->s);
        c->at = c->dev;
        c->hclk = h;
    }
}

static struct rcc *f1_rcc(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "RCC") : 0;
    struct rcc *c = p && p->ops == &stm32_rcc_ops ? p->ctx : 0;
    if (c && c->f1 && !c->running)
        rf_clock(c->p, &c->running, 1);  /* not while it is being created */
    return c && c->f1 ? c : 0;
}

static double f1_now_ns(struct sim *s)
{
    struct rcc *c = f1_rcc(s);
    if (!c)
        return 0;
    return c->ns + (c->hclk ? (double)(c->dev - c->at) * 1e9 / c->hclk : 0);
}

/* the F1's HCLK, the core's clock, in Hz (0 without the F1's RCC) */
u32 stm32f1_hclk(struct sim *s)
{
    struct rcc *c = f1_rcc(s);
    return c ? c->hclk : 0;
}

static void rcc_tick(void *ctx, u32 cycles)
{
    ((struct rcc *)ctx)->dev += cycles;
}

static u32 rcc_read(void *ctx, u32 off, int n)
{
    struct rcc *c = ctx;
    if (c->f1)
        f1_rcc(c->s);
    return rf_read(c->p, off, n);
}

static void rcc_write(void *ctx, u32 off, int n, u32 v)
{
    struct rcc *c = ctx;
    rf_write(c->p, off, n, v);
    settle(c);
    if (c->f1) {
        f1_rcc(c->s);
        f1_settle(c);
    }
}

static void rcc_reset(void *ctx)
{
    struct rcc *c = ctx;
    settle(c);
    if (c->f1) {
        c->ns = 0;
        c->at = c->dev = 0;
        c->running = 0;
        c->hclk = 0;
        f1_settle(c);
    }
}

const struct dev_ops stm32_rcc_ops = {
    "stm32-rcc", rcc_read, rcc_write, rcc_reset, rcc_tick, 0,
};

static int ends_with(const char *s, const char *t)
{
    size_t a = strlen(s), b = strlen(t);
    return a >= b && !strcmp(s + a - b, t);
}

/* the peripherals whose clock enables are in this RCC */
static void gates(struct sim *s, struct rcc *c)
{
    char want[64];
    struct rf_periph *q;
    for (int i = 0; (q = rf_periph_at(s->svd, i)) != 0; i++) {
        if (q == c->p || strlen(q->name) + 3 > sizeof want)
            continue;
        strcpy(want, q->name);
        strcat(want, "EN");
        /* an F1's GPIO port is IOPx */
        if (!strncmp(q->name, "GPIO", 4) && strlen(q->name) == 5 &&
            !rf_mask(rf_reg(c->p, "APB2ENR"), want) && rf_mask(rf_reg(c->p, "APB2ENR"), "IOPAEN"))
            snprintf(want, sizeof want, "IOP%cEN", q->name[4]);
        for (int k = 0; k < c->p->nreg && !q->gate; k++) {
            struct rf_reg *r = c->p->reg[k];
            if (!ends_with(r->name, "ENR") || ends_with(r->name, "LPENR"))
                continue;
            u64 m = rf_mask(r, want);
            if (!m)
                continue;
            size_t l = strlen(c->p->name) + strlen(r->name) + strlen(want) + 3;
            char *g = malloc(l);
            if (!g)
                die("out of memory");
            snprintf(g, l, "%s.%s.%s", c->p->name, r->name, want);
            q->gate = r;
            q->gate_mask = m;
            q->gate_name = g;
        }
    }
}

void *stm32_rcc_create(struct sim *s, struct rf_periph *p)
{
    struct rcc *c = calloc(1, sizeof *c);
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    c->cr = rf_reg(p, "CR");
    c->cfgr = rf_reg(p, "CFGR");
    c->f1 = c->cr && c->cfgr && rf_mask(c->cfgr, "PLLMUL") && rf_mask(c->cfgr, "SWS");
    if (c->f1) {
        c->hsion = rf_mask(c->cr, "HSION");
        c->hsirdy = rf_mask(c->cr, "HSIRDY");
        c->hseon = rf_mask(c->cr, "HSEON");
        c->hserdy = rf_mask(c->cr, "HSERDY");
        c->pllon = rf_mask(c->cr, "PLLON");
        c->pllrdy = rf_mask(c->cr, "PLLRDY");
        c->sw = rf_mask(c->cfgr, "SW");
        c->sws = rf_mask(c->cfgr, "SWS");
        c->pllsrc = rf_mask(c->cfgr, "PLLSRC");
        c->pllxtpre = rf_mask(c->cfgr, "PLLXTPRE");
        c->pllmul = rf_mask(c->cfgr, "PLLMUL");
        c->hpre = rf_mask(c->cfgr, "HPRE");
        s->now_ns = f1_now_ns;
    }
    for (int k = 0; k < p->nreg; k++) {
        struct rf_reg *r = p->reg[k];
        for (int i = 0; i < r->nf; i++) {
            const char *fn = r->f[i].name;
            char base[64];
            size_t l = fn ? strlen(fn) : 0;
            if (l < 3 || l >= sizeof base)
                continue;
            if (c->f1 && (r == c->cr || r == c->cfgr))
                continue;                   /* f1_settle's */
            if (l > 3 && ends_with(fn, "RDY")) {    /* HSERDY: HSEON */
                memcpy(base, fn, l - 3);
                strcpy(base + l - 3, "ON");
                add_follow(c, r, rf_mask(r, base), rf_mask(r, fn));
            } else if (!strncmp(fn, "SWS", 3)) {    /* SWS0: SW0 */
                strcpy(base, "SW");
                strcat(base, fn + 3);
                add_follow(c, r, rf_mask(r, base), rf_mask(r, fn));
            }
        }
    }
    gates(s, c);
    settle(c);
    if (c->f1)
        f1_settle(c);
    return c;
}
