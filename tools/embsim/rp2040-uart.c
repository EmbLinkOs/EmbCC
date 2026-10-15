/* rp2040-uart.c -- the RP2040's UART0 and UART1, ARM's PL011 (its
 * datasheet, 4.2), over RP2040.svd's registers.
 *
 *   - a write to UARTDR goes into the transmit FIFO (32 deep with
 *     UARTLCR_H.FEN, else one); the FIFO is sent at once, to the run's
 *     stdout, while UARTCR's UARTEN and TXE are set and clk_peri runs (a
 *     byte takes no time); a byte written while it cannot go waits, and
 *     one written to a full FIFO is lost;
 *   - the receiver is the run's input (--input) on UART0: a byte comes in
 *     while UARTEN and RXE are set and the receive FIFO has room, and a
 *     read of UARTDR takes it (its error bits 0);
 *   - UARTFR says how full each FIFO is (TXFE, TXFF, RXFE, RXFF; BUSY while
 *     a byte waits to go);
 *   - the interrupts, UARTRIS: TXRIS when a byte has gone and the transmit
 *     FIFO is at or below its trigger level (UARTIFLS; empty, without the
 *     FIFO), as the PL011's is set by a transition through the level, not
 *     by the level; RXRIS when the receive FIFO reaches its level (holds a
 *     byte, without the FIFO), cleared by reading it below; RTRIS, the
 *     receive timeout, at once when a byte waits below the level (EmbSim
 *     does not count the 32 bit periods), cleared when the FIFO empties.
 *     UARTICR clears them; UARTMIS is UARTRIS & UARTIMSC, and its bits
 *     raise UART0_IRQ or UART1_IRQ, when one sets and when a write leaves
 *     one set.
 * Every UART writes to stdout, whatever its pins' function select.
 * Not modelled: the baud rate's time, the line's format (UARTLCR_H's word
 * length, parity, stop bits and break), the modem lines, the error flags,
 * IrDA, DMA. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"
#include "svd-map.h"

#define FIFO 32

struct uart {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *dr, *fr, *lcrh, *cr, *ifls, *imsc, *ris, *mis, *icr;
    u64 m_fen, m_uarten, m_txe, m_rxe, m_txfe, m_txff, m_rxfe, m_rxff, m_busy;
    u64 m_txris, m_rxris, m_rtris, m_txsel, m_rxsel;
    int irq;
    u8 rx[FIFO], tx[FIFO];
    int nrx, rxh, ntx;
};

static u32 fld(const struct rf_reg *r, u64 mask)
{
    int sh = 0;
    if (!mask)
        return 0;
    while (!(mask >> sh & 1))
        sh++;
    return (u32)((r->value & mask) >> sh);
}

static int depth(const struct uart *u)
{
    return u->lcrh->value & u->m_fen ? FIFO : 1;
}

/* a trigger level: 1/8, 1/4, 1/2, 3/4 or 7/8 of the FIFO */
static int level(u32 sel)
{
    static const int l[] = { 4, 8, 16, 24, 28 };
    return sel < 5 ? l[sel] : 16;
}

static void flags(struct uart *u)
{
    u64 v = u->fr->value & ~(u->m_txfe | u->m_txff | u->m_rxfe | u->m_rxff | u->m_busy);
    if (!u->ntx)
        v |= u->m_txfe;
    else
        v |= u->m_busy;
    if (u->ntx >= depth(u))
        v |= u->m_txff;
    if (!u->nrx)
        v |= u->m_rxfe;
    if (u->nrx >= depth(u))
        v |= u->m_rxff;
    if (v != u->fr->value)
        rf_hw(u->fr, v);
}

static void irq(struct uart *u, int again)
{
    u64 was = u->mis->value, m = u->ris->value & u->imsc->value;
    if (m != was)
        rf_hw(u->mis, m);
    if (m && (!was || again))
        rf_interrupt(u->p, u->irq);
}

static void ris(struct uart *u, u64 set, u64 clr)
{
    u64 v = (u->ris->value | set) & ~clr;
    if (v != u->ris->value)
        rf_hw(u->ris, v);
}

static int sending(struct uart *u)
{
    return (u->cr->value & u->m_uarten) && (u->cr->value & u->m_txe) &&
           rp2040_hz(u->s, RP_CLK_PERI);
}

/* what waits in the transmit FIFO goes, if it can */
static void drain(struct uart *u)
{
    if (!u->ntx || !sending(u))
        return;
    for (int i = 0; i < u->ntx; i++)
        sim_out(u->s, u->tx[i]);
    u->ntx = 0;
    ris(u, u->m_txris, 0);          /* emptied: through any trigger level */
}

static void rx_levels(struct uart *u)
{
    int full = u->lcrh->value & u->m_fen ? u->nrx >= level(fld(u->ifls, u->m_rxsel))
                                         : u->nrx >= 1;
    ris(u, (full ? u->m_rxris : 0) | (u->nrx && !full ? u->m_rtris : 0),
        (full ? 0 : u->m_rxris) | (u->nrx ? 0 : u->m_rtris));
}

static u32 uart_read(void *ctx, u32 off, int n)
{
    struct uart *u = ctx;
    drain(u);
    flags(u);
    if (off == rf_off(u->dr)) {
        u32 v = u->nrx ? u->rx[u->rxh] : 0;
        if (!u->s->bus.debug && u->nrx) {
            u->rxh = (u->rxh + 1) % FIFO;
            u->nrx--;
            rx_levels(u);
            flags(u);
            irq(u, 0);
        }
        u->dr->value = v;
        return rf_read(u->p, off, n) & 0xff;
    }
    return rf_read(u->p, off, n);
}

static void uart_write(void *ctx, u32 off, int n, u32 v)
{
    struct uart *u = ctx;
    rf_write(u->p, off, n, v);
    if (off == rf_off(u->dr)) {
        if (u->ntx < depth(u))
            u->tx[u->ntx++] = (u8)v;
        if (u->ntx > (u->lcrh->value & u->m_fen ? level(fld(u->ifls, u->m_txsel)) : 0))
            ris(u, 0, u->m_txris);
    }
    if (off == rf_off(u->icr))
        ris(u, 0, v & 0x7ff);
    drain(u);
    rx_levels(u);
    flags(u);
    irq(u, 1);
}

static void uart_reset(void *ctx)
{
    struct uart *u = ctx;
    u->nrx = u->rxh = u->ntx = 0;
    flags(u);
}

const struct dev_ops rp2040_uart_ops = {
    "rp2040-uart", uart_read, uart_write, uart_reset, 0, 0,
};

static int uart_room(void *ctx)
{
    struct uart *u = ctx;
    return !rf_gated(u->p) && (u->cr->value & u->m_uarten) &&
           (u->cr->value & u->m_rxe) && u->nrx < depth(u);
}

static void uart_put(void *ctx, int c)
{
    struct uart *u = ctx;
    u->rx[(u->rxh + u->nrx++) % FIFO] = (u8)c;
    rx_levels(u);
    flags(u);
    irq(u, 0);
}

/* a clock started: what waited in the FIFOs goes */
void rp2040_uart_kick(struct sim *s)
{
    static const char *const names[] = { "UART0", "UART1" };
    for (int i = 0; i < 2 && s->svd; i++) {
        struct rf_periph *p = rf_periph(s->svd, names[i]);
        if (p && p->ops == &rp2040_uart_ops && !rf_gated(p)) {
            struct uart *u = p->ctx;
            drain(u);
            flags(u);
            irq(u, 0);
        }
    }
}

void *rp2040_uart_create(struct sim *s, struct rf_periph *p)
{
    struct uart *u = calloc(1, sizeof *u);
    if (!u)
        die("out of memory");
    u->s = s;
    u->p = p;
    if (!(u->dr = rf_reg(p, "UARTDR")) || !(u->fr = rf_reg(p, "UARTFR")) ||
        !(u->lcrh = rf_reg(p, "UARTLCR_H")) || !(u->cr = rf_reg(p, "UARTCR")) ||
        !(u->ifls = rf_reg(p, "UARTIFLS")) || !(u->imsc = rf_reg(p, "UARTIMSC")) ||
        !(u->ris = rf_reg(p, "UARTRIS")) || !(u->mis = rf_reg(p, "UARTMIS")) ||
        !(u->icr = rf_reg(p, "UARTICR"))) {
        free(u);
        return 0;                   /* not a PL011's layout */
    }
    u->m_fen = rf_mask(u->lcrh, "FEN");
    u->m_uarten = rf_mask(u->cr, "UARTEN");
    u->m_txe = rf_mask(u->cr, "TXE");
    u->m_rxe = rf_mask(u->cr, "RXE");
    u->m_txfe = rf_mask(u->fr, "TXFE");
    u->m_txff = rf_mask(u->fr, "TXFF");
    u->m_rxfe = rf_mask(u->fr, "RXFE");
    u->m_rxff = rf_mask(u->fr, "RXFF");
    u->m_busy = rf_mask(u->fr, "BUSY");
    u->m_txris = rf_mask(u->ris, "TXRIS");
    u->m_rxris = rf_mask(u->ris, "RXRIS");
    u->m_rtris = rf_mask(u->ris, "RTRIS");
    u->m_txsel = rf_mask(u->ifls, "TXIFLSEL");
    u->m_rxsel = rf_mask(u->ifls, "RXIFLSEL");
    u->irq = rf_irq(p, "_IRQ");
    if (!strcmp(p->name, "UART0"))
        sim_rx_port(s, uart_room, uart_put, u);
    return u;
}
