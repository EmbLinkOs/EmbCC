/* esp32c3.c -- the ESP32-C3's peripherals EmbSim models, over Espressif's
 * esp32c3.svd, its Technical Reference Manual (v1.4) the reference:
 *
 *   - INTERRUPT_CORE0, the interrupt matrix and the CPU's interrupt
 *     controller (TRM 1.5, 8): each of the 62 peripheral sources' map
 *     register (at 4 x its number) routes it to a CPU interrupt, 1-31 (0:
 *     nowhere). An interrupt's line is high while a source mapped to it
 *     is; a level type (CPU_INT_TYPE 0) is pending while it is high, an
 *     edge type (1) from its rise until CPU_INT_CLEAR clears it -- once
 *     the core has claimed it, or while it is disabled (1.5.2). It is in
 *     CPU_INT_EIP_STATUS, and the core (riscv.c's esp32c3 model) takes it
 *     at mtvec + 4 x ID when mstatus.MIE allows, if it is enabled
 *     (CPU_INT_ENABLE) and its priority (CPU_INT_PRI_n, 1-15) is not below
 *     CPU_INT_THRESH: the highest priority first, then the lowest ID.
 *     INTR_STATUS_REG_0-1 show the sources' levels. The controller's
 *     4-cycle latency is not modelled;
 *   - SYSTIMER (TRM 10): UNIT0 and UNIT1 count at 16 MHz (XTAL_CLK / 2.5)
 *     while their WORK_EN is set (UNIT0's out of reset). UNITn_OP's UPDATE
 *     latches the count into VALUE_HI/LO with VALUE_VALID; UNITn_LOAD
 *     loads LOAD_HI/LO. COMPx_LOAD takes TARGETx_HI/LO (the alarm value)
 *     and TARGETx_CONF's period; PERIOD_MODE and TIMER_UNIT_SEL apply as
 *     written. While TARGETx_WORK_EN is set a comparator alarms: in target
 *     mode once, when its unit reaches the value -- at once if the count
 *     is past it by less than 2^51 - 1 (table 10.4-2); in period mode
 *     every period from the count it started at (COMPx_LOAD, WORK_EN or
 *     PERIOD_MODE set). An alarm sets INT_RAW; INT_ST is INT_RAW & INT_ENA
 *     and the level of source SYSTIMER_TARGETx; INT_CLR, or a 1 written to
 *     INT_RAW, clears it;
 *   - UART0 and UART1 (TRM 26): a byte written to FIFO goes out at once
 *     (to stdout) and sets TX_DONE, so the transmit FIFO is always empty:
 *     STATUS.TXFIFO_CNT 0, TXFIFO_EMPTY raised while 0 is below CONF1's
 *     TXFIFO_EMPTY_THRHD. UART0's receiver is the run's input (--input),
 *     a 128-byte FIFO read from FIFO, its count in STATUS.RXFIFO_CNT,
 *     RXFIFO_FULL raised while it holds more than RXFIFO_FULL_THRHD;
 *     CONF0.RXFIFO_RST empties it. INT_ST is INT_RAW & INT_ENA, the
 *     source's level; INT_CLR (or 1 to INT_RAW) clears a bit, which a
 *     condition still true sets again;
 *   - GPIO and IO_MUX (TRM 5): a pin whose IO_MUX function is its GPIO
 *     (MCU_SEL 1, or 0 where function 0 is GPIOn: 0-3, 8-11, 18, 19),
 *     whose output enable is set (ENABLE, its W1TS and W1TC) and whose
 *     FUNCn_OUT_SEL_CFG selects GPIO_OUT (128) drives OUT's bit (inverted
 *     by INV_SEL; open drain with PINn's PAD_DRIVER, a 1 letting it go).
 *     A pin not driven reads what the board drives, else its FUN_WPU (1)
 *     or FUN_WPD (0) pull, else 0. IN shows the levels of the pins whose
 *     FUN_IE is set. Each pin's level goes to the board as it changes.
 *     The pads' reset values are table 5.12-1's, pin by pin (the SVD's
 *     one value, 0xB00, is not every pin's);
 *   - SYSTEM: the CPU's clock and the board's time: XTAL_CLK (40 MHz) /
 *     (PRE_DIV_CNT + 1), 20 MHz out of reset; the PLL's 80 or 160 MHz
 *     (CPUPERIOD_SEL 0 or 1, table 6.2-2); RC_FAST_CLK (17.5 MHz) /
 *     (PRE_DIV_CNT + 1). CPU_INTR_FROM_CPU_0-3's bit 0 is the level of
 *     source FROM_CPU_INTR0-3, the software's own interrupt;
 *   - RTC_CNTL: OPTIONS0.SW_SYS_RST resets the chip, which ends the run.
 *
 * Not modelled: the ROM (EmbSim puts the image's segments where they are
 * linked and starts at its entry, as the ROM's loader leaves it, but with
 * the registers at their reset values, the clocks too), the PLL's power-up
 * (selecting it gives its frequency at once), the caches and flash (an
 * image runs from SRAM), the watchdogs (TIMG's MWDT, RTC_CNTL's RWDT, the
 * super watchdog: they never reset), the timer groups, SYSTEM's clock
 * gates and peripheral resets, the GPIO matrix's peripheral signals, the
 * pins' interrupts, the USB Serial/JTAG controller, Wi-Fi, Bluetooth and
 * the rest (their registers keep what is written). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "riscv.h"
#include "svd-map.h"

#define NSRC 62
#define XTAL_HZ 40000000u
#define RC_FAST_HZ 17500000u
#define SYSTIMER_HZ 16000000u

static struct rf_reg *reg(struct rf_periph *p, u32 off)
{
    return rf_reg_at(p->sim->svd, p->base + off);
}

static u32 rd(struct rf_periph *p, u32 off)
{
    struct rf_reg *r = reg(p, off);
    return r ? (u32)r->value : 0;
}

static void hw(struct rf_periph *p, u32 off, u32 v)
{
    struct rf_reg *r = reg(p, off);
    if (r && r->value != v)
        rf_hw(r, v);
}

/* a write-only register reads 0 again after the write took effect */
static void spent(struct rf_periph *p, u32 off)
{
    struct rf_reg *r = reg(p, off);
    if (r)
        r->value = 0;
}

/* ---- the interrupt matrix -------------------------------------------------- */

#define M_ENABLE 0x104u
#define M_TYPE 0x108u
#define M_CLEAR 0x10cu
#define M_EIP 0x110u
#define M_PRI(id) (0x114u + 4u * (u32)(id))
#define M_THRESH 0x194u

struct matrix {
    struct sim *s;
    struct rf_periph *p;
    u8 level[NSRC];                 /* each source's level */
    u32 line;                       /* each CPU interrupt's line */
    u32 latched;                    /* the edge types' captured rises */
    u32 claimed;                    /* ... that the core has taken */
    u32 eip;                        /* CPU_INT_EIP_STATUS, as last worked out */
};

static struct matrix *matrix_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "INTERRUPT_CORE0") : 0;
    return p && p->ops == &esp32c3_matrix_ops ? p->ctx : 0;
}

/* each CPU interrupt's line, the OR of the sources mapped to it; a rise
 * of an edge type's is captured */
static void lines(struct matrix *m)
{
    u32 l = 0;
    for (int n = 0; n < NSRC; n++)
        if (m->level[n]) {
            u32 id = rd(m->p, 4u * (u32)n) & 31;
            if (id)
                l |= 1u << id;
        }
    m->latched |= l & ~m->line & rd(m->p, M_TYPE);
    m->line = l;
}

/* the interrupts pending, enabled and of a priority at or above the
 * threshold: CPU_INT_EIP_STATUS */
static u32 eligible(struct matrix *m)
{
    u32 type = rd(m->p, M_TYPE);
    u32 p = ((m->line & ~type) | (m->latched & type)) & rd(m->p, M_ENABLE);
    u32 th = rd(m->p, M_THRESH) & 15, out = 0;
    for (int id = 1; id < 32; id++) {
        u32 pri = rd(m->p, M_PRI(id)) & 15;
        if ((p >> id & 1) && pri && pri >= th)
            out |= 1u << id;
    }
    return out;
}

static void status(struct matrix *m)
{
    u32 st[2] = { 0, 0 };
    lines(m);
    for (int n = 0; n < NSRC; n++)
        if (m->level[n])
            st[n / 32] |= 1u << (n % 32);
    m->eip = eligible(m);
    hw(m->p, M_EIP, m->eip);
    hw(m->p, 0xf8, st[0]);
    hw(m->p, 0xfc, st[1]);
}

/* the core's question, after every instruction: the interrupt to take
 * now, or 0. What it depends on changes only where status() is called */
static int eic_pending(void *ctx)
{
    struct matrix *m = ctx;
    u32 e = m->eip, bp = 0;
    int best = 0;
    if (!e)
        return 0;
    for (int id = 1; id < 32; id++)
        if (e >> id & 1) {
            u32 pri = rd(m->p, M_PRI(id)) & 15;
            if (pri > bp) {
                bp = pri;
                best = id;
            }
        }
    return best;
}

static void eic_claim(void *ctx, int id)
{
    struct matrix *m = ctx;
    m->claimed |= m->latched & 1u << id;
}

void esp32c3_source(struct sim *s, int src, int level)
{
    struct matrix *m = matrix_of(s);
    if (!m || src < 0 || src >= NSRC || m->level[src] == (level != 0))
        return;
    m->level[src] = level != 0;
    status(m);
}

static u32 matrix_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct matrix *)ctx)->p, off, n);
}

static void matrix_write(void *ctx, u32 off, int n, u32 v)
{
    struct matrix *m = ctx;
    rf_write(m->p, off, n, v);
    if (off == M_CLEAR) {
        /* a captured edge is cleared once claimed, or while disabled */
        u32 c = v & (m->claimed | ~rd(m->p, M_ENABLE));
        m->latched &= ~c;
        m->claimed &= ~c;
    }
    status(m);
}

static void matrix_reset(void *ctx)
{
    struct matrix *m = ctx;
    memset(m->level, 0, sizeof m->level);
    m->line = m->latched = m->claimed = m->eip = 0;
}

const struct dev_ops esp32c3_matrix_ops = {
    "esp32c3-matrix", matrix_read, matrix_write, matrix_reset, 0, 0,
};

void *esp32c3_matrix_create(struct sim *s, struct rf_periph *p)
{
    if (!riscv_is(s->cpu))
        die("INTERRUPT_CORE0: the ESP32-C3's interrupt matrix needs its "
            "core (the riscv core's esp32c3 model)");
    struct matrix *m = calloc(1, sizeof *m);
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    struct rv_eic e = { m, eic_pending, eic_claim };
    riscv_attach_eic(s->cpu, &e);
    return m;
}

/* ---- SYSTEM: the CPU's clock, the board's time, the software interrupts --- */

struct sysreg {
    struct sim *s;
    struct rf_periph *p;
    u64 dev, at;                    /* device cycles, and at the last change */
    double ns;                      /* the board's time then */
    u32 hz;
    int running;
    int src[4];                     /* FROM_CPU_INTR0-3 */
};

static struct sysreg *sys_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "SYSTEM") : 0;
    return p && p->ops == &esp32c3_system_ops ? p->ctx : 0;
}

/* table 6.2-2; SOC_CLK_SEL 3 (reserved) is taken as XTAL_CLK, and
 * CPUPERIOD_SEL 2 and 3 (reserved) as 80 MHz */
static u32 cpu_hz(struct sysreg *y)
{
    u32 sc = rd(y->p, 0x58), sel = sc >> 10 & 3, div = (sc & 0x3ff) + 1;
    if (sel == 1)
        return (rd(y->p, 0x8) & 3) == 1 ? 160000000u : 80000000u;
    if (sel == 2)
        return RC_FAST_HZ / div;
    return XTAL_HZ / div;
}

u32 esp32c3_cpu_hz(struct sim *s)
{
    struct sysreg *y = sys_of(s);
    return y ? y->hz : XTAL_HZ / 2;
}

static double sys_now_ns(struct sim *s)
{
    struct sysreg *y = sys_of(s);
    if (!y)
        return 0;
    return y->ns + (double)(y->dev - y->at) * 1e9 / y->hz;
}

static void sys_tick(void *ctx, u32 cycles)
{
    ((struct sysreg *)ctx)->dev += cycles;
}

static u32 sys_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct sysreg *)ctx)->p, off, n);
}

static void sys_write(void *ctx, u32 off, int n, u32 v)
{
    struct sysreg *y = ctx;
    rf_write(y->p, off, n, v);
    u32 h = cpu_hz(y);
    if (h != y->hz) {
        y->ns = sys_now_ns(y->s);
        y->at = y->dev;
        y->hz = h;
    }
    if (off >= 0x28 && off <= 0x34)
        esp32c3_source(y->s, y->src[(off - 0x28) / 4], (int)(rd(y->p, off) & 1));
}

static void sys_reset(void *ctx)
{
    struct sysreg *y = ctx;
    y->dev = y->at = 0;
    y->ns = 0;
    y->hz = cpu_hz(y);
    y->running = 0;
    rf_clock(y->p, &y->running, 1);
}

const struct dev_ops esp32c3_system_ops = {
    "esp32c3-system", sys_read, sys_write, sys_reset, sys_tick, 0,
};

void *esp32c3_system_create(struct sim *s, struct rf_periph *p)
{
    struct sysreg *y = calloc(1, sizeof *y);
    if (!y)
        die("out of memory");
    y->s = s;
    y->p = p;
    y->hz = cpu_hz(y);
    /* FROM_CPU_INTR0-3 are sources 50-53 (table 8.3-1; the SVD lists
     * them under INTERRUPT_CORE0) */
    for (int i = 0; i < 4; i++)
        y->src[i] = 50 + i;
    s->now_ns = sys_now_ns;
    rf_clock(p, &y->running, 1);
    return y;
}

/* ---- SYSTIMER ---------------------------------------------------------------- */

#define M52 ((1ull << 52) - 1)
#define S_CONF 0x0u
#define S_INT_ENA 0x64u
#define S_INT_RAW 0x68u
#define S_INT_CLR 0x6cu
#define S_INT_ST 0x70u

struct systimer {
    struct sim *s;
    struct rf_periph *p;
    u64 unit[2];
    u64 acc;                        /* toward the next count: cycles x 16M */
    u32 hz;                         /* the CPU clock acc is counted in */
    u64 target[3], period[3], next[3];
    int fired[3];                   /* target mode: alarmed since armed */
    int src[3];
    u32 conf, tconf[3];             /* as last written: their rises */
    int running;
};

static int work(struct systimer *t, int x)
{
    return (int)(rd(t->p, S_CONF) >> (24 - x) & 1);
}

static int pmode(struct systimer *t, int x)
{
    return (int)(rd(t->p, 0x34 + 4u * (u32)x) >> 30 & 1);
}

static int usel(struct systimer *t, int x)
{
    return (int)(rd(t->p, 0x34 + 4u * (u32)x) >> 31);
}

static int counting(struct systimer *t, int u)
{
    return (int)(rd(t->p, S_CONF) >> (30 - u) & 1);
}

static void st_irq(struct systimer *t)
{
    u32 st = rd(t->p, S_INT_RAW) & rd(t->p, S_INT_ENA) & 7;
    hw(t->p, S_INT_ST, st);
    for (int x = 0; x < 3; x++)
        esp32c3_source(t->s, t->src[x], (int)(st >> x & 1));
}

/* a count at or past t by less than 2^51 - 1 (table 10.4-2) */
static int reached(u64 c, u64 t)
{
    return ((c - t) & M52) < (1ull << 51) - 1;
}

/* the comparators, against the units' counts now */
static void st_compare(struct systimer *t)
{
    for (int x = 0; x < 3; x++) {
        if (!work(t, x))
            continue;
        u64 c = t->unit[usel(t, x)];
        int alarm = 0;
        if (pmode(t, x)) {
            if (t->period[x] && reached(c, t->next[x])) {
                t->next[x] = (t->next[x] + t->period[x]) & M52;
                alarm = 1;
            }
        } else if (!t->fired[x] && reached(c, t->target[x])) {
            t->fired[x] = 1;
            alarm = 1;
        }
        if (alarm)
            hw(t->p, S_INT_RAW, rd(t->p, S_INT_RAW) | 1u << x);
    }
    st_irq(t);
}

/* comparator x starts comparing: once more in target mode, the periods
 * from now in period mode */
static void st_arm(struct systimer *t, int x)
{
    t->fired[x] = 0;
    t->next[x] = (t->unit[usel(t, x)] + t->period[x]) & M52;
}

static void st_clock(struct systimer *t)
{
    rf_clock(t->p, &t->running, counting(t, 0) || counting(t, 1));
}

static void st_tick(void *ctx, u32 cycles)
{
    struct systimer *t = ctx;
    u32 hz = esp32c3_cpu_hz(t->s);
    if (hz != t->hz) {              /* the clock changed: the fraction kept */
        t->acc = t->acc * hz / t->hz;
        t->hz = hz;
    }
    t->acc += (u64)cycles * SYSTIMER_HZ;
    u64 n = t->acc / hz;
    t->acc %= hz;
    if (!n)
        return;
    for (int u = 0; u < 2; u++)
        if (counting(t, u))
            t->unit[u] = (t->unit[u] + n) & M52;
    st_compare(t);
}

/* the cycles to the next alarm that would interrupt */
static int st_next(void *ctx, u32 *cycles)
{
    struct systimer *t = ctx;
    u32 ena = rd(t->p, S_INT_ENA);
    u64 best = 0;
    int any = 0;
    for (int x = 0; x < 3; x++) {
        if (!work(t, x) || !(ena >> x & 1) || !counting(t, usel(t, x)) ||
            (pmode(t, x) ? !t->period[x] : t->fired[x]))
            continue;
        u64 c = t->unit[usel(t, x)];
        u64 d = ((pmode(t, x) ? t->next[x] : t->target[x]) - c) & M52;
        if (!d)
            d = 1;
        if (!any || d < best)
            best = d;
        any = 1;
    }
    if (!any)
        return 0;
    if (best > 0xffffffffull)
        best = 0xffffffffull;
    u64 need = best * esp32c3_cpu_hz(t->s);
    u64 cy = need > t->acc ? (need - t->acc + SYSTIMER_HZ - 1) / SYSTIMER_HZ : 1;
    *cycles = cy > 0xffffffffu ? 0xffffffffu : (u32)cy;
    return 1;
}

static u32 st_read(void *ctx, u32 off, int n)
{
    return rf_read(((struct systimer *)ctx)->p, off, n);
}

static void st_write(void *ctx, u32 off, int n, u32 v)
{
    struct systimer *t = ctx;
    u32 raw = rd(t->p, S_INT_RAW);
    rf_write(t->p, off, n, v);
    if ((off == 0x4 || off == 0x8) && (v >> 30 & 1)) {     /* UNITn_OP.UPDATE */
        int u = off == 0x8;
        hw(t->p, 0x40 + 8u * (u32)u, (u32)(t->unit[u] >> 32));
        hw(t->p, 0x44 + 8u * (u32)u, (u32)t->unit[u]);
        hw(t->p, off, 1u << 29);                           /* VALUE_VALID */
    }
    if ((off == 0x5c || off == 0x60) && (v & 1)) {         /* UNITn_LOAD */
        int u = off == 0x60;
        t->unit[u] = ((u64)(rd(t->p, 0xc + 8u * (u32)u) & 0xfffff) << 32 |
                      rd(t->p, 0x10 + 8u * (u32)u)) & M52;
        spent(t->p, off);
    }
    if (off >= 0x50 && off <= 0x58 && (v & 1)) {           /* COMPx_LOAD */
        int x = (int)(off - 0x50) / 4;
        t->target[x] = ((u64)(rd(t->p, 0x1c + 8u * (u32)x) & 0xfffff) << 32 |
                        rd(t->p, 0x20 + 8u * (u32)x)) & M52;
        t->period[x] = rd(t->p, 0x34 + 4u * (u32)x) & 0x3ffffff;
        st_arm(t, x);
        spent(t->p, off);
    }
    if (off == S_CONF) {
        for (int x = 0; x < 3; x++)
            if (work(t, x) && !(t->conf >> (24 - x) & 1))
                st_arm(t, x);
        t->conf = rd(t->p, S_CONF);
    }
    if (off >= 0x34 && off <= 0x3c) {                      /* TARGETx_CONF */
        int x = (int)(off - 0x34) / 4;
        if (pmode(t, x) && !(t->tconf[x] >> 30 & 1))
            st_arm(t, x);
        t->tconf[x] = rd(t->p, off);
    }
    if (off == S_INT_CLR) {
        hw(t->p, S_INT_RAW, rd(t->p, S_INT_RAW) & ~v);
        spent(t->p, off);
    }
    if (off == S_INT_RAW)                                  /* R/WTC/SS */
        hw(t->p, S_INT_RAW, raw & ~v);
    st_compare(t);
    st_clock(t);
}

static void st_reset(void *ctx)
{
    struct systimer *t = ctx;
    memset(t->unit, 0, sizeof t->unit);
    memset(t->target, 0, sizeof t->target);
    memset(t->period, 0, sizeof t->period);
    memset(t->next, 0, sizeof t->next);
    memset(t->fired, 0, sizeof t->fired);
    memset(t->tconf, 0, sizeof t->tconf);
    t->conf = rd(t->p, S_CONF);
    t->acc = 0;
    t->hz = esp32c3_cpu_hz(t->s);
    t->running = 0;
    st_clock(t);
}

const struct dev_ops esp32c3_systimer_ops = {
    "esp32c3-systimer", st_read, st_write, st_reset, st_tick, st_next,
};

void *esp32c3_systimer_create(struct sim *s, struct rf_periph *p)
{
    struct systimer *t = calloc(1, sizeof *t);
    if (!t)
        die("out of memory");
    t->s = s;
    t->p = p;
    for (int x = 0; x < 3; x++) {
        char nm[24];
        snprintf(nm, sizeof nm, "SYSTIMER_TARGET%d", x);
        t->src[x] = rf_irq(p, nm);
    }
    t->conf = rd(p, S_CONF);
    t->hz = XTAL_HZ / 2;
    st_clock(t);
    return t;
}

/* ---- UART0, UART1 -------------------------------------------------------------- */

#define RXFIFO 128
#define U_FIFO 0x0u
#define U_INT_RAW 0x4u
#define U_INT_ST 0x8u
#define U_INT_ENA 0xcu
#define U_INT_CLR 0x10u
#define U_STATUS 0x1cu
#define U_CONF0 0x20u
#define U_CONF1 0x24u

struct uart {
    struct sim *s;
    struct rf_periph *p;
    int src;
    u8 rx[RXFIFO];
    int nrx, rxh;
};

/* the conditions that raise their INT_RAW bits while they hold */
static void uart_update(struct uart *u)
{
    u32 conf1 = rd(u->p, U_CONF1), full = conf1 & 0x1ff, empty = conf1 >> 9 & 0x1ff;
    u32 raw = rd(u->p, U_INT_RAW);
    if (empty > 0)                  /* TXFIFO_CNT, 0, is below it */
        raw |= 1u << 1;
    if ((u32)u->nrx > full)
        raw |= 1u << 0;
    hw(u->p, U_INT_RAW, raw);
    u32 st = raw & rd(u->p, U_INT_ENA);
    hw(u->p, U_INT_ST, st);
    hw(u->p, U_STATUS, (rd(u->p, U_STATUS) & ~0x03ff03ffu) | (u32)u->nrx);
    esp32c3_source(u->s, u->src, st != 0);
}

static u32 uart_read(void *ctx, u32 off, int n)
{
    struct uart *u = ctx;
    if (off == U_FIFO) {                            /* the receiver's */
        u32 v = u->nrx ? u->rx[u->rxh] : 0;
        if (u->nrx && !u->s->bus.debug) {
            u->rxh = (u->rxh + 1) % RXFIFO;
            u->nrx--;
            uart_update(u);
        }
        return v;
    }
    return rf_read(u->p, off, n);
}

static void uart_write(void *ctx, u32 off, int n, u32 v)
{
    struct uart *u = ctx;
    u32 raw = rd(u->p, U_INT_RAW);
    rf_write(u->p, off, n, v);
    if (off == U_FIFO) {
        sim_out(u->s, (int)(v & 0xff));
        hw(u->p, U_INT_RAW, rd(u->p, U_INT_RAW) | 1u << 14);   /* TX_DONE */
        spent(u->p, off);
    }
    if (off == U_INT_CLR) {
        hw(u->p, U_INT_RAW, rd(u->p, U_INT_RAW) & ~v);
        spent(u->p, off);
    }
    if (off == U_INT_RAW)                           /* R/WTC/SS */
        hw(u->p, U_INT_RAW, raw & ~v);
    if (off == U_CONF0 && (v >> 17 & 1))            /* RXFIFO_RST */
        u->nrx = u->rxh = 0;
    uart_update(u);
}

static void uart_reset(void *ctx)
{
    struct uart *u = ctx;
    u->nrx = u->rxh = 0;
    uart_update(u);
}

static int uart_room(void *ctx)
{
    return ((struct uart *)ctx)->nrx < RXFIFO;
}

static void uart_put(void *ctx, int c)
{
    struct uart *u = ctx;
    u->rx[(u->rxh + u->nrx++) % RXFIFO] = (u8)c;
    uart_update(u);
}

const struct dev_ops esp32c3_uart_ops = {
    "esp32c3-uart", uart_read, uart_write, uart_reset, 0, 0,
};

void *esp32c3_uart_create(struct sim *s, struct rf_periph *p)
{
    if (strcmp(p->name, "UART0") && strcmp(p->name, "UART1"))
        return 0;
    struct uart *u = calloc(1, sizeof *u);
    if (!u)
        die("out of memory");
    u->s = s;
    u->p = p;
    u->src = rf_irq(p, p->name);
    if (!strcmp(p->name, "UART0"))
        sim_rx_port(s, uart_room, uart_put, u);
    return u;
}

/* ---- GPIO and IO_MUX ----------------------------------------------------------- */

#define NPIN 22
#define G_OUT 0x04u
#define G_ENABLE 0x20u
#define G_IN 0x3cu
#define G_PIN(i) (0x74u + 4u * (u32)(i))
#define G_OUT_SEL(i) (0x554u + 4u * (u32)(i))

/* each pad's reset value, table 5.12-1: FUN_DRV 2 (3 on GPIO18-19);
 * reset 0 no input, 1 FUN_IE, 3 FUN_IE and FUN_WPU (MTCK's 1*, with
 * EFUSE_DIS_PAD_JTAG 0, is 3), 4 (U0TXD) FUN_WPU */
static const u32 pad_reset[NPIN] = {
    0x800, 0x800, 0xa00, 0xa00, 0xa00, 0xa00, 0xb00, 0xa00,     /* 0-7 */
    0xa00, 0xb00, 0xa00, 0x800, 0xb00, 0xb00, 0xb00, 0xb00,     /* 8-15 */
    0xb00, 0xb00, 0xc00, 0xc00, 0xb00, 0x900,                   /* 16-21 */
};

struct gpio {
    struct sim *s;
    struct rf_periph *p, *mux;
    int out[NPIN];
    int known;
};

/* whether the pin's IO_MUX function is its GPIO: MCU_SEL 1, or 0 on the
 * pins whose function 0 is GPIOn */
static int gpio_function(u32 mux, int i)
{
    u32 sel = mux >> 12 & 7;
    return sel == 1 || (sel == 0 && (i <= 3 || (i >= 8 && i <= 11) || i == 18 || i == 19));
}

static void gpio_update(struct gpio *g)
{
    struct sim *s = g->s;
    u32 out = rd(g->p, G_OUT), en = rd(g->p, G_ENABLE), in = 0;
    for (int i = 0; i < NPIN; i++) {
        u32 mux = g->mux ? rd(g->mux, 0x4 + 4u * (u32)i) : pad_reset[i];
        u32 sel = rd(g->p, G_OUT_SEL(i));
        int o = -1, lvl = 0, ext = 0;
        if (gpio_function(mux, i) && (en >> i & 1) && (sel & 0xff) == 128) {
            o = (int)((out >> i ^ sel >> 8) & 1);              /* INV_SEL */
            if (o && (rd(g->p, G_PIN(i)) >> 2 & 1))             /* PAD_DRIVER */
                o = -1;
        }
        if (o >= 0)
            lvl = o;
        else if (s->io.pin_in && s->io.pin_in(s->io.ctx, 0, i, &ext))
            lvl = ext != 0;
        else if (mux >> 8 & 1)                                  /* FUN_WPU */
            lvl = 1;
        if (mux >> 9 & 1)                                       /* FUN_IE */
            in |= (u32)lvl << i;
        if (s->io.pin_out && (!g->known || g->out[i] != o))
            s->io.pin_out(s->io.ctx, 0, i, o);
        g->out[i] = o;
    }
    g->known = 1;
    hw(g->p, G_IN, in);
}

static u32 gpio_read(void *ctx, u32 off, int n)
{
    struct gpio *g = ctx;
    if (off == G_IN)
        gpio_update(g);
    return rf_read(g->p, off, n);
}

static void gpio_write(void *ctx, u32 off, int n, u32 v)
{
    struct gpio *g = ctx;
    u32 out = rd(g->p, G_OUT), en = rd(g->p, G_ENABLE);
    rf_write(g->p, off, n, v);
    switch (off) {
    case 0x08: hw(g->p, G_OUT, (out | v) & 0x3ffffff); spent(g->p, off); break;
    case 0x0c: hw(g->p, G_OUT, out & ~v); spent(g->p, off); break;
    case 0x24: hw(g->p, G_ENABLE, (en | v) & 0x3ffffff); spent(g->p, off); break;
    case 0x28: hw(g->p, G_ENABLE, en & ~v); spent(g->p, off); break;
    }
    gpio_update(g);
}

static void gpio_reset(void *ctx)
{
    struct gpio *g = ctx;
    g->known = 0;
    gpio_update(g);
}

const struct dev_ops esp32c3_gpio_ops = {
    "esp32c3-gpio", gpio_read, gpio_write, gpio_reset, 0, 0,
};

void *esp32c3_gpio_create(struct sim *s, struct rf_periph *p)
{
    struct gpio *g = calloc(1, sizeof *g);
    if (!g)
        die("out of memory");
    g->s = s;
    g->p = p;
    g->mux = rf_periph(s->svd, "IO_MUX");
    return g;
}

/* the board drove a pin or let it go, or IO_MUX changed a pad */
void esp32c3_pins_changed(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "GPIO") : 0;
    if (p && p->ops == &esp32c3_gpio_ops)
        gpio_update(p->ctx);
}

static u32 mux_read(void *ctx, u32 off, int n)
{
    return rf_read(ctx, off, n);
}

static void mux_write(void *ctx, u32 off, int n, u32 v)
{
    struct rf_periph *p = ctx;
    rf_write(p, off, n, v);
    esp32c3_pins_changed(p->sim);
}

const struct dev_ops esp32c3_iomux_ops = {
    "esp32c3-iomux", mux_read, mux_write, 0, 0, 0,
};

void *esp32c3_iomux_create(struct sim *s, struct rf_periph *p)
{
    (void)s;
    for (int i = 0; i < NPIN; i++) {
        struct rf_reg *r = reg(p, 0x4 + 4u * (u32)i);
        if (r) {
            rf_set_reset(r, pad_reset[i]);
            r->value = pad_reset[i];
        }
    }
    return p;
}

/* ---- RTC_CNTL: the software reset ------------------------------------------------ */

static u32 rtc_read(void *ctx, u32 off, int n)
{
    return rf_read(ctx, off, n);
}

static void rtc_write(void *ctx, u32 off, int n, u32 v)
{
    struct rf_periph *p = ctx;
    struct sim *s = p->sim;
    rf_write(p, off, n, v);
    if (off == 0 && (v >> 31 & 1) && s->state == RUN) {
        s->exit_status = 0;
        sim_end(s, END_EXIT, "RTC_CNTL's SW_SYS_RST reset the chip");
    }
}

const struct dev_ops esp32c3_rtc_ops = {
    "esp32c3-rtc", rtc_read, rtc_write, 0, 0, 0,
};

void *esp32c3_rtc_create(struct sim *s, struct rf_periph *p)
{
    (void)s;
    return p;
}
