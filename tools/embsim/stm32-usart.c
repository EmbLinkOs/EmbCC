/* stm32-usart.c -- an STM32 USART or UART of the SR/DR layout (F1, F2,
 * F4), over the SVD's registers: enough for a driver that transmits,
 * polled or by interrupt.
 *
 *   - SR resets to 0x000000c0, TXE and TC, as the reference manual says
 *     (RM0090 30.6.1) and QEMU has it; ST's STM32F405.svd 1.2 says
 *     0x00c00000, which would leave a polling putchar waiting forever;
 *   - SR's CTS, LBD, TC and RXNE are rc_w0 (written 0, cleared; 1 leaves
 *     them), which the file calls read-write;
 *   - a write to DR with CR1's UE and TE set sends the byte, to stdout as
 *     the other boards' UARTs do, at once: TXE stays set and TC sets.
 *     Without UE and TE nothing is sent;
 *   - the interrupt (the SVD's, for this USART) is made pending when an
 *     access leaves TXE with TXEIE, TC with TCIE or RXNE with RXNEIE set.
 *
 * Not modelled: reception (RXNE never sets: there is no input), the baud
 * rate's time (a byte takes no time), the error flags, DMA. */
#include <stdlib.h>

#include "devices.h"
#include "svd-map.h"

struct usart {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *sr, *dr, *cr1;
    u64 txe, tc, rxne, ue, te, txeie, tcie, rxneie;
    int irq;
};

static void update_irq(struct usart *u)
{
    u64 sr = u->sr->value, cr1 = u->cr1->value;
    if (((sr & u->txe) && (cr1 & u->txeie)) || ((sr & u->tc) && (cr1 & u->tcie)) ||
        ((sr & u->rxne) && (cr1 & u->rxneie)))
        rf_interrupt(u->p, u->irq);
}

static u32 usart_read(void *ctx, u32 off, int n)
{
    struct usart *u = ctx;
    return rf_read(u->p, off, n);
}

static void usart_write(void *ctx, u32 off, int n, u32 v)
{
    struct usart *u = ctx;
    rf_write(u->p, off, n, v);
    if (off == rf_off(u->dr) && (u->cr1->value & u->ue) &&
        (u->cr1->value & u->te)) {
        sim_out(u->s, (int)(v & 0xff));
        rf_hw(u->sr, u->sr->value | u->txe | u->tc);
    }
    update_irq(u);
}

const struct dev_ops stm32_usart_ops = {
    "stm32-usart", usart_read, usart_write, 0, 0, 0,
};

void *stm32_usart_create(struct sim *s, struct rf_periph *p)
{
    struct usart u;
    u.s = s;
    u.p = p;
    u.sr = rf_reg(p, "SR");
    u.dr = rf_reg(p, "DR");
    u.cr1 = rf_reg(p, "CR1");
    if (!u.sr || !u.dr || !u.cr1)
        return 0;                   /* not this layout (ISR/TDR): registers alone */
    u.txe = rf_mask(u.sr, "TXE");
    u.tc = rf_mask(u.sr, "TC");
    u.rxne = rf_mask(u.sr, "RXNE");
    u.ue = rf_mask(u.cr1, "UE");
    u.te = rf_mask(u.cr1, "TE");
    u.txeie = rf_mask(u.cr1, "TXEIE");
    u.tcie = rf_mask(u.cr1, "TCIE");
    u.rxneie = rf_mask(u.cr1, "RXNEIE");
    if (!u.txe || !u.tc || !u.ue || !u.te)
        return 0;
    u.irq = rf_irq(p, 0);
    rf_set_reset(u.sr, u.txe | u.tc);
    rf_set_mwv(u.sr, rf_mask(u.sr, "CTS") | rf_mask(u.sr, "LBD") | u.tc | u.rxne,
               MWV_0CLR);
    struct usart *c = malloc(sizeof *c);
    if (!c)
        die("out of memory");
    *c = u;
    return c;
}
