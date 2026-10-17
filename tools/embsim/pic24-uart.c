/* pic24-uart.c -- a PIC24FJ's UART1-UART6, over the EDC's registers
 * (UxMODE, UxSTA, UxTXREG, UxRXREG, UxBRG: one peripheral, UART).
 *
 *   - A byte written to UxTXREG while UxMODE.UARTEN and UxSTA.UTXEN are
 *     set goes to stdout at once: the transmitter is always empty (TRMT
 *     set, UTXBF clear). Setting UTXEN, and each byte sent, flags the
 *     transmit interrupt (UxTX), the buffer having room again.
 *   - The run's input (--input, or the replay of one) arrives at the
 *     first UART with UARTEN set: UxRXREG holds the byte, URXDA says so
 *     and its interrupt (UxRX) is flagged; reading UxRXREG takes it.
 * Not modelled: the baud rate (a byte takes no time), 9-bit mode, flow
 * control, the error flags and break, IrDA, the smart card interface. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NUNIT 6

struct unit {
    struct rf_reg *mode, *sta, *txreg, *rxreg;
    int irq_tx, irq_rx;
};

struct uart {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NUNIT];
};

#define STA_URXDA (1u << 0)
#define STA_RIDLE (1u << 4)
#define STA_TRMT  (1u << 8)
#define STA_UTXBF (1u << 9)
#define STA_UTXEN (1u << 10)
#define HW_BITS (STA_URXDA | STA_RIDLE | STA_TRMT | STA_UTXBF)

static int on(const struct unit *q)
{
    return q->mode && (q->mode->value >> 15 & 1);
}

static int sends(const struct unit *q)
{
    return on(q) && (q->sta->value & STA_UTXEN);
}

static u32 uart_read(void *ctx, u32 off, int n)
{
    struct uart *c = ctx;
    u32 v = rf_read(c->p, off, n);
    for (int k = 0; k < NUNIT && !c->s->bus.debug; k++) {
        struct unit *q = &c->u[k];
        if (q->rxreg && off == rf_off(q->rxreg))
            rf_hw(q->sta, q->sta->value & ~(u64)STA_URXDA);
    }
    return v;
}

static void uart_write(void *ctx, u32 off, int n, u32 v)
{
    struct uart *c = ctx;
    for (int k = 0; k < NUNIT; k++) {
        struct unit *q = &c->u[k];
        if (!q->sta)
            continue;
        if (off == rf_off(q->sta) || off == rf_off(q->sta) + 1) {
            int was = sends(q);
            u64 keep = q->sta->value & HW_BITS;
            rf_write(c->p, off, n, v);
            q->sta->value = (q->sta->value & ~(u64)HW_BITS) | keep;
            if (!was && sends(q))
                pic24_irq(c->s, q->irq_tx);
            return;
        }
        if (off == rf_off(q->txreg)) {
            rf_write(c->p, off, n, v);
            if (sends(q)) {
                sim_out(c->s, (int)(v & 0xff));
                pic24_irq(c->s, q->irq_tx);
            }
            return;
        }
        if (off == rf_off(q->mode) || off == rf_off(q->mode) + 1) {
            int was = sends(q);
            rf_write(c->p, off, n, v);
            if (!was && sends(q))
                pic24_irq(c->s, q->irq_tx);
            return;
        }
    }
    rf_write(c->p, off, n, v);
}

/* the input's receiver: the first UART on */
static struct unit *listener(struct uart *c)
{
    for (int k = 0; k < NUNIT; k++)
        if (on(&c->u[k]))
            return &c->u[k];
    return 0;
}

static int rx_room(void *ctx)
{
    struct unit *q = listener(ctx);
    return q && !(q->sta->value & STA_URXDA);
}

static void rx_put(void *ctx, int ch)
{
    struct uart *c = ctx;
    struct unit *q = listener(c);
    if (!q)
        return;
    q->rxreg->value = (u64)(ch & 0xff);
    rf_hw(q->sta, q->sta->value | STA_URXDA);
    pic24_irq(c->s, q->irq_rx);
}

const struct dev_ops pic24_uart_ops = {
    "pic24-uart", uart_read, uart_write, 0, 0, 0,
};

void *pic24_uart_create(struct sim *s, struct rf_periph *p)
{
    struct uart *c = calloc(1, sizeof *c);
    char nm[24];        /* "SPI" or "U", any int, the longest suffix: GCC counts it */
    int any = 0;
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    for (int k = 0; k < NUNIT; k++) {
        struct unit *q = &c->u[k];
#define REG(f, name) snprintf(nm, sizeof nm, "U%d" name, k + 1); q->f = rf_reg(p, nm)
        REG(mode, "MODE");
        REG(sta, "STA");
        REG(txreg, "TXREG");
        REG(rxreg, "RXREG");
#undef REG
        if (!q->mode || !q->sta || !q->txreg || !q->rxreg) {
            memset(q, 0, sizeof *q);
            continue;
        }
        any = 1;
        rf_set_reset(q->sta, STA_TRMT | STA_RIDLE);
        snprintf(nm, sizeof nm, "U%dTX", k + 1);
        q->irq_tx = pic24_irq_named(s, nm);
        snprintf(nm, sizeof nm, "U%dRX", k + 1);
        q->irq_rx = pic24_irq_named(s, nm);
    }
    if (!any) {
        free(c);
        return 0;
    }
    sim_rx_port(s, rx_room, rx_put, c);
    return c;
}
