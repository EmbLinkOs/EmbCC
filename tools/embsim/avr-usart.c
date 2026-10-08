/* avr-usart.c -- the ATmega328P's USART0, at data 0xC0: a byte written
 * to UDR0 with the transmitter on (UCSR0B's TXEN0) is sent at once, so
 * the data register is always empty (UCSR0A's UDRE0) and the transmit
 * completes then (TXC0, cleared by writing it a one or by entering its
 * vector). The receiver never has anything. Its interrupts are the
 * data register empty (vector 19) and transmit complete (20), as
 * UCSR0B enables them. */
#include <stdlib.h>

#include "avr.h"
#include "devices.h"

enum { UCSRA, UCSRB, UCSRC, RES, UBRRL, UBRRH, UDR };
#define UDRE 0x20
#define TXC 0x40

struct usart {
    struct sim *sim;
    u8 a, b, c, brrl, brrh;
};

static u8 rd(struct usart *u, u32 off)
{
    switch (off) {
    case UCSRA: return u->a;
    case UCSRB: return u->b;
    case UCSRC: return u->c;
    case UBRRL: return u->brrl;
    case UBRRH: return u->brrh;
    }
    return 0;                                   /* UDR: nothing received */
}

static void wr(struct usart *u, u32 off, u8 v)
{
    switch (off) {
    case UCSRA:
        /* TXC is cleared by a one; U2X and MPCM are written */
        u->a = (u8)((u->a & ~3u) | (v & 3));
        if (v & TXC)
            u->a &= (u8)~TXC;
        break;
    case UCSRB: u->b = v; break;
    case UCSRC: u->c = v; break;
    case UBRRL: u->brrl = v; break;
    case UBRRH: u->brrh = v & 0x0f; break;
    case UDR:
        if (u->b & 0x08) {                      /* TXEN0 */
            sim_out(u->sim, v);
            u->a |= TXC | UDRE;
        }
        break;
    }
    avr_irq_changed(u->sim);
}

static u32 usart_read(void *ctx, u32 off, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u32)rd(ctx, off + (u32)i) << (8 * i);
    return v;
}

static void usart_write(void *ctx, u32 off, int n, u32 v)
{
    for (int i = 0; i < n; i++)
        wr(ctx, off + (u32)i, (u8)(v >> (8 * i)));
}

static void usart_reset(void *ctx)
{
    struct usart *u = ctx;
    u->a = UDRE;
    u->b = 0;
    u->c = 0x06;
    u->brrl = u->brrh = 0;
}

/* data register empty (19): UDRIE0 and UDRE0; transmit complete (20):
 * TXCIE0 and TXC0, which entering the vector clears */
static int usart_pending(void *ctx, int vec)
{
    struct usart *u = ctx;
    if (vec == 19)
        return (u->b & 0x20) && (u->a & UDRE);
    return (u->b & 0x40) && (u->a & TXC);
}

static void usart_ack(void *ctx, int vec)
{
    struct usart *u = ctx;
    if (vec == 20)
        u->a &= (u8)~TXC;
}

const struct dev_ops avr_usart_ops = {
    "avr-usart", usart_read, usart_write, usart_reset, 0, 0,
};

void *avr_usart_create(struct sim *s, const struct dev_desc *d)
{
    struct usart *u = calloc(1, sizeof *u);
    (void)d;
    if (!u)
        die("out of memory");
    u->sim = s;
    usart_reset(u);
    avr_irq_source(s, 19, usart_pending, 0, u);
    avr_irq_source(s, 20, usart_pending, usart_ack, u);
    return u;
}
