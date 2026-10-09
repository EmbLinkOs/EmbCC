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
 * Not modelled: the clock frequencies themselves (EmbSim's time is the
 * core's cycles, whatever the PLL says: a timer reads its APB prescaler
 * from CFGR, stm32-tim.c), the clock security system, the interrupts of
 * CIR, the reset registers (a peripheral is not reset by them), RMVF. */
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
    struct rf_periph *p;
    struct follow f[MAX_FOLLOW];
    int nf;
};

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

static u32 rcc_read(void *ctx, u32 off, int n)
{
    struct rcc *c = ctx;
    return rf_read(c->p, off, n);
}

static void rcc_write(void *ctx, u32 off, int n, u32 v)
{
    struct rcc *c = ctx;
    rf_write(c->p, off, n, v);
    settle(c);
}

static void rcc_reset(void *ctx)
{
    settle(ctx);
}

const struct dev_ops stm32_rcc_ops = {
    "stm32-rcc", rcc_read, rcc_write, rcc_reset, 0, 0,
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
    c->p = p;
    for (int k = 0; k < p->nreg; k++) {
        struct rf_reg *r = p->reg[k];
        for (int i = 0; i < r->nf; i++) {
            const char *fn = r->f[i].name;
            char base[64];
            size_t l = fn ? strlen(fn) : 0;
            if (l < 3 || l >= sizeof base)
                continue;
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
    return c;
}
