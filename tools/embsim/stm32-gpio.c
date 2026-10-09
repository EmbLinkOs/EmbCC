/* stm32-gpio.c -- an STM32 GPIO port (the F2/F4/F7/L4 layout: MODER,
 * OTYPER, PUPDR, IDR, ODR, BSRR), over the SVD's registers.
 *
 *   - BSRR sets and resets ODR's bits: its low half sets, its high half
 *     resets, and a bit set in both is set (RM0090 8.4.7). A halfword
 *     write to either half does that half. BSRR reads as 0 (the file
 *     says write-only);
 *   - IDR is what the pins read, from how each is configured, since
 *     nothing outside drives them: an output reads what ODR drives (an
 *     open-drain one at 1 is let go, and reads its pull); an input or an
 *     alternate function reads its pull-up as 1, pull-down or none as 0;
 *     an analog pin reads 0 (RM0090 8.3.12, the Schmitt trigger is off).
 *     So GPIOA reads 0xa000 out of reset: PA13 and PA15 are the debug
 *     port's, pulled up.
 *   - ODR's and IDR's changes show in --trace-periph as the hardware's
 *     (H), pin by pin: a BSRR write to set PA5 shows GPIOA.ODR.ODR5 0->1.
 *
 * Not modelled: LCKR's lock sequence (it is a plain register), the
 * alternate functions themselves, the pins' speed. */
#include <stdlib.h>

#include "devices.h"
#include "svd-map.h"

struct gpio {
    struct rf_periph *p;
    struct rf_reg *moder, *otyper, *pupdr, *idr, *odr, *bsrr;
};

static void update_idr(struct gpio *g)
{
    u64 moder = g->moder->value, pupdr = g->pupdr->value;
    u64 od = g->otyper ? g->otyper->value : 0, odr = g->odr->value;
    u64 idr = 0;
    for (int i = 0; i < 16; i++) {
        int mode = (int)(moder >> 2 * i & 3), up = (pupdr >> 2 * i & 3) == 1;
        int out = (int)(odr >> i & 1), pin;
        if (mode == 1)
            pin = (od >> i & 1) && out ? up : out;
        else if (mode == 3)
            pin = 0;
        else
            pin = up;
        idr |= (u64)pin << i;
    }
    if ((g->idr->value & 0xffff) != idr)
        rf_hw(g->idr, (g->idr->value & ~0xffffULL) | idr);
}

static u32 gpio_read(void *ctx, u32 off, int n)
{
    struct gpio *g = ctx;
    return rf_read(g->p, off, n);
}

static void gpio_write(void *ctx, u32 off, int n, u32 v)
{
    struct gpio *g = ctx;
    rf_write(g->p, off, n, v);
    u32 b = rf_off(g->bsrr);
    if (off - b < 4) {
        /* the bits this access wrote, where BSRR has them */
        u64 m = (n >= 4 ? 0xffffffffULL : (1ULL << 8 * n) - 1) << 8 * (off - b);
        u64 w = ((u64)v << 8 * (off - b)) & m;
        u64 set = w & 0xffff, clr = w >> 16 & 0xffff;
        u64 odr = g->odr->value;
        u64 nv = (odr & ~clr) | set;
        if (nv != odr)
            rf_hw(g->odr, nv);
    }
    update_idr(g);
}

static void gpio_reset(void *ctx)
{
    update_idr(ctx);
}

const struct dev_ops stm32_gpio_ops = {
    "stm32-gpio", gpio_read, gpio_write, gpio_reset, 0, 0,
};

void *stm32_gpio_create(struct sim *s, struct rf_periph *p)
{
    struct gpio g;
    (void)s;
    g.p = p;
    g.moder = rf_reg(p, "MODER");
    g.otyper = rf_reg(p, "OTYPER");
    g.pupdr = rf_reg(p, "PUPDR");
    g.idr = rf_reg(p, "IDR");
    g.odr = rf_reg(p, "ODR");
    g.bsrr = rf_reg(p, "BSRR");
    if (!g.moder || !g.pupdr || !g.idr || !g.odr || !g.bsrr)
        return 0;                   /* not this layout: the registers alone */
    struct gpio *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = g;
    update_idr(c);
    return c;
}
