/* pic24-adc.c -- a PIC24FJ's 12-bit A/D converter (ADC1), over the EDC's
 * registers (AD1CON1-AD1CON5, AD1CHS, AD1CSSL, AD1CSSH, ADC1BUF0-25).
 *
 *   - With AD1CON1.ADON: setting SAMP (or ASAM, after each conversion)
 *     starts sampling; the conversion starts when SAMP is cleared by the
 *     program (SSRC 0000) or after SAMC TADs of sampling (SSRC 0111,
 *     auto-convert), and takes 12 TADs (10-bit) or 14 (MODE12). A TAD is
 *     ADCS + 1 instruction cycles, or 250 ns of the ADC's RC clock with
 *     ADRC.
 *   - The input: CH0SA, or with CSCNA the next of the inputs AD1CSSL and
 *     AD1CSSH select, in ascending order. AN0-AN23 are the board's pins
 *     (struct board_io's analog, 0 V where nothing is connected); 28 the
 *     band gap (1.2 V), 29 AVSS, 30 AVDD (3.3 V, the reference); 24 and
 *     25 (the CTMU's) 0 V.
 *   - The result, against AVDD at 3.3 V, truncated to 10 or 12 bits, in
 *     the format FORM gives (integer or fractional, unsigned or signed),
 *     goes to the next buffer of the FIFO (ADC1BUF0 up), or to the
 *     input's own (BUFREGEN). DONE is set. After SMPI + 1 conversions the
 *     ADC1 interrupt is flagged and the FIFO starts again at ADC1BUF0
 *     (at ADC1BUF13 every other time with BUFM, BUFS saying which).
 * Not modelled: the other trigger sources (timers, INT0, CTMU), the
 * clock synchronisation time, DMA, the threshold detect and auto-scan of
 * AD1CON5, ALTS's sample B. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NBUF 26
#define AVDD 3.3

struct adc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con1, *con2, *con3, *chs, *cssl, *cssh, *buf[NBUF];
    int irq;
    int running;
    int phase;                      /* 0 idle, 1 sampling, 2 converting */
    u64 left;                       /* cycles of the phase */
    int scan;                       /* the scan list's next */
    int fill;                       /* the FIFO's next */
    int count;                      /* conversions since the interrupt */
    int half;                       /* BUFM: the upper half's turn */
    int in;                         /* the input converting */
};

#define CON1_ADON  (1u << 15)
#define CON1_SAMP  (1u << 1)
#define CON1_DONE  (1u << 0)
#define CON1_ASAM  (1u << 2)

static u64 tad(const struct adc *a)
{
    u64 c3 = a->con3->value;
    if (c3 & 0x8000) {                          /* ADRC: 4 MHz */
        u64 t = pic24_fcy(a->s) / 4000000u;
        return t ? t : 1;
    }
    return (c3 & 0xff) + 1;
}

static void set_clock(struct adc *a)
{
    rf_clock(a->p, &a->running, a->phase != 0);
}

/* the next input: CH0SA, or the scan list's next */
static int next_input(struct adc *a)
{
    if (!(a->con2->value & 0x0400))             /* CSCNA */
        return (int)(a->chs->value & 0x1f);
    u32 sel = (u32)(a->cssl ? a->cssl->value : 0) |
              (u32)(a->cssh ? a->cssh->value : 0) << 16;
    if (!sel)
        return (int)(a->chs->value & 0x1f);
    for (int k = 0; k < 32; k++) {
        int in = (a->scan + k) % 32;
        if (sel >> in & 1) {
            a->scan = in + 1;
            return in;
        }
    }
    return 0;
}

static void start_sampling(struct adc *a)
{
    rf_hw(a->con1, a->con1->value | CON1_SAMP);
    a->phase = 1;
    u64 samc = a->con3->value >> 8 & 0x1f;
    a->left = (samc ? samc : 1) * tad(a);
    set_clock(a);
}

static void start_conversion(struct adc *a)
{
    rf_hw(a->con1, a->con1->value & ~(u64)(CON1_SAMP | CON1_DONE));
    a->in = next_input(a);
    a->phase = 2;
    a->left = ((a->con1->value & 0x0400) ? 14 : 12) * tad(a);
    set_clock(a);
}

static double volts(struct adc *a, int in)
{
    struct board_io *io = &a->s->io;
    if (in < 24) {
        double v = io->analog ? io->analog(io->ctx, in) : 0.0;
        return v < 0 ? 0.0 : v;
    }
    switch (in) {
    case 28: return 1.2;
    case 30: return AVDD;
    }
    return 0.0;
}

static void finish(struct adc *a)
{
    u64 c1 = a->con1->value, c2 = a->con2->value;
    int bits = (c1 & 0x0400) ? 12 : 10;
    double v = volts(a, a->in);
    u32 full = 1u << bits;
    u32 code = v >= AVDD ? full - 1 : (u32)(v / AVDD * full);
    int form = (int)(c1 >> 8 & 3);
    u32 r = code;
    if (form & 1)                               /* signed: about mid-scale */
        r = (u32)((int32_t)code - (int32_t)(full / 2));
    if (form & 2)                               /* fractional: left-justified */
        r <<= 16 - bits;
    r &= 0xffff;
    int slot;
    if (c2 & 0x0800)                            /* BUFREGEN: the input's own */
        slot = a->in;
    else
        slot = (a->half ? 13 : 0) + a->fill++;
    if (slot >= 0 && slot < NBUF && a->buf[slot])
        rf_hw(a->buf[slot], r);
    rf_hw(a->con1, a->con1->value | CON1_DONE);
    a->phase = 0;
    if (++a->count > (int)(c2 >> 2 & 0x1f)) {
        a->count = 0;
        a->fill = 0;
        if (c2 & 0x0002) {                      /* BUFM */
            a->half = !a->half;
            rf_hw(a->con2, (a->con2->value & ~0x80ull) | (a->half ? 0x80u : 0));
        }
        pic24_irq(a->s, a->irq);
    }
    if (a->con1->value & CON1_ASAM)
        start_sampling(a);
    set_clock(a);
}

static void adc_tick(void *ctx, u32 cycles)
{
    struct adc *a = ctx;
    u64 c = cycles;
    while (c && a->phase) {
        if (!(a->con1->value & CON1_ADON)) {
            a->phase = 0;
            break;
        }
        if (c < a->left) {
            a->left -= c;
            break;
        }
        c -= a->left;
        a->left = 0;
        if (a->phase == 1) {
            /* sampling: auto-convert ends it; else it goes on */
            if ((a->con1->value >> 4 & 15) == 7)
                start_conversion(a);
            else {
                a->left = ~0ull >> 1;
                break;
            }
        } else
            finish(a);
    }
    set_clock(a);
}

static int adc_next_event(void *ctx, u32 *cycles)
{
    struct adc *a = ctx;
    if (!a->phase || !(a->con1->value & CON1_ADON))
        return 0;
    u64 t = a->left;
    if (a->phase == 1) {
        if ((a->con1->value >> 4 & 15) != 7)
            return 0;
        t += ((a->con1->value & 0x0400) ? 14 : 12) * tad(a);
    }
    *cycles = t > 0xffffffffu ? 0xffffffffu : (u32)t;
    return 1;
}

static u32 adc_read(void *ctx, u32 off, int n)
{
    struct adc *a = ctx;
    return rf_read(a->p, off, n);
}

static void adc_write(void *ctx, u32 off, int n, u32 v)
{
    struct adc *a = ctx;
    u64 before = a->con1->value;
    rf_write(a->p, off, n, v);
    u32 c = rf_off(a->con1);
    if (off + (u32)n <= c || off >= c + 2)
        return;
    u64 now = a->con1->value;
    if (!(now & CON1_ADON)) {
        a->phase = 0;
        a->fill = a->count = a->scan = 0;
        set_clock(a);
        return;
    }
    int on = !(before & CON1_ADON);
    if ((now & CON1_SAMP) && !(before & CON1_SAMP) && a->phase == 0)
        start_sampling(a);
    else if (on && (now & CON1_ASAM) && a->phase == 0)
        start_sampling(a);
    else if (!(now & CON1_SAMP) && (before & CON1_SAMP) && a->phase == 1 &&
             (now >> 4 & 15) == 0)
        start_conversion(a);                    /* SAMP cleared: convert */
}

static void adc_reset(void *ctx)
{
    struct adc *a = ctx;
    a->phase = 0;
    a->left = 0;
    a->scan = a->fill = a->count = a->half = 0;
    a->running = 0;
}

const struct dev_ops pic24_adc_ops = {
    "pic24-adc", adc_read, adc_write, adc_reset, adc_tick, adc_next_event,
};

void *pic24_adc_create(struct sim *s, struct rf_periph *p)
{
    struct adc *a = calloc(1, sizeof *a);
    if (!a)
        die("out of memory");
    a->s = s;
    a->p = p;
    a->con1 = rf_reg(p, "AD1CON1");
    a->con2 = rf_reg(p, "AD1CON2");
    a->con3 = rf_reg(p, "AD1CON3");
    a->chs = rf_reg(p, "AD1CHS");
    a->cssl = rf_reg(p, "AD1CSSL");
    a->cssh = rf_reg(p, "AD1CSSH");
    if (!a->con1 || !a->con2 || !a->con3 || !a->chs) {
        free(a);
        return 0;
    }
    char nm[16];
    for (int k = 0; k < NBUF; k++) {
        snprintf(nm, sizeof nm, "ADC1BUF%d", k);
        a->buf[k] = rf_reg(p, nm);
    }
    rf_set_acc(a->con2, 0x80, ACC_RO);         /* BUFS: the hardware's */
    a->irq = pic24_irq_named(s, "ADC1");
    return a;
}
