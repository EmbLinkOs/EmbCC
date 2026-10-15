/* pic24-rtcc.c -- a PIC24FJ's real-time clock and calendar (the EDC's
 * RTCC: RTCCON1L to RTCSTATL, TIMEL/H, DATEL/H, the alarm's and the
 * timestamp's registers), as the data sheet's section 22 has it.
 *
 *   - With RTCCON1L.RTCEN set it counts half seconds from CLKSEL's clock:
 *     the SOSC (32.768 kHz), the LPRC (31 kHz) or the system clock (Fosc,
 *     twice Fcy), divided by PS (1, 16, 64, 256) and by DIV + 1
 *     (RTCCON2H; 0x3FFF out of reset, half a second of the SOSC);
 *     RTCSTATL.HALFSEC is the second half of a second. The time and date
 *     are BCD: seconds to years, 2000 to 2099 with their leap years, the
 *     weekday counting 0 to 6 with the day;
 *   - WRLOCK set ignores writes to RTCEN, RTCCON2L/H and the time and
 *     date; clearing it takes NVMKEY's unlock (0x55, 0xAA) just before,
 *     setting it does not. Writing TIMEL clears HALFSEC;
 *   - the alarm: with ALRMEN set, the time and date matching ALMTIME and
 *     ALMDATE in the fields AMASK names (each half second, second, 10
 *     seconds, minute, 10 minutes, hour, day, week, month, year) sets
 *     RTCSTATL.ALMEVT and the RTCC interrupt's flag; ALMRPT counts the
 *     repeats down, CHIME lets it wrap, and ALRMEN clears when it is
 *     done;
 *   - writing 1 to RTCSTATL.TSAEVT takes a timestamp: the time and date
 *     into TSATIME and TSADATE, and the RTCCTS interrupt's flag. ALMEVT
 *     and TSAEVT are cleared by writing 0.
 * Not modelled: FDIV's fine division, the power control output, the
 * PWRLCLK pin's clock (no clock: it does not count), the TMPR pin. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define RTCEN       (1u << 15)
#define WRLOCK      (1u << 11)
#define ALRMEN      (1u << 15)
#define CHIME       (1u << 14)
#define ST_ALMEVT   (1u << 5)
#define ST_TSAEVT   (1u << 3)
#define ST_HALFSEC  1u

struct rtcc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con1l, *con1h, *con2l, *con2h, *stat;
    struct rf_reg *timel, *timeh, *datel, *dateh;
    struct rf_reg *atimel, *atimeh, *adatel, *adateh;
    struct rf_reg *tstimel, *tstimeh, *tsdatel, *tsdateh;
    int irq, ts_irq;
    u64 acc;                        /* clock pulses times Fcy, toward a half second */
    int running;
};

static const u32 prescale[4] = { 1, 16, 64, 256 };

static u32 bcd(u32 v) { return (v / 10) << 4 | v % 10; }
static u32 dec(u32 v) { return (v >> 4) * 10 + (v & 15); }

static int on(const struct rtcc *m)
{
    return (m->con1l->value & RTCEN) && (m->con2l->value & 3) != 2;
}

/* the clock's pulses a second */
static u32 fsrc(const struct rtcc *m)
{
    switch (m->con2l->value & 3) {
    case 0: return 32768;
    case 1: return 31000;
    case 3: return 2 * pic24_fcy(m->s);
    }
    return 0;
}

static int days_in(u32 month, u32 year)
{
    static const u8 d[13] = { 31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && year % 4 == 0)
        return 29;
    return d[month <= 12 ? month : 0];
}

/* a second on: the BCD carries up to the year */
static void second(struct rtcc *m)
{
    u32 tl = (u32)m->timel->value, th = (u32)m->timeh->value;
    u32 dl = (u32)m->datel->value, dh = (u32)m->dateh->value;
    u32 sec = dec(tl >> 8 & 0x7f), min = dec(th & 0x7f), hr = dec(th >> 8 & 0x3f);
    u32 wday = dl & 7, day = dec(dl >> 8 & 0x3f), mon = dec(dh & 0x1f), yr = dec(dh >> 8);
    if (++sec == 60) {
        sec = 0;
        if (++min == 60) {
            min = 0;
            if (++hr == 24) {
                hr = 0;
                wday = wday >= 6 ? 0 : wday + 1;
                if (++day > (u32)days_in(mon, yr)) {
                    day = 1;
                    if (++mon > 12) {
                        mon = 1;
                        yr = (yr + 1) % 100;
                    }
                }
            }
        }
    }
    rf_hw(m->timel, bcd(sec) << 8);
    rf_hw(m->timeh, bcd(hr) << 8 | bcd(min));
    rf_hw(m->datel, bcd(day) << 8 | wday);
    rf_hw(m->dateh, bcd(yr) << 8 | bcd(mon));
}

/* the fields AMASK compares: TIMEL, TIMEH, DATEL, DATEH's masks */
static int alarm_match(const struct rtcc *m, int whole)
{
    static const u16 mask[10][4] = {
        { 0, 0, 0, 0 },                         /* each half second */
        { 0, 0, 0, 0 },                         /* each second */
        { 0x0f00, 0, 0, 0 },                    /* 10 seconds: SECONE */
        { 0x7f00, 0, 0, 0 },                    /* a minute */
        { 0x7f00, 0x000f, 0, 0 },               /* 10 minutes */
        { 0x7f00, 0x007f, 0, 0 },               /* an hour */
        { 0x7f00, 0x3f7f, 0, 0 },               /* a day */
        { 0x7f00, 0x3f7f, 0x0007, 0 },          /* a week */
        { 0x7f00, 0x3f7f, 0x3f00, 0 },          /* a month */
        { 0x7f00, 0x3f7f, 0x3f00, 0x001f },     /* a year */
    };
    u32 am = (u32)(m->con1h->value >> 8 & 15);
    if (am > 9)
        return 0;
    if (am >= 1 && !whole)
        return 0;                               /* on the second */
    const u16 *k = mask[am];
    return ((m->timel->value ^ m->atimel->value) & k[0]) == 0 &&
           ((m->timeh->value ^ m->atimeh->value) & k[1]) == 0 &&
           ((m->datel->value ^ m->adatel->value) & k[2]) == 0 &&
           ((m->dateh->value ^ m->adateh->value) & k[3]) == 0;
}

static void alarm(struct rtcc *m)
{
    u32 h = (u32)m->con1h->value, rpt = h & 0xff;
    rf_hw(m->stat, m->stat->value | ST_ALMEVT);
    if (m->irq >= 0)
        pic24_irq(m->s, m->irq);
    if (rpt)
        h = (h & ~0xffu) | (rpt - 1);
    else if (h & CHIME)
        h |= 0xff;
    else
        h &= ~ALRMEN;
    rf_hw(m->con1h, h);
}

static void half_second(struct rtcc *m)
{
    int whole = (m->stat->value & ST_HALFSEC) != 0;
    rf_hw(m->stat, m->stat->value ^ ST_HALFSEC);
    if (whole)
        second(m);
    if ((m->con1h->value & ALRMEN) && alarm_match(m, whole))
        alarm(m);
}

static u64 half_period(const struct rtcc *m)
{
    return (u64)prescale[m->con2l->value >> 4 & 3] * ((m->con2h->value & 0xffff) + 1) *
           pic24_fcy(m->s);
}

static void rtcc_tick(void *ctx, u32 cycles)
{
    struct rtcc *m = ctx;
    if (!on(m))
        return;
    u64 per = half_period(m);
    m->acc += (u64)cycles * fsrc(m);
    while (per && m->acc >= per) {
        m->acc -= per;
        half_second(m);
    }
}

/* the next half second, when an alarm could come of it */
static int rtcc_next(void *ctx, u32 *cycles)
{
    struct rtcc *m = ctx;
    if (!on(m) || !(m->con1h->value & ALRMEN) || !pic24_irq_enabled(m->s, m->irq))
        return 0;
    u64 per = half_period(m), f = fsrc(m);
    if (!f)
        return 0;
    u64 c = (per - m->acc + f - 1) / f;
    *cycles = c > 0xffffffffu ? 0xffffffffu : (u32)c;
    return 1;
}

static void timestamp(struct rtcc *m)
{
    if (m->tstimel) {
        rf_hw(m->tstimel, m->timel->value);
        rf_hw(m->tstimeh, m->timeh->value);
        rf_hw(m->tsdatel, m->datel->value);
        rf_hw(m->tsdateh, m->dateh->value);
    }
    rf_hw(m->stat, m->stat->value | ST_TSAEVT);
    if (m->ts_irq >= 0)
        pic24_irq(m->s, m->ts_irq);
}

static u32 rtcc_read(void *ctx, u32 off, int n)
{
    struct rtcc *m = ctx;
    return rf_read(m->p, off, n);
}

static void rtcc_write(void *ctx, u32 off, int n, u32 v)
{
    struct rtcc *m = ctx;
    u32 locked = (u32)m->con1l->value & WRLOCK;
    if (off == rf_off(m->con1l)) {
        u64 was = m->con1l->value;
        rf_write(m->p, off, n, v);
        u64 now = m->con1l->value;
        if (locked && !(now & WRLOCK) && !pic24_unlocked(m->s))
            now |= WRLOCK;                      /* the unlock is wanted */
        if ((was & WRLOCK) && (now & WRLOCK))
            now = (now & ~(u64)RTCEN) | (was & RTCEN);
        rf_hw(m->con1l, now);
    } else if (off == rf_off(m->stat)) {
        /* ALMEVT and TSAEVT: cleared by a 0; TSAEVT's 1 takes a stamp */
        u32 x = n == 2 ? v : (off & 1 ? v << 8 : v);
        u64 st = m->stat->value;
        st &= ~(u64)((~x) & (ST_ALMEVT | ST_TSAEVT));
        rf_hw(m->stat, st);
        if (x & ST_TSAEVT)
            timestamp(m);
    } else if (locked && (off == rf_off(m->con2l) || off == rf_off(m->con2h) ||
                          off == rf_off(m->timel) || off == rf_off(m->timeh) ||
                          off == rf_off(m->datel) || off == rf_off(m->dateh))) {
        return;                                 /* WRLOCK */
    } else {
        rf_write(m->p, off, n, v);
        if (off == rf_off(m->timel)) {
            rf_hw(m->stat, m->stat->value & ~(u64)ST_HALFSEC);
            m->acc = 0;
        }
    }
    rf_clock(m->p, &m->running, on(m));
}

static void rtcc_reset(void *ctx)
{
    struct rtcc *m = ctx;
    m->acc = 0;
    m->running = 0;
}

const struct dev_ops pic24_rtcc_ops = {
    "pic24-rtcc", rtcc_read, rtcc_write, rtcc_reset, rtcc_tick, rtcc_next,
};

void *pic24_rtcc_create(struct sim *s, struct rf_periph *p)
{
    struct rtcc *m = calloc(1, sizeof *m);
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
#define R(f, name) m->f = rf_reg(p, name)
    R(con1l, "RTCCON1L"); R(con1h, "RTCCON1H"); R(con2l, "RTCCON2L");
    R(con2h, "RTCCON2H"); R(stat, "RTCSTATL");
    R(timel, "TIMEL"); R(timeh, "TIMEH"); R(datel, "DATEL"); R(dateh, "DATEH");
    R(atimel, "ALMTIMEL"); R(atimeh, "ALMTIMEH"); R(adatel, "ALMDATEL");
    R(adateh, "ALMDATEH"); R(tstimel, "TSATIMEL"); R(tstimeh, "TSATIMEH");
    R(tsdatel, "TSADATEL"); R(tsdateh, "TSADATEH");
#undef R
    if (!m->con1l || !m->con1h || !m->con2l || !m->con2h || !m->stat || !m->timel ||
        !m->timeh || !m->datel || !m->dateh || !m->atimel || !m->atimeh ||
        !m->adatel || !m->adateh) {
        free(m);
        return 0;
    }
    if (!m->tstimeh || !m->tsdatel || !m->tsdateh)
        m->tstimel = 0;
    m->irq = pic24_irq_named(s, "RTCC");
    m->ts_irq = pic24_irq_named(s, "RTCCTS");
    /* the hardware's status bits: the program's writes are the model's */
    rf_set_acc(m->stat, 0xffff, ACC_RO);
    return m;
}
