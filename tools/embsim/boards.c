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
#include "svd-map.h"

#define KB(n) ((u32)(n) << 10)
#define MB(n) ((u32)(n) << 20)

/* the peripheral space reads as zero and ignores writes where nothing is
 * modelled, as a part's reserved registers mostly do */
#define PERIPHERALS { "zero", 0x40000000u, 0x20000000u }
#define BITBAND { { 0x22000000u, 0x02000000u, 0x20000000u }, \
                  { 0x42000000u, 0x02000000u, 0x40000000u } }
#define MPS2_MEM { { 0, MB(4), MEM_RAM, 0, 0 }, \
                   { 0x20000000u, MB(4), MEM_RAM, 1, 0 }, \
                   { 0x60000000u, MB(16), MEM_RAM, 0, 0 } }

/* the STM32's models, over ST's SVD (svd-map.h) */
static const struct model_desc stm32_models[] = {
    { "RCC", "stm32-rcc" },         /* first: the others' clock gates */
    { "GPIO*", "stm32-gpio" },
    { "USART*", "stm32-usart" },
    { "UART*", "stm32-usart" },
    { "TIM*", "stm32-tim" },
    { 0, 0 },
};

const struct board_desc boards[] = {
    { "lm3s6965evb", "cortex-m", "cortex-m3", 3,
      { { 0, KB(256), MEM_FLASH, 0, 0 }, { 0x20000000u, KB(64), MEM_RAM, 1, 0 } },
      { { "pl011", 0x4000C000u, 0x1000 }, PERIPHERALS },
      BITBAND, 0, 0 },
    { "mps2-an385", "cortex-m", "cortex-m3", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND, 0, 0 },
    { "mps2-an386", "cortex-m", "cortex-m4", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND, 0, 0 },
    { "mps2-an500", "cortex-m", "cortex-m7", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND, 0, 0 },
    { "microbit", "cortex-m", "cortex-m0", 2,
      { { 0, KB(256), MEM_FLASH, 0, 0 }, { 0x20000000u, KB(16), MEM_RAM, 1, 0 } },
      { { "nrf51-uart", 0x40002000u, 0x1000 }, PERIPHERALS },
      { { 0, 0, 0 } }, 0, 0 },
    /* an STM32F405, as QEMU's netduinoplus2 has it: 1 MiB of flash at
     * 0x08000000, seen at 0 too, where the core finds its vector table;
     * 128 KiB of SRAM and 64 KiB of CCM RAM. The peripherals are ST's
     * SVD's, with models for RCC, GPIO, the USARTs and the timers */
    { "stm32f405", "cortex-m", "cortex-m4", 4,
      { { 0x08000000u, MB(1), MEM_FLASH, 0, 0 },
        { 0, MB(1), MEM_ALIAS, 0, 0x08000000u },
        { 0x20000000u, KB(128), MEM_RAM, 1, 0 },
        { 0x10000000u, KB(64), MEM_RAM, 0, 0 } },
      { { 0, 0, 0 } },
      BITBAND, "STM32F405.svd", stm32_models },
    /* QEMU's virt, RV32 or RV64 by the image: the reset ROM where the
     * core starts, the test device that ends a run, the CLINT, the
     * NS16550A, and the PLIC's space reading as zero */
    { "virt", "riscv", "riscv", 0,
      { { 0x80000000u, MB(128), MEM_RAM, 1, 0 } },
      { { "virt-rom", 0x1000, 0xf000 }, { "sifive-test", 0x100000, 0x1000 },
        { "clint", 0x2000000, 0x10000 }, { "ns16550a", 0x10000000u, 0x100 },
        { "zero", 0x0c000000u, 0x600000 } },
      { { 0, 0, 0 } }, 0, 0 },
    /* QEMU's uno, the ATmega328P: 32 KiB of flash at 0, and the data
     * space at 0x800000 (gdb's numbering): 2 KiB of SRAM at 0x100,
     * USART0, Timer/Counter1, the I/O registers that keep their values,
     * and nothing above RAMEND */
    { "uno", "avr", "atmega328p", 0,
      { { 0, KB(32), MEM_FLASH, 0, 0 }, { 0x800100u, KB(2), MEM_RAM, 1, 0 } },
      { { "avr-usart", 0x8000c0u, 7 }, { "avr-timer16", 0x800080u, 12 },
        { "avr-io", 0x800020u, 0xe0 }, { "zero", 0x800900u, 0xf700 } },
      { { 0, 0, 0 } }, 0, 0 },
};
const int nboards = (int)(sizeof boards / sizeof boards[0]);

const struct core_type cores[] = {
    { "cortex-m", cortexm_create },
    { "riscv", riscv_create },
    { "avr", avr_create },
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
    { "virt-rom", &virt_rom_ops, virt_rom_create },
    { "sifive-test", &sifive_test_ops, sifive_test_create },
    { "clint", &clint_ops, clint_create },
    { "ns16550a", &ns16550a_ops, ns16550a_create },
    { "avr-io", &avr_io_ops, avr_io_create },
    { "avr-usart", &avr_usart_ops, avr_usart_create },
    { "avr-timer16", &avr_timer16_ops, avr_timer16_create },
};

/* ---- the peripheral models, over an SVD's registers (svd-map.h) -------- */

static const struct model_type model_types[] = {
    { "stm32-rcc", &stm32_rcc_ops, stm32_rcc_create },
    { "stm32-gpio", &stm32_gpio_ops, stm32_gpio_create },
    { "stm32-usart", &stm32_usart_ops, stm32_usart_create },
    { "stm32-tim", &stm32_tim_ops, stm32_tim_create },
    { 0, 0, 0 },
};

const struct model_type *model_find(const char *name)
{
    for (size_t i = 0; model_types[i].name; i++)
        if (!strcmp(model_types[i].name, name))
            return &model_types[i];
    return 0;
}

const struct board_desc *board_find(const char *name)
{
    for (int i = 0; i < nboards; i++)
        if (!strcmp(boards[i].name, name))
            return &boards[i];
    return 0;
}

void sim_add_dev(struct sim *s, u32 base, u32 size, const struct dev_ops *ops,
                 void *ctx)
{
    bus_add_device(&s->bus, base, size, ops, ctx);
    if (ops->tick || ops->next_event) {
        if (s->ntick == SIM_TICKERS)
            die("too many devices that keep time");
        s->tick[s->ntick++] = &s->bus.dev[s->bus.ndev - 1];
        if (ops->tick) {
            s->tick_fn[s->ntick_fn] = ops->tick;
            s->tick_ctx[s->ntick_fn++] = ctx;
        }
    }
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
    sim_add_dev(s, d->base, d->size, t->ops, t->create(s, d));
}
