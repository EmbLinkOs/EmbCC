/* pic32-adc.c -- a PIC32MZ's ADC (ADCHS), what a driver needs to convert
 * a channel and read its result, over the ATDF's registers.
 *
 *   - Turning the ADC on (ADCCON1.ON) makes its reference ready at once
 *     (ADCCON2.BGVRRDY set, REFFLT clear); enabling an ADC's analog
 *     circuit (ADCANCON.ANENx) makes it ready (WKRDYx).
 *   - A conversion of input n happens when it is triggered:
 *       - the global software trigger (ADCCON3.GSWTRG, and GLSWTRG)
 *         converts each input whose trigger source (ADCTRGx.TRGSRCn) is
 *         it (1), and with ADCCON1.STRGSRC 1, each the scan list
 *         (ADCCSSx) names;
 *       - ADCCON3.RQCNVRT converts ADINSEL's input.
 *     The input is converted only when its ADC is on: inputs 0-4 by the
 *     dedicated ADCs 0-4 (ADCCON3.DIGENx) -- each of which converts its
 *     alternate pin instead, AN45 to AN49, with ADCTRGMODE.SHxALT 01 --
 *     the others by the shared ADC7 (DIGEN7). The result is at once, the
 *     trigger bits cleared.
 *   - The result is the board's voltage on the input (struct board_io's
 *     analog: 0 when it gives none) against VREF+ of 3.3 V (AVDD), at the
 *     resolution ADCxTIME.SELRES (dedicated) or ADCCON1.SELRES (shared)
 *     gives -- 6, 8, 10 or 12 bits, left-justified with ADCCON1.FRACT --
 *     in ADCDATAn, with ADCDSTATx's ARDYn set and, when ADCGIRQENx
 *     enables it, the input's interrupt (ADC_DATAn) flagged. Reading
 *     ADCDATAn clears ARDYn.
 * Not modelled: conversion time, the early interrupts, the comparators,
 * the filters, differential and signed inputs, DMA, the turbo modes,
 * triggers other than the software's. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

#define NIN 64

struct adc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con1, *con2, *con3, *ancon, *trgmode;
    struct rf_reg *trg[16], *css[2], *dstat[2], *girqen[2];
    struct rf_reg *data[NIN], *time[5];
    int irq[NIN];
};

#define VREF 3.3

static int adc_on(const struct adc *a, int in)
{
    int unit = in < 5 ? in : 7;
    return (a->con1->value >> 15 & 1) && (a->con3->value >> (16 + unit) & 1);
}

static void convert(struct adc *a, int in)
{
    if (in < 0 || in >= NIN || !a->data[in] || !adc_on(a, in))
        return;
    struct board_io *io = &a->s->io;
    /* a dedicated ADC's alternate input: ADC0 converts AN45 with
     * ADCTRGMODE.SH0ALT 01, ADC1 AN46, and so on to ADC4 and AN49 */
    int pin = in;
    if (in < 5 && a->trgmode && ((a->trgmode->value >> (16 + 2 * in)) & 3) == 1)
        pin = 45 + in;
    double v = io->analog ? io->analog(io->ctx, pin) : 0.0;
    if (v < 0)
        v = 0;
    u64 sel = in < 5 && a->time[in] ? (a->time[in]->value >> 24) & 3
                                     : (a->con1->value >> 21) & 3;
    int bits = 6 + 2 * (int)sel;
    double full = (double)(1u << bits);
    u32 code = v >= VREF ? (1u << bits) - 1 : (u32)(v / VREF * full);
    if (code > (1u << bits) - 1)
        code = (1u << bits) - 1;
    if (a->con1->value >> 23 & 1)              /* FRACT */
        code <<= 16 - bits;
    rf_hw(a->data[in], code);
    struct rf_reg *st = a->dstat[in / 32];
    if (st)
        rf_hw(st, st->value | 1ull << (in % 32));
    struct rf_reg *en = a->girqen[in / 32];
    if (en && (en->value >> (in % 32) & 1))
        pic32_irq(a->s, a->irq[in]);
}

/* the global software trigger: each input whose source it is, and the
 * scan list when it starts the scan */
static void global_trigger(struct adc *a)
{
    for (int in = 0; in < NIN; in++) {
        struct rf_reg *t = a->trg[in / 4];
        if (t && ((t->value >> (8 * (in % 4))) & 0x1f) == 1)
            convert(a, in);
    }
    if (((a->con1->value >> 16) & 0x1f) == 1)
        for (int in = 0; in < NIN; in++)
            if (a->css[in / 32] && (a->css[in / 32]->value >> (in % 32) & 1))
                convert(a, in);
}

static u32 adc_read(void *ctx, u32 off, int n)
{
    struct adc *a = ctx;
    u32 v = rf_read(a->p, off, n);
    if (a->s->bus.debug)
        return v;
    for (int in = 0; in < NIN; in++)
        if (a->data[in] && off == rf_off(a->data[in])) {
            struct rf_reg *st = a->dstat[in / 32];
            if (st && (st->value >> (in % 32) & 1))
                rf_hw(st, st->value & ~(1ull << (in % 32)));
            break;
        }
    return v;
}

static void adc_write(void *ctx, u32 off, int n, u32 v)
{
    struct adc *a = ctx;
    rf_write(a->p, off, n, v);
    if (off == rf_off(a->con1)) {
        u64 c2 = a->con2->value;
        if (a->con1->value >> 15 & 1)
            c2 = (c2 | 1ull << 31) & ~(1ull << 30);
        else
            c2 &= ~(1ull << 31);
        if (c2 != a->con2->value)
            rf_hw(a->con2, c2);
    } else if (a->ancon && off == rf_off(a->ancon)) {
        u64 an = a->ancon->value, w = (an & 0xff) << 8;
        if ((an & 0xff00) != w)
            rf_hw(a->ancon, (an & ~0xff00ull) | w);
    } else if (off == rf_off(a->con3)) {
        u64 c3 = a->con3->value;
        if (c3 & (1u << 8))                     /* RQCNVRT */
            convert(a, (int)(c3 & 0x3f));
        if (c3 & (3u << 6))                     /* GSWTRG, GLSWTRG */
            global_trigger(a);
        if (c3 & (1u << 6 | 1u << 8))
            rf_hw(a->con3, a->con3->value & ~(u64)(1u << 6 | 1u << 8));
    }
}

const struct dev_ops pic32_adc_ops = {
    "pic32-adc", adc_read, adc_write, 0, 0, 0,
};

void *pic32_adc_create(struct sim *s, struct rf_periph *p)
{
    struct adc *a = calloc(1, sizeof *a);
    char nm[24];
    if (!a)
        die("out of memory");
    a->s = s;
    a->p = p;
    a->con1 = rf_reg(p, "ADCCON1");
    a->con2 = rf_reg(p, "ADCCON2");
    a->con3 = rf_reg(p, "ADCCON3");
    a->ancon = rf_reg(p, "ADCANCON");
    a->trgmode = rf_reg(p, "ADCTRGMODE");
    if (!a->con1 || !a->con2 || !a->con3) {
        free(a);
        return 0;
    }
    for (int i = 0; i < 16; i++) {
        snprintf(nm, sizeof nm, "ADCTRG%d", i + 1);
        a->trg[i] = rf_reg(p, nm);
    }
    for (int i = 0; i < 2; i++) {
        snprintf(nm, sizeof nm, "ADCCSS%d", i + 1);
        a->css[i] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "ADCDSTAT%d", i + 1);
        a->dstat[i] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "ADCGIRQEN%d", i + 1);
        a->girqen[i] = rf_reg(p, nm);
    }
    for (int i = 0; i < NIN; i++) {
        snprintf(nm, sizeof nm, "ADCDATA%d", i);
        a->data[i] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "ADC_DATA%d", i);
        a->irq[i] = pic32_irq_named(s, nm);
    }
    for (int i = 0; i < 5; i++) {
        snprintf(nm, sizeof nm, "ADC%dTIME", i);
        a->time[i] = rf_reg(p, nm);
    }
    /* the hardware's bits, which the file calls read-write */
    rf_set_acc(a->con2, 0xe0000000u, ACC_RO);           /* BGVRRDY REFFLT EOSRDY */
    if (a->ancon)
        rf_set_acc(a->ancon, 0xff00u, ACC_RO);          /* WKRDYx */
    for (int i = 0; i < 2; i++)
        if (a->dstat[i])
            rf_set_acc(a->dstat[i], ~0ull, ACC_RO);
    return a;
}
