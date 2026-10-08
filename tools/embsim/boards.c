/* boards.c -- the boards, and the cores and devices they are built from.
 *
 * A board is data: its core and the core's model, the NVIC's priority
 * bits, its memory, its devices at their addresses, and its bit-band
 * aliases. Adding a board is adding an entry to boards[]; adding a
 * device type is a file with a struct dev_ops and a line in dev_types[];
 * adding a core, a file with a create function and a line in cores[]
 * (docs/internals/embsim.md).
 *
 * The boards are QEMU's models of them, so an image built for QEMU runs
 * unchanged. The lm3s6965 and the nRF51 have flash at 0, which the
 * core's stores do not change; the MPS2 boards have SSRAM there. The
 * core adds its own devices (the system control space, SysTick, DWT and
 * the rest of the private peripheral bus) itself. */
#include <string.h>

#include "devices.h"

#define KB(n) ((u32)(n) << 10)
#define MB(n) ((u32)(n) << 20)

/* the peripheral space reads as zero and ignores writes where nothing is
 * modelled, as a part's reserved registers mostly do */
#define PERIPHERALS { "zero", 0x40000000u, 0x20000000u }
#define BITBAND { { 0x22000000u, 0x02000000u, 0x20000000u }, \
                  { 0x42000000u, 0x02000000u, 0x40000000u } }
#define MPS2_MEM { { 0, MB(4), MEM_RAM, 0 }, \
                   { 0x20000000u, MB(4), MEM_RAM, 1 }, \
                   { 0x60000000u, MB(16), MEM_RAM, 0 } }

const struct board_desc boards[] = {
    { "lm3s6965evb", "cortex-m", "cortex-m3", 3,
      { { 0, KB(256), MEM_FLASH, 0 }, { 0x20000000u, KB(64), MEM_RAM, 1 } },
      { { "pl011", 0x4000C000u, 0x1000 }, PERIPHERALS },
      BITBAND },
    { "mps2-an385", "cortex-m", "cortex-m3", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND },
    { "mps2-an386", "cortex-m", "cortex-m4", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND },
    { "mps2-an500", "cortex-m", "cortex-m7", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND },
    { "microbit", "cortex-m", "cortex-m0", 2,
      { { 0, KB(256), MEM_FLASH, 0 }, { 0x20000000u, KB(16), MEM_RAM, 1 } },
      { { "nrf51-uart", 0x40002000u, 0x1000 }, PERIPHERALS },
      { { 0, 0, 0 } } },
};
const int nboards = (int)(sizeof boards / sizeof boards[0]);

const struct core_type cores[] = {
    { "cortex-m", cortexm_create },
};
const int ncores = (int)(sizeof cores / sizeof cores[0]);

/* ---- the device types --------------------------------------------------- */

static u32 zero_read(void *ctx, u32 off, int n)
{
    (void)ctx;
    (void)off;
    (void)n;
    return 0;
}

const struct dev_ops zero_ops = { "zero", zero_read, 0, 0, 0, 0 };

static void *no_ctx(struct sim *s, const struct dev_desc *d)
{
    (void)s;
    (void)d;
    return 0;
}

static const struct dev_type dev_types[] = {
    { "pl011", &pl011_ops, pl011_create },
    { "cmsdk-uart", &cmsdk_uart_ops, cmsdk_uart_create },
    { "nrf51-uart", &nrf51_uart_ops, nrf51_uart_create },
    { "systick", &systick_ops, systick_create },
    { "scs", &scs_ops, scs_create },
    { "dwt", &dwt_ops, dwt_create },
    { "zero", &zero_ops, no_ctx },
};

const struct board_desc *board_find(const char *name)
{
    for (int i = 0; i < nboards; i++)
        if (!strcmp(boards[i].name, name))
            return &boards[i];
    return 0;
}

/* a device of the board's, or of the core's: on the bus, and keeping
 * time if it has a clock */
void sim_add_device(struct sim *s, const struct dev_desc *d)
{
    const struct dev_type *t = 0;
    for (size_t i = 0; i < sizeof dev_types / sizeof dev_types[0]; i++)
        if (!strcmp(dev_types[i].type, d->type))
            t = &dev_types[i];
    if (!t)
        die("unknown device type '%s'", d->type);
    void *ctx = t->create(s, d);
    bus_add_device(&s->bus, d->base, d->size, t->ops, ctx);
    if (t->ops->tick || t->ops->next_event) {
        if (s->ntick == SIM_TICKERS)
            die("too many devices that keep time");
        s->tick[s->ntick++] = &s->bus.dev[s->bus.ndev - 1];
        if (t->ops->tick) {
            s->tick_fn[s->ntick_fn] = t->ops->tick;
            s->tick_ctx[s->ntick_fn++] = ctx;
        }
    }
}
