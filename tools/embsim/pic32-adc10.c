/* pic32-adc10.c -- a PIC32MX's 10-bit A/D converter (ADC1), over the
 * device file's registers (AD1CON1-AD1CON3, AD1CHS, AD1CSSL, AD1PCFG,
 * ADC1BUF0-ADC1BUFF), as the family reference manual's section 17 has
 * it.
 *
 *   - With AD1CON1.ON: setting SAMP (or ASAM, again after each
 *     conversion) starts sampling; the conversion starts when the program
 *     clears SAMP (SSRC 000) or after SAMC TADs of sampling (SSRC 111,
 *     auto-convert), and takes 12 TADs. A TAD is 2 (ADCS + 1) peripheral
 *     bus clocks, or with ADRC the RC clock's, taken as 16 core cycles
 *     (200 ns at 80 MHz).
 *   - The input: CH0SA, or with CSCNA the next of those AD1CSSL selects,
 *     in ascending order; AN0-AN15 are the board's (struct board_io's
 *     analog, 0 V where nothing is connected).
 *   - The result, against AVDD at 3.3 V, truncated to 10 bits, in FORM's
 *     format (integer or fractional, unsigned or signed, 16 or 32 bits),
 *     goes to the next of ADC1BUF0-ADC1BUFF; DONE is set; after SMPI + 1
 *     conversions the ADC interrupt is flagged and the buffer starts again
 *     at ADC1BUF0 (at ADC1BUF8 every other time with BUFM, BUFS saying
 *     which); CLRASAM then clears ASAM.
 * Not modelled: the other trigger sources (Timer3, INT0, CTMU), ALTS's
 * MUX B, the offset calibration, VCFG's external references. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

#define NBUF 16
#define AVDD 3.3

#define CON1_ON      (1u << 15)
#define CON1_CLRASAM (1u << 4)
#define CON1_ASAM    (1u << 2)
#define CON1_SAMP    (1u << 1)
#define CON1_DONE    1u

struct adc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con1, *con2, *con3, *chs, *cssl, *buf[NBUF];
    int irq;
    int running;
    int phase;                      /* 0 idle, 1 sampling, 2 converting */
    u64 left;                       /* cycles of the phase */
    int scan, fill, count, half, in;
};

static u64 tad(const struct adc *a)
{
    u64 c3 = a->con3->value;
    if (c3 & 0x8000)                            /* ADRC */
        return 16;
    return 2 * ((c3 & 0xff) + 1) * pic32_pb_div(a->s, 3);
}

static int ssrc(const struct adc *a)
{
    return (int)(a->con1->value >> 5 & 7);
}

static void set_clock(struct adc *a)
{
    rf_clock(a->p, &a->running, a->phase != 0);
}

static int next_input(struct adc *a)
{
    u32 sel = a->cssl ? (u32)a->cssl->value & 0xffff : 0;
    if (!(a->con2->value & (1u << 10)) || !sel)  /* CSCNA */
        return (int)(a->chs->value >> 16 & 15);
    for (int k = 0; k < 16; k++) {
        int in = (a->scan + k) % 16;
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
    a->left = 12 * tad(a);
    set_clock(a);
}

static void finish(struct adc *a)
{
    struct board_io *io = &a->s->io;
    u64 c1 = a->con1->value, c2 = a->con2->value;
    double v = io->analog ? io->analog(io->ctx, a->in) : 0.0;
    u32 code = v <= 0 ? 0 : v >= AVDD ? 1023 : (u32)(v / AVDD * 1024);
    int form = (int)(c1 >> 8 & 7);
    u32 r = code;
    if (form & 1)                               /* signed, about mid-scale */
        r = (u32)((int32_t)code - 512);
    if (form & 2)                               /* fractional */
        r <<= (form & 4) ? 22 : 6;
    if (!(form & 4))
        r &= 0xffff;
    if ((form & 5) == 1)                        /* signed 16-bit: extended */
        r = (u32)(int32_t)(int16_t)r;
    int slot = (a->half ? 8 : 0) + a->fill++;
    if (slot < NBUF && a->buf[slot])
        rf_hw(a->buf[slot], r);
    rf_hw(a->con1, a->con1->value | CON1_DONE);
    a->phase = 0;
    if (++a->count > (int)(c2 >> 2 & 15)) {
        a->count = 0;
        a->fill = 0;
        if (c2 & 2) {                           /* BUFM */
            a->half = !a->half;
            rf_hw(a->con2, (a->con2->value & ~0x80ull) | (a->half ? 0x80u : 0));
        }
        if (a->con1->value & CON1_CLRASAM)
            rf_hw(a->con1, a->con1->value & ~(u64)CON1_ASAM);
        pic32_irq(a->s, a->irq);
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
        if (!(a->con1->value & CON1_ON)) {
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
            if (ssrc(a) == 7)
                start_conversion(a);
            else {
                a->left = ~0ull >> 1;           /* until SAMP clears */
                break;
            }
        } else {
            finish(a);
        }
    }
    set_clock(a);
}

static int adc_next_event(void *ctx, u32 *cycles)
{
    struct adc *a = ctx;
    if (!a->phase || !(a->con1->value & CON1_ON))
        return 0;
    u64 t = a->left;
    if (a->phase == 1) {
        if (ssrc(a) != 7)
            return 0;
        t += 12 * tad(a);
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
    if ((off & ~3u) != rf_off(a->con1))
        return;
    u64 now = a->con1->value;
    if (!(now & CON1_ON)) {
        a->phase = 0;
        a->fill = a->count = a->scan = 0;
        set_clock(a);
        return;
    }
    int on = !(before & CON1_ON);
    if ((now & CON1_SAMP) && !(before & CON1_SAMP) && a->phase == 0)
        start_sampling(a);
    else if ((now & CON1_ASAM) && (on || !(before & CON1_ASAM)) && a->phase == 0)
        start_sampling(a);
    else if (!(now & CON1_SAMP) && (before & CON1_SAMP) && a->phase == 1 && ssrc(a) == 0)
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

const struct dev_ops pic32_adc10_ops = {
    "pic32-adc10", adc_read, adc_write, adc_reset, adc_tick, adc_next_event,
};

void *pic32_adc10_create(struct sim *s, struct rf_periph *p)
{
    struct adc *a = calloc(1, sizeof *a);
    char nm[16];
    if (!a)
        die("out of memory");
    a->s = s;
    a->p = p;
    a->con1 = rf_reg(p, "AD1CON1");
    a->con2 = rf_reg(p, "AD1CON2");
    a->con3 = rf_reg(p, "AD1CON3");
    a->chs = rf_reg(p, "AD1CHS");
    a->cssl = rf_reg(p, "AD1CSSL");
    if (!a->con1 || !a->con2 || !a->con3 || !a->chs) {
        free(a);
        return 0;
    }
    for (int k = 0; k < NBUF; k++) {
        snprintf(nm, sizeof nm, "ADC1BUF%X", k);
        a->buf[k] = rf_reg(p, nm);
    }
    rf_set_acc(a->con2, 0x80, ACC_RO);         /* BUFS */
    a->irq = pic32_irq_named(s, "ADC");
    return a;
}
