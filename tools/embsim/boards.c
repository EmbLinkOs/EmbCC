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
#include <ctype.h>
#include <string.h>

#include "../embsvd/svd.h"
#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define KB(n) ((u32)(n) << 10)
#define MB(n) ((u32)(n) << 20)

/* the peripheral space reads as zero and ignores writes where nothing is
 * modelled, as a part's reserved registers mostly do */
#define PERIPHERALS { "zero", 0x40000000u, 0x20000000u }
#define BITBAND { { 0x22000000u, 0x02000000u, 0x20000000u }, \
                  { 0x42000000u, 0x02000000u, 0x40000000u } }
#define MPS2_MEM { { 0, MB(4), MEM_RAM, 0, 0, 0 }, \
                   { 0x20000000u, MB(4), MEM_RAM, 1, 0, 0 }, \
                   { 0x60000000u, MB(16), MEM_RAM, 0, 0, 0 } }

/* the STM32's models, over ST's SVD (svd-map.h) */
static const struct model_desc stm32_models[] = {
    { "RCC", "stm32-rcc" },         /* first: the others' clock gates */
    { "GPIO*", "stm32-gpio" },
    { "USART*", "stm32-usart" },
    { "UART*", "stm32-usart" },
    { "TIM*", "stm32-tim" },
    { 0, 0 },
};

/* a PIC32MZ's models, over its ATDF (pic32.h); the EVIC first, which
 * the others raise their interrupts through, and the oscillator's, whose
 * bus dividers the timers read */
static const struct model_desc pic32mz_models[] = {
    { "INT", "pic32-evic" },
    { "CFG", "pic32-cfg" },
    { "CRU", "pic32-cru" },
    { "TMR*", "pic32-tmr" },
    { "GPIO", "pic32-gpio" },
    { "UART*", "pic32-uart" },
    { "PMP", "pic32-pmp" },
    { "ADCHS", "pic32-adc" },
    { 0, 0 },
};

/* a PIC24FJ's models, over its EDC (pic24.h): the interrupt controller
 * first, which the others raise their interrupts through */
static const struct model_desc pic24f_models[] = {
    { "INT", "pic24-intc" },
    { "OSC", "pic24-osc" },
    { "PPS", "pic24-pps" },
    { "TMR", "pic24-tmr" },
    { "UART", "pic24-uart" },
    { "GPIO", "pic32-gpio" },
    { "PMP", "pic24-epmp" },
    { "ADC1", "pic24-adc" },
    { "SPI", "pic24-spi" },
    { "NVM", "pic24-nvm" },
    { "RTCC", "pic24-rtcc" },
    { "DMA", "pic24-dma" },
    { "I2C", "pic24-i2c" },
    { "IC", "pic24-ic" },
    { "OC", "pic24-oc" },
    { "CCP", "pic24-ccp" },
    { 0, 0 },
};

/* the PIC24FJ1024GB610's memory, on the bus as pic24.h places it:
 * program flash to program address 0xABFFF (the configuration words in
 * its last page) at twice its address, the configuration memory at
 * 0x800000 (FBOOT), and 32 KiB of RAM at data address 0x0800 to 0x87FF
 * (its top 2 KiB EDS page 1's) */
#define PIC24FJ1024_MEM { { 0, 2 * 0xac000u, MEM_FLASH, 0, 0, 0x00ffffffu }, \
                          { 2 * 0x800000u, 0x4000, MEM_FLASH, 0, 0, 0x00ffffffu }, \
                          { 0x2000800u, KB(32), MEM_RAM, 1, 0, 0 } }

/* the PIC32MZ2048EFH100's memory: 512 KiB of RAM at 0, 2 MiB of program
 * flash at 0x1D000000 and the boot flash at 0x1FC00000 (the physical
 * addresses kseg0 and kseg1 reach) */
#define PIC32MZ_MEM { { 0, KB(512), MEM_RAM, 1, 0, 0 }, \
                      { 0x1d000000u, MB(2), MEM_FLASH, 0, 0, 0xffffffffu }, \
                      { 0x1fc00000u, 0x74000, MEM_FLASH, 0, 0, 0xffffffffu } }

const struct board_desc boards[] = {
    { "lm3s6965evb", "cortex-m", "cortex-m3", 3,
      { { 0, KB(256), MEM_FLASH, 0, 0, 0 }, { 0x20000000u, KB(64), MEM_RAM, 1, 0, 0 } },
      { { "pl011", 0x4000C000u, 0x1000 }, PERIPHERALS },
      BITBAND, 0, 0 },
    { "mps2-an385", "cortex-m", "cortex-m3", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND, 0, 0 },
    { "mps2-an386", "cortex-m", "cortex-m4", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND, 0, 0 },
    { "mps2-an500", "cortex-m", "cortex-m7", 3, MPS2_MEM,
      { { "cmsdk-uart", 0x40004000u, 0x1000 }, PERIPHERALS }, BITBAND, 0, 0 },
    { "microbit", "cortex-m", "cortex-m0", 2,
      { { 0, KB(256), MEM_FLASH, 0, 0, 0 }, { 0x20000000u, KB(16), MEM_RAM, 1, 0, 0 } },
      { { "nrf51-uart", 0x40002000u, 0x1000 }, PERIPHERALS },
      { { 0, 0, 0 } }, 0, 0 },
    /* an STM32F405, as QEMU's netduinoplus2 has it: 1 MiB of flash at
     * 0x08000000, seen at 0 too, where the core finds its vector table;
     * 128 KiB of SRAM and 64 KiB of CCM RAM. The peripherals are ST's
     * SVD's, with models for RCC, GPIO, the USARTs and the timers */
    { "stm32f405", "cortex-m", "cortex-m4", 4,
      { { 0x08000000u, MB(1), MEM_FLASH, 0, 0, 0 },
        { 0, MB(1), MEM_ALIAS, 0, 0x08000000u, 0 },
        { 0x20000000u, KB(128), MEM_RAM, 1, 0, 0 },
        { 0x10000000u, KB(64), MEM_RAM, 0, 0, 0 } },
      { { 0, 0, 0 } },
      BITBAND, "STM32F405.svd", stm32_models },
    /* QEMU's virt, RV32 or RV64 by the image: the reset ROM where the
     * core starts, the test device that ends a run, the CLINT, the
     * NS16550A, and the PLIC's space reading as zero */
    { "virt", "riscv", "riscv", 0,
      { { 0x80000000u, MB(128), MEM_RAM, 1, 0, 0 } },
      { { "virt-rom", 0x1000, 0xf000 }, { "sifive-test", 0x100000, 0x1000 },
        { "clint", 0x2000000, 0x10000 }, { "ns16550a", 0x10000000u, 0x100 },
        { "zero", 0x0c000000u, 0x600000 } },
      { { 0, 0, 0 } }, 0, 0 },
    /* QEMU's uno, the ATmega328P: 32 KiB of flash at 0, and the data
     * space at 0x800000 (gdb's numbering): 2 KiB of SRAM at 0x100,
     * USART0, Timer/Counter1, the I/O registers that keep their values,
     * and nothing above RAMEND */
    { "uno", "avr", "atmega328p", 0,
      { { 0, KB(32), MEM_FLASH, 0, 0, 0 }, { 0x800100u, KB(2), MEM_RAM, 1, 0, 0 } },
      { { "avr-usart", 0x8000c0u, 7 }, { "avr-timer16", 0x800080u, 12 },
        { "avr-io", 0x800020u, 0xe0 }, { "zero", 0x800900u, 0xf700 } },
      { { 0, 0, 0 } }, 0, 0 },
    /* QEMU's malta with a 24Kc, as tests/harness/mips runs it: 64 MiB of
     * RAM at 0 (0x80000000 in kseg0), the FPGA's UART with its registers
     * eight bytes apart and its SOFTRES, and the boot ROM where the core
     * starts, jumping to the image as QEMU's -kernel does */
    { "malta", "mips", "24kc", 0,
      { { 0, MB(64), MEM_RAM, 1, 0, 0 } },
      { { "ns16550a-8", 0x1f000900u, 0x40 },
        { "malta-fpga", 0x1f000000u, 0x100000 },
        { "malta-rom", 0x1fc00000u, 0x400000 } },
      { { 0, 0, 0 } }, 0, 0 },
    /* the PIC32MZ2048EFH100 alone: the core in EIC mode, the memory, and
     * every register the device pack's ATDF describes, with the models
     * of its EVIC, oscillator, timers, ports, UARTs, PMP and ADC */
    { "pic32mz2048efh100", "mips", "pic32mz", 0, PIC32MZ_MEM,
      { { 0, 0, 0 } }, { { 0, 0, 0 } },
      "PIC32MZ2048EFH100.atdf", pic32mz_models },
    /* Microchip's Explorer 16/32 with the PIC32MZ EF PIM: the part, and
     * the board's LEDs, buttons, potentiometer, temperature sensor and
     * LCD (pic-boards.c) */
    { "explorer16-32", "mips", "pic32mz", 0, PIC32MZ_MEM,
      { { "explorer16-32", 0, 0 } }, { { 0, 0, 0 } },
      "PIC32MZ2048EFH100.atdf", pic32mz_models },
    /* Microchip's Curiosity PIC32MZ EF 2.0, a PIC32MZ2048EFM144: its LEDs
     * and switches (pic-boards.c) */
    { "curiosity-pic32mz-ef-2", "mips", "pic32mz", 0, PIC32MZ_MEM,
      { { "curiosity-pic32mz-ef-2", 0, 0 } }, { { 0, 0, 0 } },
      "PIC32MZ2048EFM144.atdf", pic32mz_models },
    /* the PIC24FJ1024GB610 alone: the core, the memory, and every register
     * the device pack's EDC describes, with the models of its interrupt
     * controller, oscillator, PPS lock, timers, UARTs, ports, EPMP, ADC
     * and SPI */
    { "pic24fj1024gb610", "pic24", "pic24fj1024gb610", 0, PIC24FJ1024_MEM,
      { { 0, 0, 0 } }, { { 0, 0, 0 } },
      "PIC24FJ1024GB610.PIC", pic24f_models },
    /* the Explorer 16/32 with the PIC24FJ1024GB610 PIM: its LEDs, buttons,
     * potentiometer, temperature sensor, LCD and serial EEPROM
     * (pic-boards.c) */
    { "explorer16-32-pic24", "pic24", "pic24fj1024gb610", 0, PIC24FJ1024_MEM,
      { { "explorer16-32-pic24", 0, 0 } }, { { 0, 0, 0 } },
      "PIC24FJ1024GB610.PIC", pic24f_models },
};
const int nboards = (int)(sizeof boards / sizeof boards[0]);

const struct core_type cores[] = {
    { "cortex-m", cortexm_create },
    { "riscv", riscv_create },
    { "avr", avr_create },
    { "mips", mips_create },
    { "pic24", pic24_create },
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
    { "ns16550a-8", &ns16550a_8_ops, ns16550a_create },
    { "malta-fpga", &malta_fpga_ops, malta_fpga_create },
    { "malta-rom", &malta_rom_ops, malta_rom_create },
    { "explorer16-32", &pic_board_ops, pic_board_create },
    { "curiosity-pic32mz-ef-2", &pic_board_ops, pic_board_create },
    { "explorer16-32-pic24", &pic_board_ops, pic_board_create },
};

/* ---- the peripheral models, over an SVD's registers (svd-map.h) -------- */

static const struct model_type model_types[] = {
    { "stm32-rcc", &stm32_rcc_ops, stm32_rcc_create },
    { "stm32-gpio", &stm32_gpio_ops, stm32_gpio_create },
    { "stm32-usart", &stm32_usart_ops, stm32_usart_create },
    { "stm32-tim", &stm32_tim_ops, stm32_tim_create },
    { "pic32-evic", &pic32_evic_ops, pic32_evic_create },
    { "pic32-cfg", &pic32_cfg_ops, pic32_cfg_create },
    { "pic32-cru", &pic32_cru_ops, pic32_cru_create },
    { "pic32-tmr", &pic32_tmr_ops, pic32_tmr_create },
    { "pic32-gpio", &pic32_gpio_ops, pic32_gpio_create },
    { "pic32-uart", &pic32_uart_ops, pic32_uart_create },
    { "pic32-pmp", &pic32_pmp_ops, pic32_pmp_create },
    { "pic32-adc", &pic32_adc_ops, pic32_adc_create },
    { "pic24-intc", &pic24_intc_ops, pic24_intc_create },
    { "pic24-adc", &pic24_adc_ops, pic24_adc_create },
    { "pic24-epmp", &pic24_epmp_ops, pic24_epmp_create },
    { "pic24-spi", &pic24_spi_ops, pic24_spi_create },
    { "pic24-uart", &pic24_uart_ops, pic24_uart_create },
    { "pic24-tmr", &pic24_tmr_ops, pic24_tmr_create },
    { "pic24-nvm", &pic24_nvm_ops, pic24_nvm_create },
    { "pic24-rtcc", &pic24_rtcc_ops, pic24_rtcc_create },
    { "pic24-dma", &pic24_dma_ops, pic24_dma_create },
    { "pic24-i2c", &pic24_i2c_ops, pic24_i2c_create },
    { "pic24-ic", &pic24_ic_ops, pic24_ic_create },
    { "pic24-oc", &pic24_oc_ops, pic24_oc_create },
    { "pic24-ccp", &pic24_ccp_ops, pic24_ccp_create },
    { "pic24-osc", &pic24_osc_ops, pic24_osc_create },
    { "pic24-pps", &pic24_pps_ops, pic24_pps_create },
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

/* NAME is digits from `at` for `n` characters at least */
static int digits(const char *s, int at, int n)
{
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[at + i]))
            return 0;
    return 1;
}

/* a part of a family EmbSim models, alone, by its name: every PIC32MZ EF
 * (pic32mz1024efg064) and every PIC24FJ of the GA6/GB6 family
 * (pic24fj256ga610), its memory as its pack's ATDF or EDC gives it */
const struct board_desc *board_part(const char *name)
{
    static struct board_desc b;
    static char nm[40], file[48];
    size_t l = strlen(name);
    /* pic32mz NNNN ef X NNN; pic24fj NNN(N) g A|B 6 NN */
    int pic32 = l == 17 && !strncmp(name, "pic32mz", 7) && digits(name, 7, 4) &&
                !strncmp(name + 11, "ef", 2) && name[13] && strchr("efghkm", name[13]) &&
                digits(name, 14, 3);
    int pic24 = !strncmp(name, "pic24fj", 7) && l >= 15 && l <= 16 &&
                digits(name, 7, (int)l - 12) && name[l - 5] == 'g' &&
                (name[l - 4] == 'a' || name[l - 4] == 'b') && name[l - 3] == '6' &&
                digits(name, (int)l - 2, 2);
    if (!pic32 && !pic24) {
        if (!strncmp(name, "pic32", 5) || !strncmp(name, "pic24", 5) ||
            !strncmp(name, "dspic", 5))
            die("--board %s: of Microchip's parts, EmbSim has the PIC32MZ EF "
                "family's and the PIC24FJ GA6/GB6 family's by name "
                "(pic32mz1024efg064, pic24fj256ga610)", name);
        return 0;
    }
    snprintf(nm, sizeof nm, "%s", name);
    for (size_t i = 0; i < l; i++)
        file[i] = (char)toupper((unsigned char)name[i]);
    snprintf(file + l, sizeof file - l, pic32 ? ".atdf" : ".PIC");
    static char path[1024];
    const char *found = svd_search(file);
    if (!found)
        die("--board %s needs %s, from its device pack (Microchip.%s_DFP): "
            "give the pack's directory with --dfp DIR", name, file,
            pic32 ? "PIC32MZ-EF" : "PIC24F-GA-GB");
    snprintf(path, sizeof path, "%s", found);
    svd_prog = "embsim";
    svd_status = 2;
    svd_quiet = 1;
    struct svd_device *d = pic32 ? atdf_load(path) : edc_load(path);
    memset(&b, 0, sizeof b);
    b.name = nm;
    b.svd = file;
    if (pic32) {
        b.core = "mips";
        b.cpu = "pic32mz";
        b.models = pic32mz_models;
        b.mem[0] = (struct mem_desc){ 0, (u32)d->ram_end, MEM_RAM, 1, 0, 0 };
        b.mem[1] = (struct mem_desc){ 0x1d000000u, (u32)d->code_end, MEM_FLASH, 0, 0, 0xffffffffu };
        b.mem[2] = (struct mem_desc){ 0x1fc00000u, 0x74000, MEM_FLASH, 0, 0, 0xffffffffu };
    } else {
        b.core = "pic24";
        b.cpu = nm;
        b.models = pic24f_models;
        b.mem[0] = (struct mem_desc){ 0, 2 * (u32)d->code_end, MEM_FLASH, 0, 0, 0x00ffffffu };
        b.mem[1] = (struct mem_desc){ 2 * 0x800000u, 0x4000, MEM_FLASH, 0, 0, 0x00ffffffu };
        b.mem[2] = (struct mem_desc){ PIC24_DATA + (u32)d->ram_base,
                                      (u32)(d->ram_end - d->ram_base), MEM_RAM, 1, 0, 0 };
    }
    if (!b.mem[0].size || !b.mem[pic32 ? 1 : 2].size)
        die("--board %s: %s does not give the part's memory", name, path);
    return &b;
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
