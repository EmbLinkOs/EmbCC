/* avr-io.c -- the ATmega328P's I/O and extended I/O registers that no
 * peripheral of EmbSim's models (the ports, the external interrupts'
 * and the ADC's, EEPROM's, the watchdog's...): each keeps what is
 * written, as a register does, and starts at zero. The modelled
 * peripherals (USART0, Timer/Counter1) are on the bus before it and
 * answer for their own; the core answers for SP and SREG. SMCR (0x53)
 * is here, and the core reads its SE bit for SLEEP. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"

struct avr_io {
    u8 reg[0xe0];
};

static u32 io_read(void *ctx, u32 off, int n)
{
    struct avr_io *io = ctx;
    u32 v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | (off + (u32)i < sizeof io->reg ? io->reg[off + (u32)i] : 0);
    return v;
}

static void io_write(void *ctx, u32 off, int n, u32 v)
{
    struct avr_io *io = ctx;
    for (int i = 0; i < n; i++)
        if (off + (u32)i < sizeof io->reg)
            io->reg[off + (u32)i] = (u8)(v >> (8 * i));
}

static void io_reset(void *ctx)
{
    struct avr_io *io = ctx;
    memset(io->reg, 0, sizeof io->reg);
}

const struct dev_ops avr_io_ops = {
    "avr-io", io_read, io_write, io_reset, 0, 0,
};

void *avr_io_create(struct sim *s, const struct dev_desc *d)
{
    struct avr_io *io = calloc(1, sizeof *io);
    (void)s;
    (void)d;
    if (!io)
        die("out of memory");
    return io;
}
