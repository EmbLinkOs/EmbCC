/* avr-timer16.c -- the ATmega328P's Timer/Counter1, the 16-bit timer:
 * TCCR1A/B/C, TCNT1, ICR1, OCR1A and OCR1B at data 0x80, TIMSK1 at 0x6F
 * and TIFR1 at 0x36.
 *
 * It counts the core's cycles through the prescaler (CS12:10: stopped,
 * /1, /8, /64, /256, /1024; an external clock on T1 does not count),
 * which runs free as the part's does, so a timer started mid-way has a
 * partial first period. Every waveform mode counts as the datasheet
 * says -- normal, CTC with OCR1A or ICR1 as TOP, fast PWM and the
 * dual-slope ones -- and sets its flags: TOV1 at MAX (normal and CTC),
 * at TOP (fast PWM) or at BOTTOM (dual-slope); OCF1A and OCF1B when
 * TCNT1 reaches the compare value; ICF1 at TOP when ICR1 is TOP. The
 * flags clear by writing them a one, or as their vector (10 to 13) is
 * entered. The output pins and input capture from a pin are not
 * modelled. The 16-bit registers are reached through TEMP, the high
 * byte's latch, as on the part. */
#include <stdlib.h>

#include "avr.h"
#include "devices.h"

#define TOV 1
#define OCFA 2
#define OCFB 4
#define ICF 32

struct t16 {
    struct sim *sim;
    u8 ccra, ccrb, ccrc, timsk, tifr, temp;
    u16 tcnt, ocra, ocrb, icr;
    int down;                               /* a dual-slope count going down */
    int running;
};

static int wgm(const struct t16 *t)
{
    return ((t->ccrb >> 1) & 0xc) | (t->ccra & 3);
}

static u32 prescale(const struct t16 *t)
{
    static const u32 div[8] = { 0, 1, 8, 64, 256, 1024, 0, 0 };
    return div[t->ccrb & 7];
}

static u16 top(const struct t16 *t)
{
    switch (wgm(t)) {
    case 1: case 5: return 0xff;
    case 2: case 6: return 0x1ff;
    case 3: case 7: return 0x3ff;
    case 4: case 9: case 11: case 15: return t->ocra;
    case 8: case 10: case 12: case 14: return t->icr;
    }
    return 0xffff;
}

static int dual(const struct t16 *t)
{
    int m = wgm(t);
    return (m >= 1 && m <= 3) || (m >= 8 && m <= 11);
}

/* One timer clock on a copy of the counter: the flags it sets. */
static u8 clock1(const struct t16 *t, u16 *tcnt, int *down)
{
    int m = wgm(t);
    u16 tp = top(t);
    u8 f = 0;
    if (dual(t)) {
        if (!*down) {
            if (*tcnt >= tp) {
                *down = 1;
                *tcnt = (u16)(*tcnt - 1);
            } else {
                (*tcnt)++;
            }
        } else if (*tcnt == 0) {
            *down = 0;
            (*tcnt)++;
        } else {
            (*tcnt)--;
            if (*tcnt == 0)
                f |= TOV;                   /* at BOTTOM */
        }
    } else if (*tcnt == tp) {
        *tcnt = 0;
        if (m == 0 || ((m == 4 || m == 12) && tp == 0xffff))
            f |= TOV;                       /* MAX to BOTTOM */
    } else {
        (*tcnt)++;
    }
    if (!dual(t) && m >= 5 && *tcnt == tp)
        f |= TOV;                           /* fast PWM: at TOP */
    if (*tcnt == t->ocra)
        f |= OCFA;
    if (*tcnt == t->ocrb)
        f |= OCFB;
    if ((m == 8 || m == 10 || m == 12 || m == 14) && *tcnt == tp)
        f |= ICF;
    return f;
}

static void t_tick(void *ctx, u32 cycles)
{
    struct t16 *t = ctx;
    u32 n = prescale(t);
    if (!n)
        return;
    /* the prescaler runs free: the timer clocks between the cycles
     * before this instruction and after it */
    u64 now = t->sim->cycles, before = now - cycles;
    u64 k = now / n - before / n;
    u8 f = 0;
    while (k--)
        f |= clock1(t, &t->tcnt, &t->down);
    if (f & ~t->tifr) {
        t->tifr |= f;
        avr_irq_changed(t->sim);
    }
}

/* the cycles until a flag whose interrupt is enabled is set */
static int t_next(void *ctx, u32 *cycles)
{
    struct t16 *t = ctx;
    u32 n = prescale(t);
    u8 en = t->timsk & (TOV | OCFA | OCFB | ICF);
    if (!n || !en)
        return 0;
    if (t->tifr & en) {
        *cycles = 0;
        return 1;
    }
    u16 c = t->tcnt;
    int down = t->down;
    for (u32 k = 1; k <= 0x20002; k++)
        if (clock1(t, &c, &down) & en) {
            u64 first = n - t->sim->cycles % n;
            u64 left = first + (u64)(k - 1) * n;
            *cycles = left > 0xffffffffu ? 0xffffffffu : (u32)left;
            return 1;
        }
    return 0;
}

static u8 rd(struct t16 *t, u32 off)
{
    switch (off) {
    case 0x0: return t->ccra;
    case 0x1: return t->ccrb;
    case 0x2: return t->ccrc;
    case 0x4: t->temp = (u8)(t->tcnt >> 8); return (u8)t->tcnt;
    case 0x6: t->temp = (u8)(t->icr >> 8); return (u8)t->icr;
    case 0x5: case 0x7: return t->temp;
    case 0x8: return (u8)t->ocra;
    case 0x9: return (u8)(t->ocra >> 8);
    case 0xa: return (u8)t->ocrb;
    case 0xb: return (u8)(t->ocrb >> 8);
    }
    return 0;
}

static void wr(struct t16 *t, u32 off, u8 v)
{
    switch (off) {
    case 0x0: t->ccra = v & 0xf3; break;
    case 0x1:
        t->ccrb = v & 0xdf;
        sim_clock(t->sim, &t->running, prescale(t) != 0);
        break;
    case 0x2: t->ccrc = 0; break;               /* FOC1A/B: the pins only */
    case 0x5: case 0x7: case 0x9: case 0xb: t->temp = v; break;
    case 0x4: t->tcnt = (u16)(t->temp << 8 | v); break;
    case 0x6: t->icr = (u16)(t->temp << 8 | v); break;
    case 0x8: t->ocra = (u16)(t->temp << 8 | v); break;
    case 0xa: t->ocrb = (u16)(t->temp << 8 | v); break;
    }
}

static u32 t_read(void *ctx, u32 off, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u32)rd(ctx, off + (u32)i) << (8 * i);
    return v;
}

static void t_write(void *ctx, u32 off, int n, u32 v)
{
    for (int i = 0; i < n; i++)
        wr(ctx, off + (u32)i, (u8)(v >> (8 * i)));
}

/* TIFR1 and TIMSK1, a byte each elsewhere in the I/O space */
static u32 tifr_read(void *ctx, u32 off, int n)
{
    (void)n;
    return off ? 0 : ((struct t16 *)ctx)->tifr;
}

static void tifr_write(void *ctx, u32 off, int n, u32 v)
{
    struct t16 *t = ctx;
    (void)n;
    if (!off) {
        t->tifr &= (u8)~(v & 0x27);            /* a one clears the flag */
        avr_irq_changed(t->sim);
    }
}

static u32 timsk_read(void *ctx, u32 off, int n)
{
    (void)n;
    return off ? 0 : ((struct t16 *)ctx)->timsk;
}

static void timsk_write(void *ctx, u32 off, int n, u32 v)
{
    struct t16 *t = ctx;
    (void)n;
    if (!off) {
        t->timsk = (u8)(v & 0x27);
        avr_irq_changed(t->sim);
    }
}

static void t_reset(void *ctx)
{
    struct t16 *t = ctx;
    struct sim *s = t->sim;
    sim_clock(s, &t->running, 0);
    *t = (struct t16){ 0 };
    t->sim = s;
}

/* the vectors: 10 capture, 11 compare A, 12 compare B, 13 overflow */
static u8 vec_flag(int vec)
{
    return vec == 10 ? ICF : vec == 11 ? OCFA : vec == 12 ? OCFB : TOV;
}

static int t_pending(void *ctx, int vec)
{
    struct t16 *t = ctx;
    return (t->tifr & t->timsk & vec_flag(vec)) != 0;
}

static void t_ack(void *ctx, int vec)
{
    ((struct t16 *)ctx)->tifr &= (u8)~vec_flag(vec);
}

static const struct dev_ops tifr_ops = { "avr-timer1-tifr", tifr_read, tifr_write, 0, 0, 0 };
static const struct dev_ops timsk_ops = { "avr-timer1-timsk", timsk_read, timsk_write, 0, 0, 0 };

const struct dev_ops avr_timer16_ops = {
    "avr-timer16", t_read, t_write, t_reset, t_tick, t_next,
};

/* at TCCR1A's address; TIFR1 and TIMSK1 are added at theirs */
void *avr_timer16_create(struct sim *s, const struct dev_desc *d)
{
    struct t16 *t = calloc(1, sizeof *t);
    if (!t)
        die("out of memory");
    t->sim = s;
    bus_add_device(&s->bus, d->base - 0x80 + 0x36, 1, &tifr_ops, t);
    bus_add_device(&s->bus, d->base - 0x80 + 0x6f, 1, &timsk_ops, t);
    for (int v = 10; v <= 13; v++)
        avr_irq_source(s, v, t_pending, t_ack, t);
    return t;
}
