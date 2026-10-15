/* rp2040-sio.c -- the RP2040's single-cycle IO block (its datasheet,
 * 2.3.1), as core 0 sees it, over RP2040.svd's SIO registers.
 *
 *   - CPUID reads 0: the core is core 0;
 *   - GPIO_OUT and GPIO_OE, GPIO_HI_OUT and GPIO_HI_OE, each with its SET,
 *     CLR and XOR aliases, drive the pins whose function is SIO
 *     (rp2040-io.c); GPIO_IN and GPIO_HI_IN read them;
 *   - the 32 spinlocks: a read of SPINLOCKn claims the lock and reads
 *     1 << n, or 0 when it was claimed already; a write releases it;
 *     SPINLOCK_ST is their bitmap;
 *   - the divider: a write of an operand (UDIVIDEND, UDIVISOR, unsigned;
 *     SDIVIDEND, SDIVISOR, signed) starts a calculation, whose QUOTIENT and
 *     REMAINDER are there at once but CSR.READY only after 8 cycles; DIRTY
 *     sets on any write and clears when QUOTIENT is read; a write of
 *     QUOTIENT or REMAINDER halts the calculation (READY). A divisor of 0
 *     is not defined by the datasheet (the SDK never asks it): EmbSim gives
 *     a quotient of all ones (signed: -1, or 1 for a negative dividend) and
 *     the dividend as the remainder; INT_MIN / -1 gives INT_MIN, remainder 0;
 *   - the FIFOs: core 0's outgoing FIFO takes eight words (RDY while it
 *     has room; a write when full is lost and sets WOF); its incoming FIFO
 *     is always empty, since core 1 is not modelled (VLD never sets; a
 *     read while empty sets ROE and reads 0). WOF and ROE are cleared by
 *     writing FIFO_ST. SIO_IRQ_PROC0 is raised while VLD, WOF or ROE is set.
 * Not modelled: the interpolators (their registers keep what is written),
 * core 1's view. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"
#include "svd-map.h"

struct sio {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *cpuid, *gin, *ghin, *out, *oe, *hout, *hoe;
    struct rf_reg *out_set, *out_clr, *out_xor, *oe_set, *oe_clr, *oe_xor;
    struct rf_reg *hout_set, *hout_clr, *hout_xor, *hoe_set, *hoe_clr, *hoe_xor;
    struct rf_reg *fifo_st, *fifo_wr, *fifo_rd, *lock_st, *lock[32];
    struct rf_reg *udividend, *udivisor, *sdividend, *sdivisor, *quot, *rem, *csr;
    u64 m_vld, m_rdy, m_wof, m_roe, m_ready, m_dirty;
    int irq;
    u32 locks;
    u32 tx[8];
    int ntx;
    u64 div_at;                     /* the cycle the calculation started */
    int div_running;
};

/* FIFO_ST, with WOF or ROE set by what just happened (`set`): its IRQ is
 * raised when VLD, WOF or ROE sets */
static void fifo_status(struct sio *x, u64 set)
{
    u64 irq = x->m_vld | x->m_wof | x->m_roe;
    int was = (x->fifo_st->value & irq) != 0;
    u64 v = (x->fifo_st->value | set) & ~(x->m_vld | x->m_rdy);
    if (x->ntx < 8)
        v |= x->m_rdy;
    if (v != x->fifo_st->value)
        rf_hw(x->fifo_st, v);
    if (!was && (v & irq))
        rf_interrupt(x->p, x->irq);
}

static void divide(struct sio *x, int sign)
{
    u32 n = (u32)x->udividend->value, d = (u32)x->udivisor->value, q, r;
    if (sign) {
        s32 sn = (s32)n, sd = (s32)d;
        if (!sd) {
            q = sn < 0 ? 1u : 0xffffffffu;
            r = n;
        } else if (sn == (s32)0x80000000 && sd == -1) {
            q = n;
            r = 0;
        } else {
            q = (u32)(sn / sd);
            r = (u32)(sn % sd);
        }
    } else if (!d) {
        q = 0xffffffffu;
        r = n;
    } else {
        q = n / d;
        r = n % d;
    }
    x->quot->value = q;
    x->rem->value = r;
    x->div_at = x->s->cycles;
    x->div_running = 1;
}

static void div_csr(struct sio *x, int dirty)
{
    u64 v = x->csr->value & ~x->m_ready;
    if (!x->div_running || x->s->cycles - x->div_at >= 8) {
        v |= x->m_ready;
        x->div_running = 0;
    }
    if (dirty > 0)
        v |= x->m_dirty;
    if (dirty < 0)
        v &= ~x->m_dirty;
    x->csr->value = v;
}

static void pins(struct sio *x)
{
    rp2040_sio_pins(x->s, (u32)x->out->value & 0x3fffffff, (u32)x->oe->value & 0x3fffffff,
                    (u32)x->hout->value & 0x3f, (u32)x->hoe->value & 0x3f);
}

static u32 sio_read(void *ctx, u32 off, int n)
{
    struct sio *x = ctx;
    struct sim *s = x->s;
    int side = !s->bus.debug;
    if (off == rf_off(x->gin))
        x->gin->value = rp2040_gpio_in(s);
    else if (off == rf_off(x->ghin))
        x->ghin->value = rp2040_gpio_hi_in(s);
    else if (off == rf_off(x->csr))
        div_csr(x, 0);
    else if (off == rf_off(x->lock_st))
        x->lock_st->value = x->locks;
    else if (off >= rf_off(x->lock[0]) && off <= rf_off(x->lock[31])) {
        int k = (int)((off - rf_off(x->lock[0])) / 4);
        u32 v = x->locks >> k & 1 ? 0 : 1u << k;
        if (side)
            x->locks |= 1u << k;
        return v;
    } else if (off == rf_off(x->fifo_rd)) {
        /* the incoming FIFO: core 1 is not there, so it is empty */
        if (side)
            fifo_status(x, x->m_roe);
        return 0;
    }
    u32 v = rf_read(x->p, off, n);
    if (off == rf_off(x->quot) && side) {
        div_csr(x, -1);
    }
    return v;
}

/* a SET, CLR or XOR alias of `r` */
static void alias(struct rf_reg *r, struct rf_reg *set, struct rf_reg *clr,
                  struct rf_reg *xor, u32 off, u32 v)
{
    if (off == rf_off(set))
        r->value |= v;
    else if (off == rf_off(clr))
        r->value &= ~(u64)v;
    else if (off == rf_off(xor))
        r->value ^= v;
}

static void sio_write(void *ctx, u32 off, int n, u32 v)
{
    struct sio *x = ctx;
    if (off >= rf_off(x->lock[0]) && off <= rf_off(x->lock[31])) {
        x->locks &= ~(1u << (off - rf_off(x->lock[0])) / 4);
        return;
    }
    if (off == rf_off(x->fifo_wr)) {
        u64 set = 0;
        if (x->ntx < 8)
            x->tx[x->ntx++] = v;
        else
            set = x->m_wof;         /* written while full: lost */
        fifo_status(x, set);
        return;
    }
    rf_write(x->p, off, n, v);
    alias(x->out, x->out_set, x->out_clr, x->out_xor, off, v);
    alias(x->oe, x->oe_set, x->oe_clr, x->oe_xor, off, v);
    alias(x->hout, x->hout_set, x->hout_clr, x->hout_xor, off, v);
    alias(x->hoe, x->hoe_set, x->hoe_clr, x->hoe_xor, off, v);
    if (off >= rf_off(x->out) && off <= rf_off(x->hoe_xor))
        pins(x);
    if (off == rf_off(x->udividend) || off == rf_off(x->udivisor)) {
        x->sdividend->value = x->udividend->value;
        x->sdivisor->value = x->udivisor->value;
        divide(x, 0);
        div_csr(x, 1);
    } else if (off == rf_off(x->sdividend) || off == rf_off(x->sdivisor)) {
        x->udividend->value = x->sdividend->value;
        x->udivisor->value = x->sdivisor->value;
        divide(x, 1);
        div_csr(x, 1);
    } else if (off == rf_off(x->quot) || off == rf_off(x->rem)) {
        x->div_running = 0;
        div_csr(x, 1);
    }
    if (off == rf_off(x->fifo_st))
        fifo_status(x, 0);
}

static void sio_reset(void *ctx)
{
    struct sio *x = ctx;
    x->locks = 0;
    x->ntx = 0;
    x->div_running = 0;
    fifo_status(x, 0);
    pins(x);
}

const struct dev_ops rp2040_sio_ops = {
    "rp2040-sio", sio_read, sio_write, sio_reset, 0, 0,
};

void *rp2040_sio_create(struct sim *s, struct rf_periph *p)
{
    struct sio *x = calloc(1, sizeof *x);
    struct { struct rf_reg **r; const char *name; } regs[] = {
        { &x->cpuid, "CPUID" }, { &x->gin, "GPIO_IN" }, { &x->ghin, "GPIO_HI_IN" },
        { &x->out, "GPIO_OUT" }, { &x->out_set, "GPIO_OUT_SET" },
        { &x->out_clr, "GPIO_OUT_CLR" }, { &x->out_xor, "GPIO_OUT_XOR" },
        { &x->oe, "GPIO_OE" }, { &x->oe_set, "GPIO_OE_SET" },
        { &x->oe_clr, "GPIO_OE_CLR" }, { &x->oe_xor, "GPIO_OE_XOR" },
        { &x->hout, "GPIO_HI_OUT" }, { &x->hout_set, "GPIO_HI_OUT_SET" },
        { &x->hout_clr, "GPIO_HI_OUT_CLR" }, { &x->hout_xor, "GPIO_HI_OUT_XOR" },
        { &x->hoe, "GPIO_HI_OE" }, { &x->hoe_set, "GPIO_HI_OE_SET" },
        { &x->hoe_clr, "GPIO_HI_OE_CLR" }, { &x->hoe_xor, "GPIO_HI_OE_XOR" },
        { &x->fifo_st, "FIFO_ST" }, { &x->fifo_wr, "FIFO_WR" }, { &x->fifo_rd, "FIFO_RD" },
        { &x->lock_st, "SPINLOCK_ST" }, { &x->udividend, "DIV_UDIVIDEND" },
        { &x->udivisor, "DIV_UDIVISOR" }, { &x->sdividend, "DIV_SDIVIDEND" },
        { &x->sdivisor, "DIV_SDIVISOR" }, { &x->quot, "DIV_QUOTIENT" },
        { &x->rem, "DIV_REMAINDER" }, { &x->csr, "DIV_CSR" },
    };
    if (!x)
        die("out of memory");
    x->s = s;
    x->p = p;
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if (!(*regs[i].r = rf_reg(p, regs[i].name)))
            die("RP2040: SIO has no %s in %s", regs[i].name, rf_device_name(s->svd));
    for (int k = 0; k < 32; k++) {
        char nm[16];
        snprintf(nm, sizeof nm, "SPINLOCK%d", k);
        if (!(x->lock[k] = rf_reg(p, nm)))
            die("RP2040: SIO has no %s", nm);
        if (k && rf_off(x->lock[k]) != rf_off(x->lock[0]) + 4u * (u32)k)
            die("RP2040: SIO's spinlocks are not one after another");
    }
    x->m_vld = rf_mask(x->fifo_st, "VLD");
    x->m_rdy = rf_mask(x->fifo_st, "RDY");
    x->m_wof = rf_mask(x->fifo_st, "WOF");
    x->m_roe = rf_mask(x->fifo_st, "ROE");
    x->m_ready = rf_mask(x->csr, "READY");
    x->m_dirty = rf_mask(x->csr, "DIRTY");
    x->irq = rf_irq(p, "SIO_IRQ_PROC0");
    rf_set_reset(x->cpuid, 0);
    return x;
}
