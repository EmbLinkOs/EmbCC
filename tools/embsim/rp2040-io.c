/* rp2040-io.c -- the RP2040's user GPIOs (its datasheet, 2.19): IO_BANK0's
 * function select, overrides and interrupts, PADS_BANK0's pads, and the
 * pins they make; and IO_QSPI's chip select, which the Pico's BOOTSEL
 * button pulls low.
 *
 * Each of GPIO0-29:
 *   - its function (CTRL.FUNCSEL) gives the output and its enable: SIO
 *     (5), GPIO_OUT and GPIO_OE (rp2040-sio.c); UART (2), a TX or RTS pin
 *     driven high (the line idle) and RX and CTS pins inputs, by the
 *     function table's pattern (GPIO0-3 UART0, 4-11 UART1, 12-19 UART0,
 *     20-27 UART1, 28-29 UART0); any other function does not drive the
 *     pin (its peripheral is not modelled), nor does NULL (0x1f, out of
 *     reset);
 *   - OUTOVER and OEOVER then pass, invert, or force them low or high
 *     (OEOVER: disable or enable), and the pad's OD disables the output;
 *   - the pin is the output where enabled; else what the board drives on
 *     it; else its pad's pull (PUE high, PDE low, both: it keeps its last
 *     level, the bus keeper); else 0 (floating, EmbSim's choice);
 *   - its input, when the pad's IE is set, is the pin (INOVER passes,
 *     inverts or forces it): what GPIO_IN reads, and the interrupt's;
 *   - the interrupts (INTR0-3): LEVEL_LOW and LEVEL_HIGH follow the input
 *     after IRQOVER, EDGE_LOW and EDGE_HIGH latch its falls and rises until
 *     written with 1; PROC0_INTS is (INTR | PROC0_INTF) & PROC0_INTE, and
 *     any bit of it raises IO_IRQ_BANK0, when it sets and when a write
 *     leaves it set;
 *   - GPIOn_STATUS shows each stage: OUTFROMPERI, OUTTOPAD, OEFROMPERI,
 *     OETOPAD, INFROMPAD, INTOPERI, IRQFROMPAD, IRQTOPROC.
 * The board's device (mcu-boards.c) is told each pin's level as it
 * changes, and says what it drives (struct board_io's pin_out, pin_in:
 * port 0 is the user bank, port 1 the QSPI bank).
 *
 * The QSPI bank: the SIO's GPIO_HI_IN reads its six pins (SCLK, SS,
 * SD0-3). The SSI drives them: SS high but while its own transfer runs,
 * so it reads 1 -- unless GPIO_QSPI_SS_CTRL's OEOVER lets the pin go (2),
 * when it reads what the board pulls it to: the Pico's BOOTSEL button
 * pressed pulls it low.
 *
 * Not modelled: PROC1's interrupts (core 1), DORMANT_WAKE, the pads'
 * drive strength, slew and Schmitt trigger, the QSPI bank's interrupts
 * and the other functions' peripherals (SPI, I2C, PWM, PIO, the clock
 * pins, USB). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"
#include "svd-map.h"

#define NPIN 30

enum { F_SPI = 1, F_UART, F_I2C, F_PWM, F_SIO, F_PIO0, F_PIO1, F_CLOCK, F_USB, F_NULL = 0x1f };

struct io {
    struct sim *s;
    struct rf_periph *p, *pads, *qspi;
    struct rf_reg *status[NPIN], *ctrl[NPIN], *pad[NPIN];
    struct rf_reg *intr[4], *inte[4], *intf[4], *ints[4];
    struct rf_reg *ss_ctrl;
    u64 m_funcsel, m_outover, m_oeover, m_inover, m_irqover;
    u64 m_od, m_ie, m_pue, m_pde;
    u64 st_outperi, st_outpad, st_oeperi, st_oepad, st_inpad, st_inperi, st_irqpad, st_irqproc;
    int irq;
    u32 sio_out, sio_oe, hi_out, hi_oe;
    u32 level;                      /* each pin's level */
    u32 irq_level;                  /* each pin's interrupt input, after IRQOVER */
    u32 known;                      /* the levels have been worked out */
    int busy;
};

static struct io *io_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "IO_BANK0") : 0;
    return p && p->ops == &rp2040_io_ops ? p->ctx : 0;
}

static u32 fld(const struct rf_reg *r, u64 mask)
{
    int sh = 0;
    if (!r || !mask)
        return 0;
    while (!(mask >> sh & 1))
        sh++;
    return (u32)((r->value & mask) >> sh);
}

static int over(int v, int mode)
{
    return mode == 0 ? v : mode == 1 ? !v : mode == 2 ? 0 : 1;
}

/* the UART signal on pin n with FUNCSEL 2: 0 TX, 1 RX, 2 CTS, 3 RTS */
static int uart_signal(int n)
{
    return n & 3;
}

/* what the function gives the pin: its output and output enable */
static void peri(struct io *x, int n, int *out, int *oe)
{
    *out = *oe = 0;
    switch (fld(x->ctrl[n], x->m_funcsel)) {
    case F_SIO:
        *out = (int)(x->sio_out >> n & 1);
        *oe = (int)(x->sio_oe >> n & 1);
        break;
    case F_UART:
        if (uart_signal(n) == 0 || uart_signal(n) == 3)
            *out = *oe = 1;         /* TX idle, RTS not asserted */
        break;
    }
}

/* every pin's level, the stages GPIOn_STATUS shows, the interrupts */
static void settle(struct io *x)
{
    struct sim *s = x->s;
    u32 lvl = 0, irq = 0;
    if (x->busy)
        return;
    x->busy = 1;
    for (int n = 0; n < NPIN; n++) {
        int out, oe, ext;
        peri(x, n, &out, &oe);
        int pout = over(out, (int)fld(x->ctrl[n], x->m_outover));
        int poe = over(oe, (int)fld(x->ctrl[n], x->m_oeover));
        int drive = poe && !fld(x->pad[n], x->m_od);
        int pin;
        if (drive)
            pin = pout;
        else if (s->io.pin_in && s->io.pin_in(s->io.ctx, 0, n, &ext))
            pin = ext != 0;
        else if (fld(x->pad[n], x->m_pue) && fld(x->pad[n], x->m_pde))
            pin = (int)(x->level >> n & 1);
        else
            pin = fld(x->pad[n], x->m_pue) ? 1 : 0;
        int in = fld(x->pad[n], x->m_ie) ? pin : 0;
        int toperi = over(in, (int)fld(x->ctrl[n], x->m_inover));
        int toproc = over(in, (int)fld(x->ctrl[n], x->m_irqover));
        lvl |= (u32)pin << n;
        irq |= (u32)toproc << n;
        u64 st = (out ? x->st_outperi : 0) | (pout ? x->st_outpad : 0) |
                 (oe ? x->st_oeperi : 0) | (poe ? x->st_oepad : 0) |
                 (in ? x->st_inpad | x->st_irqpad : 0) | (toperi ? x->st_inperi : 0) |
                 (toproc ? x->st_irqproc : 0);
        x->status[n]->value = st;
    }
    u32 rise = x->known ? irq & ~x->irq_level : 0;
    u32 fall = x->known ? ~irq & x->irq_level : 0;
    u32 changed = x->known ? lvl ^ x->level : ~0u;
    x->level = lvl;
    x->irq_level = irq;
    x->known = 1;
    for (int r = 0; r < 4; r++) {
        u64 v = x->intr[r]->value;
        for (int k = 0; k < 8 && 8 * r + k < NPIN; k++) {
            int n = 8 * r + k, b = 4 * k;
            v &= ~(3ull << b);
            v |= (u64)(irq >> n & 1 ? 2 : 1) << b;  /* LEVEL_HIGH, LEVEL_LOW */
            if (fall >> n & 1)
                v |= 4ull << b;
            if (rise >> n & 1)
                v |= 8ull << b;
        }
        if (v != x->intr[r]->value)
            rf_hw(x->intr[r], v);
    }
    if (s->io.pin_out)
        for (int n = 0; n < NPIN; n++)
            if (changed >> n & 1)
                s->io.pin_out(s->io.ctx, 0, n, (int)(lvl >> n & 1));
    x->busy = 0;
}

static void irqs(struct io *x, int again)
{
    int any = 0, was = 0;
    for (int r = 0; r < 4; r++) {
        u64 v = (x->intr[r]->value | x->intf[r]->value) & x->inte[r]->value;
        was |= x->ints[r]->value != 0;
        if (v != x->ints[r]->value)
            rf_hw(x->ints[r], v);
        any |= v != 0;
    }
    if (any && (!was || again))
        rf_interrupt(x->p, x->irq);
}

void rp2040_sio_pins(struct sim *s, u32 out, u32 oe, u32 hi_out, u32 hi_oe)
{
    struct io *x = io_of(s);
    if (!x)
        return;
    x->sio_out = out;
    x->sio_oe = oe;
    x->hi_out = hi_out;
    x->hi_oe = hi_oe;
    settle(x);
    irqs(x, 0);
}

u32 rp2040_gpio_in(struct sim *s)
{
    struct io *x = io_of(s);
    u32 v = 0;
    if (!x)
        return 0;
    settle(x);
    for (int n = 0; n < NPIN; n++)
        if (x->status[n]->value & x->st_inperi)
            v |= 1u << n;
    return v;
}

u32 rp2040_gpio_hi_in(struct sim *s)
{
    struct io *x = io_of(s);
    /* SCLK low, SS high, the data lines pulled up by the flash, unless
     * SS is let go: then the board's (BOOTSEL) */
    u32 v = 0x3c | 2;
    int lvl;
    if (x && x->ss_ctrl && fld(x->ss_ctrl, x->m_oeover) == 2 && s->io.pin_in &&
        s->io.pin_in(s->io.ctx, 1, 1, &lvl) && !lvl)
        v &= ~2u;
    return v;
}

static u32 io_read(void *ctx, u32 off, int n)
{
    struct io *x = ctx;
    settle(x);
    return rf_read(x->p, off, n);
}

static void io_write(void *ctx, u32 off, int n, u32 v)
{
    struct io *x = ctx;
    rf_write(x->p, off, n, v);
    settle(x);
    irqs(x, 1);
}

static void io_reset(void *ctx)
{
    struct io *x = ctx;
    x->known = 0;
    settle(x);
    irqs(x, 0);
}

const struct dev_ops rp2040_io_ops = {
    "rp2040-io", io_read, io_write, io_reset, 0, 0,
};

/* the pads, and the QSPI bank's controls: written, the pins follow */
static u32 pads_read(void *ctx, u32 off, int n)
{
    return rf_read(ctx, off, n);
}

static void pads_write(void *ctx, u32 off, int n, u32 v)
{
    struct rf_periph *p = ctx;
    struct io *x = io_of(p->sim);
    rf_write(p, off, n, v);
    if (x) {
        settle(x);
        irqs(x, 0);
    }
    if (!strcmp(p->name, "IO_QSPI"))
        rp2040_ssi_select(p->sim);  /* its chip select may be let go */
}

static void pads_reset(void *ctx)
{
    struct rf_periph *p = ctx;
    struct io *x = io_of(p->sim);
    if (x) {
        settle(x);
        irqs(x, 0);
    }
}

const struct dev_ops rp2040_pads_ops = {
    "rp2040-pads", pads_read, pads_write, pads_reset, 0, 0,
};

void *rp2040_pads_create(struct sim *s, struct rf_periph *p)
{
    (void)s;
    return p;
}

void *rp2040_io_create(struct sim *s, struct rf_periph *p)
{
    struct io *x = calloc(1, sizeof *x);
    char nm[32];
    if (!x)
        die("out of memory");
    x->s = s;
    x->p = p;
    x->pads = rf_periph(s->svd, "PADS_BANK0");
    x->qspi = rf_periph(s->svd, "IO_QSPI");
    if (!x->pads)
        die("RP2040: no PADS_BANK0 in %s", rf_device_name(s->svd));
    for (int n = 0; n < NPIN; n++) {
        snprintf(nm, sizeof nm, "GPIO%d_STATUS", n);
        x->status[n] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "GPIO%d_CTRL", n);
        x->ctrl[n] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "GPIO%d", n);
        x->pad[n] = rf_reg(x->pads, nm);
        if (!x->status[n] || !x->ctrl[n] || !x->pad[n])
            die("RP2040: GPIO%d's registers are not in %s", n, rf_device_name(s->svd));
    }
    for (int r = 0; r < 4; r++) {
        snprintf(nm, sizeof nm, "INTR%d", r);
        x->intr[r] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "PROC0_INTE%d", r);
        x->inte[r] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "PROC0_INTF%d", r);
        x->intf[r] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "PROC0_INTS%d", r);
        x->ints[r] = rf_reg(p, nm);
        if (!x->intr[r] || !x->inte[r] || !x->intf[r] || !x->ints[r])
            die("RP2040: IO_BANK0's interrupt registers are not in %s",
                rf_device_name(s->svd));
    }
    x->ss_ctrl = x->qspi ? rf_reg(x->qspi, "GPIO_QSPI_SS_CTRL") : 0;
    x->m_funcsel = rf_mask(x->ctrl[0], "FUNCSEL");
    x->m_outover = rf_mask(x->ctrl[0], "OUTOVER");
    x->m_oeover = rf_mask(x->ctrl[0], "OEOVER");
    x->m_inover = rf_mask(x->ctrl[0], "INOVER");
    x->m_irqover = rf_mask(x->ctrl[0], "IRQOVER");
    x->m_od = rf_mask(x->pad[0], "OD");
    x->m_ie = rf_mask(x->pad[0], "IE");
    x->m_pue = rf_mask(x->pad[0], "PUE");
    x->m_pde = rf_mask(x->pad[0], "PDE");
    x->st_outperi = rf_mask(x->status[0], "OUTFROMPERI");
    x->st_outpad = rf_mask(x->status[0], "OUTTOPAD");
    x->st_oeperi = rf_mask(x->status[0], "OEFROMPERI");
    x->st_oepad = rf_mask(x->status[0], "OETOPAD");
    x->st_inpad = rf_mask(x->status[0], "INFROMPAD");
    x->st_inperi = rf_mask(x->status[0], "INTOPERI");
    x->st_irqpad = rf_mask(x->status[0], "IRQFROMPAD");
    x->st_irqproc = rf_mask(x->status[0], "IRQTOPROC");
    x->irq = rf_irq(p, "IO_IRQ_BANK0");
    return x;
}

/* the board drove a pin, or let it go (--set): the levels again */
void rp2040_pins_changed(struct sim *s)
{
    struct io *x = io_of(s);
    if (x) {
        settle(x);
        irqs(x, 0);
    }
}
