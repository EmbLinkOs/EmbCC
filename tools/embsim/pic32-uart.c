/* pic32-uart.c -- a PIC32's UARTs, over the ATDF's registers (UxMODE,
 * UxSTA, UxTXREG, UxRXREG, UxBRG).
 *
 *   - A byte written to UxTXREG while UxMODE.ON and UxSTA.UTXEN are set
 *     goes to stdout at once: the transmitter is always empty (UxSTA.TRMT
 *     set, UTXBF clear), so printf works with any of the six. UxSTA's
 *     status bits are the hardware's: a write leaves them.
 *   - The transmit interrupt (UARTn_TX) is persistent while the buffer
 *     has room, as UTXISEL 00 and 10 have it on the part: with the
 *     transmitter on, its flag sets again as soon as it is cleared.
 *     UTXISEL 01 (when all is sent) is the same here.
 *   - The run's input (--input, or the replay of one) arrives at the
 *     first UART with ON and URXEN set: UxRXREG holds the byte, URXDA
 *     says so and its interrupt (UARTn_RX) is flagged; reading UxRXREG
 *     takes it. Without an input, nothing arrives.
 * Not modelled: the baud rate (a byte takes no time), 9-bit mode,
 * flow control, the error flags and break, IrDA, auto-baud. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic32.h"
#include "svd-map.h"

struct uart {
    struct sim *s;
    struct rf_periph *p;
    int n;
    struct rf_reg *mode, *sta, *txreg, *rxreg;
    int irq_tx, irq_rx;
    struct uart *next;              /* the part's UARTs, for the input */
};

#define STA_URXDA (1u << 0)
#define STA_RIDLE (1u << 4)
#define STA_TRMT  (1u << 8)
#define STA_UTXBF (1u << 9)
#define STA_UTXEN (1u << 10)
#define STA_URXEN (1u << 12)

static int on(const struct uart *u)
{
    return (u->mode->value >> 15) & 1;
}

static int tx_ready(void *ctx)
{
    struct uart *u = ctx;
    return on(u) && (u->sta->value & STA_UTXEN);
}

static u32 uart_read(void *ctx, u32 off, int n)
{
    struct uart *u = ctx;
    u32 v = rf_read(u->p, off, n);
    if (off == rf_off(u->rxreg) && !u->s->bus.debug)
        rf_hw(u->sta, u->sta->value & ~(u64)STA_URXDA);
    return v;
}

static void uart_write(void *ctx, u32 off, int n, u32 v)
{
    struct uart *u = ctx;
    u64 keep = u->sta->value & (STA_URXDA | STA_RIDLE | STA_TRMT | STA_UTXBF);
    rf_write(u->p, off, n, v);
    if (off == rf_off(u->sta))
        u->sta->value = (u->sta->value & ~(u64)(STA_URXDA | STA_RIDLE |
                                                STA_TRMT | STA_UTXBF)) | keep;
    if (off == rf_off(u->txreg) && tx_ready(u))
        sim_out(u->s, (int)(v & 0xff));
    if (tx_ready(u))
        pic32_irq(u->s, u->irq_tx);
}

/* the input's receiver: the first UART listening (the input's port has
 * the first UART, the others after it) */
static struct uart *listener(void *first)
{
    for (struct uart *u = first; u; u = u->next)
        if (on(u) && (u->sta->value & STA_URXEN))
            return u;
    return 0;
}

static int rx_room(void *ctx)
{
    struct uart *u = listener(ctx);
    return u && !(u->sta->value & STA_URXDA);
}

static void rx_put(void *ctx, int c)
{
    struct uart *u = listener(ctx);
    if (!u)
        return;
    u->rxreg->value = (u64)(c & 0xff);
    rf_hw(u->sta, u->sta->value | STA_URXDA);
    pic32_irq(u->s, u->irq_rx);
}

static void uart_reset(void *ctx)
{
    (void)ctx;
}

const struct dev_ops pic32_uart_ops = {
    "pic32-uart", uart_read, uart_write, uart_reset, 0, 0,
};

void *pic32_uart_create(struct sim *s, struct rf_periph *p)
{
    char nm[16];
    int n = 0;
    if (sscanf(p->name, "UART%d", &n) != 1)
        return 0;
    struct uart *u = calloc(1, sizeof *u);
    if (!u)
        die("out of memory");
    u->s = s;
    u->p = p;
    u->n = n;
    snprintf(nm, sizeof nm, "U%dMODE", n);
    u->mode = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "U%dSTA", n);
    u->sta = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "U%dTXREG", n);
    u->txreg = rf_reg(p, nm);
    snprintf(nm, sizeof nm, "U%dRXREG", n);
    u->rxreg = rf_reg(p, nm);
    if (!u->mode || !u->sta || !u->txreg || !u->rxreg) {
        free(u);
        return 0;
    }
    rf_set_reset(u->sta, STA_TRMT | STA_RIDLE);
    snprintf(nm, sizeof nm, "UART%d_TX", n);
    u->irq_tx = pic32_irq_named(s, nm);
    snprintf(nm, sizeof nm, "UART%d_RX", n);
    u->irq_rx = pic32_irq_named(s, nm);
    pic32_irq_level(s, u->irq_tx, tx_ready, u);
    if (s->rx.room == rx_room) {
        struct uart *t = s->rx.ctx;
        while (t->next)
            t = t->next;
        t->next = u;
    } else {
        sim_rx_port(s, rx_room, rx_put, u);
    }
    return u;
}
