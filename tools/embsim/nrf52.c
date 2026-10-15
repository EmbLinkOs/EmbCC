/* nrf52.c -- the nRF52840's peripherals EmbSim models, over Nordic's
 * nrf52840.svd: CLOCK, the GPIO ports, UART0/UARTE0 and UARTE1, TIMER0-4,
 * RTC0-2; and its FICR. The nRF52840 Product Specification (v1.1) is the
 * reference.
 *
 * Every peripheral has Nordic's tasks, events and interrupts (the PS's
 * "Peripheral interface"): writing 1 to a task register (offsets below
 * 0x100) triggers it; an event register (0x100 + 4n) is set to 1 by the
 * hardware and cleared by writing 0; INTEN (0x300) holds an enable bit per
 * event (bit n for the event at 0x100 + 4n), INTENSET (0x304) and INTENCLR
 * (0x308) set and clear its bits and read it. The peripheral's interrupt
 * is raised while an enabled event is set: when it sets, and again when a
 * write leaves it set. SHORTS connect an event to a task at once.
 *
 *   - CLOCK: HFCLKSTART starts the 64 MHz crystal (HFXO): HFCLKSTARTED and
 *     HFCLKSTAT (Xtal, running) once HFXODEBOUNCE x 16 us have passed
 *     (the PS's debounce; the crystal's power-up time is not counted);
 *     HFCLKSTOP stops it. LFCLKSTART starts the 32.768 kHz clock from the
 *     source LFCLKSRC names (copied to LFCLKSRCCOPY): LFCLKSTARTED and
 *     LFCLKSTAT at once; LFCLKSTOP stops it. The core runs at 64 MHz: the
 *     board's time (s->now_ns) is its cycles at it.
 *   - P0, P1: OUT, OUTSET, OUTCLR; DIR, DIRSET, DIRCLR, and PIN_CNF[n]'s
 *     DIR, the same bits; IN is each pin with its input buffer connected
 *     (PIN_CNF.INPUT 0): an output what OUT drives -- unless DRIVE lets
 *     the level go (D0S1, D0H1 for 0; S0D1, H0D1 for 1) --, else what the
 *     board drives, else the pull (PULL up 1, down 0), else 0 (floating,
 *     EmbSim's choice); a disconnected input reads 0. Each pin's level as
 *     it changes goes to the board (port 0 is P0). LATCH, DETECT and
 *     SENSE are not modelled.
 *   - UART0 (ENABLE 4): after STARTTX, a byte written to TXD is sent
 *     (stdout) and TXDRDY set; after STARTRX the run's input (--input)
 *     comes into RXD's six-byte FIFO, RXDRDY each; STOPRX gives RXTO.
 *     UARTE0 (ENABLE 8, the same space) and UARTE1: STARTTX sends
 *     TXD.MAXCNT bytes from TXD.PTR (TXSTARTED, TXDRDY each, ENDTX,
 *     TXD.AMOUNT); STOPTX gives TXSTOPPED; STARTRX (RXSTARTED) puts the
 *     input's bytes at RXD.PTR, RXDRDY each, ENDRX and RXD.AMOUNT at
 *     RXD.MAXCNT; STOPRX ends the buffer (ENDRX, RXTO); SHORTS'
 *     ENDRX_STARTRX and ENDRX_STOPRX. EasyDMA reads and writes Data RAM
 *     alone (the PS: elsewhere "may result in a HardFault or RAM
 *     corruption"): a buffer outside it is a warning, and nothing moves.
 *     A byte takes no time.
 *   - TIMER0-4: START, STOP, CLEAR, CAPTURE[n], SHUTDOWN; in Timer mode
 *     the counter counts at 16 MHz / 2^PRESCALER (PRESCALER above 9 is
 *     9), in Counter modes at each COUNT; BITMODE's width (16, 8, 24, 32)
 *     wraps it. COMPARE[n] when the counter becomes CC[n]; SHORTS'
 *     COMPAREn_CLEAR and COMPAREn_STOP. TIMER0-2 have four CC registers,
 *     TIMER3-4 six (the PS's instance table; the SVD gives each six: the
 *     two extra keep what is written).
 *   - RTC0-2, while the LFCLK runs: the 24-bit COUNTER at 32768 Hz /
 *     (PRESCALER + 1), the prescaler latched by START, CLEAR and
 *     TRIGOVRFLW (which sets COUNTER to 0xFFFFF0) and PRESCALER read-only
 *     while it runs; TICK at each count, OVRFLW from 0xFFFFFF to 0,
 *     COMPARE[n] when COUNTER becomes CC[n] -- an event set only when
 *     EVTEN or INTEN enables it. RTC0 has three CC registers, RTC1-2
 *     four.
 *   - FICR (rom: a device): CODEPAGESIZE 4096, CODESIZE 256, INFO.PART
 *     0x52840, VARIANT "AACA", PACKAGE QI, RAM 256 KiB, FLASH 1 MiB, a
 *     DEVICEID of EmbSim's; every other word 0xFFFFFFFF, as unprogrammed
 *     (so the MDK's errata checks find no revision to work around).
 *
 * Not modelled: PPI, GPIOTE, the radio and the rest (their registers keep
 * what is written), the UARTs' baud rate, flow control and errors, the
 * TIMER's PCLK1M choice, the RTC's "may not trigger" cases (writing N or
 * N + 1 to CC when COUNTER is N: EmbSim triggers at the transition). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "svd-map.h"

#define CPU_HZ 64000000u
#define DATA_RAM 0x20000000u
#define DATA_RAM_SIZE 0x40000u

/* ---- the peripheral interface: tasks, events, INTEN, the interrupt -------- */

struct nrfp {
    struct sim *s;
    struct rf_periph *p;
    u32 base;
    int irq;
    u32 inten;
};

static void nrfp_init(struct nrfp *x, struct sim *s, struct rf_periph *p)
{
    x->s = s;
    x->p = p;
    x->base = p->base;
    x->irq = rf_irq(p, 0);
    x->inten = 0;
}

static struct rf_reg *nreg(struct nrfp *x, u32 off)
{
    return rf_reg_at(x->s->svd, x->base + off);
}

static u32 rd(struct nrfp *x, u32 off)
{
    struct rf_reg *r = nreg(x, off);
    return r ? (u32)r->value : 0;
}

/* the hardware sets a register (traced) */
static void hw(struct nrfp *x, u32 off, u32 v)
{
    struct rf_reg *r = nreg(x, off);
    if (r && r->value != v)
        rf_hw(r, v);
}

static int line(struct nrfp *x)
{
    for (int n = 0; n < 32; n++)
        if ((x->inten >> n & 1) && rd(x, 0x100 + 4u * (u32)n))
            return 1;
    return 0;
}

/* the interrupt: when it rises, and with `again` when it stays up */
static void irq(struct nrfp *x, int was, int again)
{
    if (line(x) && (!was || again) && x->irq >= 0)
        rf_interrupt(x->p, x->irq);
}

/* an event happens */
static void event(struct nrfp *x, u32 off)
{
    int was = line(x);
    hw(x, off, 1);
    irq(x, was, 0);
}

/* INTEN, INTENSET, INTENCLR: read as INTEN; 1 if the access was one */
static int inten_read(struct nrfp *x, u32 off)
{
    if (off < 0x300 || off > 0x308)
        return 0;
    struct rf_reg *r = nreg(x, off);
    if (r)
        r->value = x->inten;
    return 1;
}

static int inten_write(struct nrfp *x, u32 off, u32 v)
{
    if (off == 0x300)
        x->inten = v;
    else if (off == 0x304)
        x->inten |= v;
    else if (off == 0x308)
        x->inten &= ~v;
    else
        return 0;
    struct rf_reg *r = nreg(x, off);
    if (r)
        r->value = x->inten;
    return 1;
}

/* ---- CLOCK, and the board's time --------------------------------------- */

struct nclock {
    struct nrfp x;
    u64 dev;                        /* the cycles the devices were told of */
    u64 hf_at;                      /* the HFXO starting: ready at this */
    int hf_starting, hf_on, lf_on, running;
};

static double clock_now_ns(struct sim *s);

static struct nclock *clock_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "CLOCK") : 0;
    return p && p->ops == &nrf_clock_ops ? p->ctx : 0;
}

static double clock_now_ns(struct sim *s)
{
    struct nclock *c = clock_of(s);
    return c ? (double)c->dev * 1e9 / CPU_HZ : (double)s->cycles * 1e9 / CPU_HZ;
}

/* whether the 32.768 kHz clock runs (the RTCs count on it) */
static int lfclk_on(struct sim *s)
{
    struct nclock *c = clock_of(s);
    return c && c->lf_on;
}

static void clock_status(struct nclock *c)
{
    hw(&c->x, 0x40c, c->hf_on ? 0x10001u : 0);
    hw(&c->x, 0x418, c->lf_on ? 0x10000u | (rd(&c->x, 0x41c) & 3) : 0);
}

static void clock_task(struct nclock *c, u32 off)
{
    switch (off) {
    case 0x000:                     /* HFCLKSTART */
        hw(&c->x, 0x408, 1);
        if (!c->hf_on && !c->hf_starting) {
            c->hf_starting = 1;
            c->hf_at = c->dev + (u64)(rd(&c->x, 0x528) & 0xff) * 16 * (CPU_HZ / 1000000);
        }
        break;
    case 0x004:                     /* HFCLKSTOP */
        c->hf_on = c->hf_starting = 0;
        hw(&c->x, 0x408, 0);
        break;
    case 0x008:                     /* LFCLKSTART */
        hw(&c->x, 0x414, 1);
        hw(&c->x, 0x41c, rd(&c->x, 0x518) & 3);
        c->lf_on = 1;
        clock_status(c);
        event(&c->x, 0x104);
        break;
    case 0x00c:                     /* LFCLKSTOP */
        c->lf_on = 0;
        hw(&c->x, 0x414, 0);
        break;
    }
    clock_status(c);
}

static void clock_tick(void *ctx, u32 cycles)
{
    struct nclock *c = ctx;
    c->dev += cycles;
    if (c->hf_starting && c->dev >= c->hf_at) {
        c->hf_starting = 0;
        c->hf_on = 1;
        clock_status(c);
        event(&c->x, 0x100);        /* HFCLKSTARTED */
    }
}

static int clock_next(void *ctx, u32 *cycles)
{
    struct nclock *c = ctx;
    if (!c->hf_starting || !(c->x.inten & 1))
        return 0;
    *cycles = c->hf_at > c->dev ? (u32)(c->hf_at - c->dev) : 1;
    return 1;
}

static u32 clock_read(void *ctx, u32 off, int n)
{
    struct nclock *c = ctx;
    inten_read(&c->x, off);
    return rf_read(c->x.p, off, n);
}

static void clock_write(void *ctx, u32 off, int n, u32 v)
{
    struct nclock *c = ctx;
    int was = line(&c->x);
    rf_write(c->x.p, off, n, v);
    inten_write(&c->x, off, v);
    if (off < 0x100 && (v & 1))
        clock_task(c, off);
    irq(&c->x, was, 1);
}

static void clock_reset(void *ctx)
{
    struct nclock *c = ctx;
    c->dev = 0;
    c->hf_starting = c->hf_on = c->lf_on = 0;
    c->running = 0;
    c->x.inten = 0;
    clock_status(c);
    rf_clock(c->x.p, &c->running, 1);   /* the board's time, from reset */
}

const struct dev_ops nrf_clock_ops = {
    "nrf-clock", clock_read, clock_write, clock_reset, clock_tick, clock_next,
};

void *nrf_clock_create(struct sim *s, struct rf_periph *p)
{
    struct nclock *c = calloc(1, sizeof *c);
    if (!c)
        die("out of memory");
    nrfp_init(&c->x, s, p);
    s->now_ns = clock_now_ns;
    rf_clock(p, &c->running, 1);
    return c;
}

/* ---- GPIO --------------------------------------------------------------- */

struct ngpio {
    struct nrfp x;
    int port;
    int out[32];                    /* told to the board: 0, 1, -1 let go */
    int known;
};

/* DIR's bits are PIN_CNF's DIR bits */
static void gpio_dir(struct ngpio *g, u32 dir)
{
    for (int i = 0; i < 32; i++) {
        struct rf_reg *r = nreg(&g->x, 0x700 + 4u * (u32)i);
        if (r && (u32)(r->value & 1) != (dir >> i & 1))
            rf_hw(r, (r->value & ~1ull) | (dir >> i & 1));
    }
}

static void gpio_update(struct ngpio *g)
{
    struct sim *s = g->x.s;
    u32 out = rd(&g->x, 0x504), in = 0, dir = 0;
    for (int i = 0; i < 32; i++) {
        struct rf_reg *r = nreg(&g->x, 0x700 + 4u * (u32)i);
        if (!r)
            continue;
        u32 c = (u32)r->value, drive = c >> 8 & 7, pull = c >> 2 & 3;
        int o = -1, lvl, ext;
        dir |= (c & 1) << i;
        if (c & 1) {
            int v = (int)(out >> i & 1);
            int let_go = v ? (drive == 6 || drive == 7) : (drive == 4 || drive == 5);
            if (!let_go)
                o = v;
        }
        if (o >= 0)
            lvl = o;
        else if (s->io.pin_in && s->io.pin_in(s->io.ctx, g->port, i, &ext))
            lvl = ext != 0;
        else
            lvl = pull == 3 ? 1 : 0;
        if (!(c & 2))
            in |= (u32)lvl << i;
        if (s->io.pin_out && (!g->known || g->out[i] != o))
            s->io.pin_out(s->io.ctx, g->port, i, o);
        g->out[i] = o;
    }
    g->known = 1;
    hw(&g->x, 0x510, in);
    hw(&g->x, 0x514, dir);
    hw(&g->x, 0x518, dir);
    hw(&g->x, 0x51c, dir);
    hw(&g->x, 0x508, out);
    hw(&g->x, 0x50c, out);
}

static u32 gpio_read(void *ctx, u32 off, int n)
{
    struct ngpio *g = ctx;
    gpio_update(g);
    return rf_read(g->x.p, off, n);
}

static void gpio_write(void *ctx, u32 off, int n, u32 v)
{
    struct ngpio *g = ctx;
    u32 out = rd(&g->x, 0x504), dir = rd(&g->x, 0x514);
    rf_write(g->x.p, off, n, v);
    switch (off) {
    case 0x504: out = v; break;
    case 0x508: out |= v; break;
    case 0x50c: out &= ~v; break;
    case 0x514: gpio_dir(g, v); break;
    case 0x518: gpio_dir(g, dir | v); break;
    case 0x51c: gpio_dir(g, dir & ~v); break;
    }
    hw(&g->x, 0x504, out);
    gpio_update(g);
}

static void gpio_reset(void *ctx)
{
    struct ngpio *g = ctx;
    g->known = 0;
    gpio_update(g);
}

const struct dev_ops nrf_gpio_ops = {
    "nrf-gpio", gpio_read, gpio_write, gpio_reset, 0, 0,
};

void *nrf_gpio_create(struct sim *s, struct rf_periph *p)
{
    if (strcmp(p->name, "P0") && strcmp(p->name, "P1"))
        return 0;
    struct ngpio *g = calloc(1, sizeof *g);
    if (!g)
        die("out of memory");
    nrfp_init(&g->x, s, p);
    g->port = p->name[1] - '0';
    return g;
}

/* the board drove a pin or let it go: both ports again */
void nrf_pins_changed(struct sim *s)
{
    static const char *const ports[] = { "P0", "P1" };
    for (int i = 0; i < 2 && s->svd; i++) {
        struct rf_periph *p = rf_periph(s->svd, ports[i]);
        if (p && p->ops == &nrf_gpio_ops)
            gpio_update(p->ctx);
    }
}

/* ---- UART0, UARTE0, UARTE1 ---------------------------------------------- */

#define RXFIFO 6

struct nuart {
    struct nrfp x;
    int tx_on, rx_on;               /* UART: started */
    u8 fifo[RXFIFO];                /* UART's RXD FIFO */
    int nfifo;
    u32 rx_ptr, rx_max, rx_n;       /* UARTE's buffer being filled */
    int rx_buf;
};

static int easydma_ok(struct nuart *u, u32 ptr, u32 n, const char *what)
{
    if (!n || (ptr >= DATA_RAM && ptr - DATA_RAM < DATA_RAM_SIZE &&
               DATA_RAM_SIZE - (ptr - DATA_RAM) >= n))
        return 1;
    fflush(stdout);
    fprintf(stderr, "embsim: warning: %s's %s 0x%08x is not in Data RAM: "
            "EasyDMA reaches only that (the part may fault or corrupt RAM); "
            "nothing is moved\n", u->x.p->name, what, ptr);
    return 0;
}

static u32 enabled(struct nuart *u)
{
    return rd(&u->x, 0x500) & 15;
}

static void uart_task(struct nuart *u, u32 off)
{
    struct sim *s = u->x.s;
    u32 en = enabled(u);
    switch (off) {
    case 0x000:                     /* STARTRX */
        if (en == 8) {
            u->rx_ptr = rd(&u->x, 0x534);
            u->rx_max = rd(&u->x, 0x538) & 0xffff;
            u->rx_n = 0;
            u->rx_buf = easydma_ok(u, u->rx_ptr, u->rx_max, "RXD.PTR");
            event(&u->x, 0x14c);    /* RXSTARTED */
        }
        u->rx_on = en == 4 || en == 8;
        break;
    case 0x004:                     /* STOPRX */
        if (u->rx_on) {
            u->rx_on = 0;
            if (en == 8 && u->rx_buf) {
                hw(&u->x, 0x53c, u->rx_n);
                u->rx_buf = 0;
                event(&u->x, 0x110);        /* ENDRX */
            }
            event(&u->x, 0x144);    /* RXTO */
        }
        break;
    case 0x008:                     /* STARTTX */
        if (en == 4)
            u->tx_on = 1;
        if (en == 8) {
            u32 ptr = rd(&u->x, 0x544), n = rd(&u->x, 0x548) & 0xffff;
            event(&u->x, 0x150);    /* TXSTARTED */
            if (!easydma_ok(u, ptr, n, "TXD.PTR"))
                n = 0;
            for (u32 i = 0; i < n; i++) {
                u32 b;
                bus_debug_read(&s->bus, ptr + i, 1, &b);
                sim_out(s, (int)(b & 0xff));
                event(&u->x, 0x11c);        /* TXDRDY */
            }
            hw(&u->x, 0x54c, n);
            event(&u->x, 0x120);    /* ENDTX */
        }
        break;
    case 0x00c:                     /* STOPTX */
        u->tx_on = 0;
        if (en == 8)
            event(&u->x, 0x158);    /* TXSTOPPED */
        break;
    }
}

static u32 uart_read(void *ctx, u32 off, int n)
{
    struct nuart *u = ctx;
    inten_read(&u->x, off);
    if (off == 0x518 && enabled(u) == 4) {         /* RXD: the FIFO's head */
        u32 v = u->nfifo ? u->fifo[0] : 0;
        if (u->nfifo && !u->x.s->bus.debug) {
            memmove(u->fifo, u->fifo + 1, (size_t)--u->nfifo);
            if (u->nfifo)
                event(&u->x, 0x108);        /* RXDRDY: the next */
        }
        return v;
    }
    return rf_read(u->x.p, off, n);
}

static void uart_write(void *ctx, u32 off, int n, u32 v)
{
    struct nuart *u = ctx;
    int was = line(&u->x);
    struct rf_reg *r = nreg(&u->x, off);
    /* a register of the peripheral sharing the space (UART0 and UARTE0)
     * is written as its own */
    if (r)
        rf_write(r->p, off, n, v);
    inten_write(&u->x, off, v);
    if (off < 0x100 && (v & 1))
        uart_task(u, off);
    if (off == 0x51c && enabled(u) == 4 && u->tx_on) {    /* TXD */
        sim_out(u->x.s, (int)(v & 0xff));
        event(&u->x, 0x11c);
    }
    irq(&u->x, was, 1);
}

static void uart_reset(void *ctx)
{
    struct nuart *u = ctx;
    u->tx_on = u->rx_on = u->nfifo = 0;
    u->rx_buf = 0;
    u->x.inten = 0;
}

static int uart_room(void *ctx)
{
    struct nuart *u = ctx;
    u32 en = enabled(u);
    return u->rx_on && ((en == 4 && u->nfifo < RXFIFO) || (en == 8 && u->rx_buf));
}

static void uart_put(void *ctx, int c)
{
    struct nuart *u = ctx;
    struct sim *s = u->x.s;
    if (enabled(u) == 4) {
        u->fifo[u->nfifo++] = (u8)c;
        if (u->nfifo == 1) {
            hw(&u->x, 0x518, (u32)c & 0xff);
            event(&u->x, 0x108);
        }
        return;
    }
    bus_debug_write(&s->bus, u->rx_ptr + u->rx_n++, 1, (u32)c & 0xff);
    event(&u->x, 0x108);            /* RXDRDY */
    if (u->rx_n == u->rx_max) {
        hw(&u->x, 0x53c, u->rx_n);
        u->rx_buf = 0;
        event(&u->x, 0x110);        /* ENDRX */
        u32 sh = rd(&u->x, 0x200);
        if (sh & (1u << 5))
            uart_task(u, 0x000);    /* ENDRX_STARTRX */
        if (sh & (1u << 6))
            uart_task(u, 0x004);    /* ENDRX_STOPRX */
    }
}

const struct dev_ops nrf_uart_ops = {
    "nrf-uart", uart_read, uart_write, uart_reset, 0, 0,
};

void *nrf_uart_create(struct sim *s, struct rf_periph *p)
{
    /* UART0 and UARTE0 share their space: one model for both */
    if (!strcmp(p->name, "UART0")) {
        struct rf_periph *e = rf_periph(s->svd, "UARTE0");
        if (e && e->ops == &nrf_uart_ops)
            return e->ctx;
    }
    if (strncmp(p->name, "UART", 4))
        return 0;
    struct nuart *u = calloc(1, sizeof *u);
    if (!u)
        die("out of memory");
    nrfp_init(&u->x, s, p);
    if (!strcmp(p->name, "UARTE0"))
        sim_rx_port(s, uart_room, uart_put, u);
    return u;
}

/* ---- TIMER0-4 -------------------------------------------------------------- */

struct ntimer {
    struct nrfp x;
    int ncc, on, running;
    u32 count;
    u32 frac;                       /* core cycles toward the next tick */
};

static u32 tim_mask(struct ntimer *t)
{
    static const u32 m[4] = { 0xffff, 0xff, 0xffffff, 0xffffffff };
    return m[rd(&t->x, 0x508) & 3];
}

static u32 tim_period(struct ntimer *t)     /* core cycles per tick */
{
    u32 p = rd(&t->x, 0x510) & 15;
    return 4u << (p > 9 ? 9 : p);
}

static int tim_timer_mode(struct ntimer *t)
{
    return (rd(&t->x, 0x504) & 3) == 0;
}

static void tim_clock(struct ntimer *t)
{
    rf_clock(t->x.p, &t->running, t->on && tim_timer_mode(t));
}

/* the counter is now `count`: the compares it meets, and their shorts */
static void tim_set(struct ntimer *t, u32 count)
{
    t->count = count & tim_mask(t);
    for (int n = 0; n < t->ncc; n++)
        if (t->count == (rd(&t->x, 0x540 + 4u * (u32)n) & tim_mask(t))) {
            event(&t->x, 0x140 + 4u * (u32)n);
            u32 sh = rd(&t->x, 0x200);
            if (sh >> n & 1)
                t->count = 0;
            if (sh >> (8 + n) & 1) {
                t->on = 0;
                tim_clock(t);
            }
        }
}

static void tim_tick(void *ctx, u32 cycles)
{
    struct ntimer *t = ctx;
    u32 per = tim_period(t);
    t->frac += cycles;
    while (t->frac >= per && t->on) {
        t->frac -= per;
        tim_set(t, t->count + 1);
    }
}

static int tim_next(void *ctx, u32 *cycles)
{
    struct ntimer *t = ctx;
    u32 best = 0, per = tim_period(t), m = tim_mask(t);
    int any = 0;
    if (!t->on || !tim_timer_mode(t))
        return 0;
    for (int n = 0; n < t->ncc; n++) {
        if (!(t->x.inten >> (16 + n) & 1))
            continue;
        u32 d = ((rd(&t->x, 0x540 + 4u * (u32)n) - t->count) & m);
        if (!d)
            d = m + 1 ? m + 1 : 0xffffffffu;
        if (!any || d < best)
            best = d;
        any = 1;
    }
    if (!any)
        return 0;
    u64 c = (u64)best * per - t->frac;
    *cycles = c > 0xffffffffu ? 0xffffffffu : (u32)c;
    return 1;
}

static u32 tim_read(void *ctx, u32 off, int n)
{
    struct ntimer *t = ctx;
    inten_read(&t->x, off);
    return rf_read(t->x.p, off, n);
}

static void tim_write(void *ctx, u32 off, int n, u32 v)
{
    struct ntimer *t = ctx;
    int was = line(&t->x);
    rf_write(t->x.p, off, n, v);
    inten_write(&t->x, off, v);
    if (off < 0x100 && (v & 1)) {
        switch (off) {
        case 0x000: t->on = 1; break;               /* START */
        case 0x004: t->on = 0; break;               /* STOP */
        case 0x008:                                 /* COUNT */
            if (!tim_timer_mode(t) && t->on)
                tim_set(t, t->count + 1);
            break;
        case 0x00c: t->count = 0; break;            /* CLEAR */
        case 0x010: t->on = 0; t->count = 0; break; /* SHUTDOWN */
        default:
            if (off >= 0x040 && off < 0x040 + 4u * (u32)t->ncc)  /* CAPTURE */
                hw(&t->x, 0x540 + (off - 0x040), t->count);
        }
    }
    tim_clock(t);
    irq(&t->x, was, 1);
}

static void tim_reset(void *ctx)
{
    struct ntimer *t = ctx;
    t->on = t->running = 0;
    t->count = t->frac = 0;
    t->x.inten = 0;
}

const struct dev_ops nrf_timer_ops = {
    "nrf-timer", tim_read, tim_write, tim_reset, tim_tick, tim_next,
};

void *nrf_timer_create(struct sim *s, struct rf_periph *p)
{
    if (strncmp(p->name, "TIMER", 5))
        return 0;
    struct ntimer *t = calloc(1, sizeof *t);
    if (!t)
        die("out of memory");
    nrfp_init(&t->x, s, p);
    t->ncc = !strcmp(p->name, "TIMER3") || !strcmp(p->name, "TIMER4") ? 6 : 4;
    return t;
}

/* ---- RTC0-2 ------------------------------------------------------------------- */

struct nrtc {
    struct nrfp x;
    int ncc, on, running;
    u32 presc;                      /* the latched prescaler */
    u64 acc;                        /* core cycles x 32768 toward a tick */
};

/* an RTC event, set only when EVTEN or INTEN has it */
static void rtc_event(struct nrtc *r, u32 off)
{
    u32 bit = 1u << ((off - 0x100) / 4);
    if ((rd(&r->x, 0x340) | r->x.inten) & bit)
        event(&r->x, off);
}

static void rtc_clock(struct nrtc *r)
{
    rf_clock(r->x.p, &r->running, r->on);
}

static void rtc_count(struct nrtc *r)
{
    u32 c = (rd(&r->x, 0x504) + 1) & 0xffffff;
    struct rf_reg *reg = nreg(&r->x, 0x504);
    if (reg)
        reg->value = c;             /* every count: not traced */
    rtc_event(r, 0x100);            /* TICK */
    if (!c)
        rtc_event(r, 0x104);        /* OVRFLW */
    for (int n = 0; n < r->ncc; n++)
        if (c == (rd(&r->x, 0x540 + 4u * (u32)n) & 0xffffff))
            rtc_event(r, 0x140 + 4u * (u32)n);
}

static void rtc_tick(void *ctx, u32 cycles)
{
    struct nrtc *r = ctx;
    if (!lfclk_on(r->x.s))
        return;
    u64 one = (u64)CPU_HZ * (r->presc + 1);
    r->acc += (u64)cycles * 32768;
    while (r->acc >= one) {
        r->acc -= one;
        rtc_count(r);
    }
}

static int rtc_next(void *ctx, u32 *cycles)
{
    struct nrtc *r = ctx;
    if (!r->on || !lfclk_on(r->x.s))
        return 0;
    u32 c = rd(&r->x, 0x504), best = 0;
    int any = 0;
    for (int n = 0; n < 32; n++) {
        if (!(r->x.inten >> n & 1))
            continue;
        u32 d;
        if (n == 0)
            d = 1;                              /* TICK */
        else if (n == 1)
            d = (0x1000000u - c) & 0xffffff;    /* OVRFLW */
        else if (n >= 16 && n < 16 + r->ncc)
            d = (rd(&r->x, 0x540 + 4u * (u32)(n - 16)) - c) & 0xffffff;
        else
            continue;
        if (!d)
            d = 0x1000000u;
        if (!any || d < best)
            best = d;
        any = 1;
    }
    if (!any)
        return 0;
    u64 one = (u64)CPU_HZ * (r->presc + 1);
    u64 need = (u64)best * one - r->acc;
    u64 cy = (need + 32767) / 32768;
    *cycles = cy > 0xffffffffu ? 0xffffffffu : (u32)cy;
    return 1;
}

static u32 rtc_read(void *ctx, u32 off, int n)
{
    struct nrtc *r = ctx;
    inten_read(&r->x, off);
    if (off >= 0x340 && off <= 0x348) {         /* EVTEN, EVTENSET, EVTENCLR */
        struct rf_reg *e = nreg(&r->x, off);
        if (e)
            e->value = rd(&r->x, 0x340);
    }
    return rf_read(r->x.p, off, n);
}

static void rtc_write(void *ctx, u32 off, int n, u32 v)
{
    struct nrtc *r = ctx;
    int was = line(&r->x);
    u32 presc = rd(&r->x, 0x508), evten = rd(&r->x, 0x340);
    rf_write(r->x.p, off, n, v);
    inten_write(&r->x, off, v);
    if (off == 0x508 && r->on)
        hw(&r->x, 0x508, presc);    /* read-only while it runs */
    if (off == 0x344)
        hw(&r->x, 0x340, evten | v);
    if (off == 0x348)
        hw(&r->x, 0x340, evten & ~v);
    if (off < 0x100 && (v & 1)) {
        switch (off) {
        case 0x000: r->on = 1; break;           /* START */
        case 0x004: r->on = 0; break;           /* STOP */
        case 0x008: hw(&r->x, 0x504, 0); break; /* CLEAR */
        case 0x00c: hw(&r->x, 0x504, 0xfffff0); break;   /* TRIGOVRFLW */
        }
        if (off == 0x000 || off == 0x008 || off == 0x00c) {
            r->presc = rd(&r->x, 0x508) & 0xfff;
            r->acc = 0;
        }
    }
    rtc_clock(r);
    irq(&r->x, was, 1);
}

static void rtc_reset(void *ctx)
{
    struct nrtc *r = ctx;
    r->on = r->running = 0;
    r->presc = 0;
    r->acc = 0;
    r->x.inten = 0;
}

const struct dev_ops nrf_rtc_ops = {
    "nrf-rtc", rtc_read, rtc_write, rtc_reset, rtc_tick, rtc_next,
};

void *nrf_rtc_create(struct sim *s, struct rf_periph *p)
{
    if (strncmp(p->name, "RTC", 3))
        return 0;
    struct nrtc *r = calloc(1, sizeof *r);
    if (!r)
        die("out of memory");
    nrfp_init(&r->x, s, p);
    r->ncc = !strcmp(p->name, "RTC0") ? 3 : 4;
    return r;
}

/* ---- FICR --------------------------------------------------------------------- */

static u32 ficr_word(u32 off)
{
    switch (off) {
    case 0x010: return 4096;        /* CODEPAGESIZE */
    case 0x014: return 256;         /* CODESIZE */
    case 0x060: return 0x53626d45u; /* DEVICEID: "EmbSim" */
    case 0x064: return 0x00006d69u;
    case 0x100: return 0x52840u;    /* INFO.PART */
    case 0x104: return 0x41414341u; /* INFO.VARIANT "AACA" */
    case 0x108: return 0x2004u;     /* INFO.PACKAGE: QI */
    case 0x10c: return 0x100u;      /* INFO.RAM: 256 KiB */
    case 0x110: return 0x400u;      /* INFO.FLASH: 1 MiB */
    }
    return 0xffffffffu;
}

static u32 ficr_read(void *ctx, u32 off, int n)
{
    (void)ctx;
    return ficr_word(off & ~3u) >> (8 * (off & 3)) & (n == 4 ? 0xffffffffu : (1u << (8 * n)) - 1);
}

const struct dev_ops nrf52_ficr_ops = { "nrf52840-ficr", ficr_read, 0, 0, 0, 0 };

void *nrf52_ficr_create(struct sim *s, const struct dev_desc *d)
{
    (void)s;
    (void)d;
    return 0;
}
