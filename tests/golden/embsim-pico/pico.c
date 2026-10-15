/* A Raspberry Pi Pico, as its RP2040's datasheet has it: built from the
 * header, startup (with its boot2) and linker script embsvd writes from
 * RP2040.svd, and run on EmbSim's pico board.
 *
 * Each line is "NAME VALUE"; pico.txt holds what the datasheet (and the
 * Pico SDK's code, where the datasheet quotes it) says each must be. The
 * boot state is taken first, before the program changes anything, and
 * printed once UART0 runs. Output is UART0's, polled, as the SDK's
 * stdio_uart does; the LED's changes are the board's lines on stderr. */
#include "RP2040.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define ALIAS_XOR(r) (*(volatile uint32_t *)((uintptr_t)&(r) + 0x1000u))
#define ALIAS_SET(r) (*(volatile uint32_t *)((uintptr_t)&(r) + 0x2000u))
#define ALIAS_CLR(r) (*(volatile uint32_t *)((uintptr_t)&(r) + 0x3000u))
#define VTOR REG(0xE000ED08u)
#define CPUID_M0 REG(0xE000ED00u)
#define NVIC_ISER REG(0xE000E100u)
#define NVIC_ICPR REG(0xE000E280u)

/* RESETS.RESET's bits */
#define R_IO_BANK0 (1u << 5)
#define R_IO_QSPI (1u << 6)
#define R_PADS_BANK0 (1u << 8)
#define R_PADS_QSPI (1u << 9)
#define R_PLL_SYS (1u << 12)
#define R_TIMER (1u << 21)
#define R_UART0 (1u << 22)

static void putc_(char c)
{
    while (UART0->UARTFR & (1u << 5))           /* TXFF */
        ;
    UART0->UARTDR = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void line(const char *name, uint32_t v)
{
    puts_(name);
    putc_(' ');
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 0xf]);
    putc_('\n');
}

static void unreset(uint32_t bits)
{
    ALIAS_CLR(RESETS->RESET) = bits;
    while ((RESETS->RESET_DONE & bits) != bits)
        ;
}

static volatile uint32_t alarm_at, alarm_irqs, gpio_irqs, gpio_intr, sio_irqs;

void TIMER_IRQ_0_IRQHandler(void)
{
    alarm_at = TIMER->TIMERAWL;
    TIMER->INTR = 1;                            /* WC */
    alarm_irqs++;
}

void IO_IRQ_BANK0_IRQHandler(void)
{
    gpio_intr = IO_BANK0->INTR0;
    IO_BANK0->INTR0 = 0xffffffffu;              /* the edges, WC */
    gpio_irqs++;
}

void SIO_IRQ_PROC0_IRQHandler(void)
{
    SIO->FIFO_ST = 0xff;                        /* WOF, ROE */
    sio_irqs++;
}

/* microseconds, by the timer */
static void delay_us(uint32_t us)
{
    uint32_t t0 = TIMER->TIMERAWL;
    while (TIMER->TIMERAWL - t0 < us)
        ;
}

/* a command to the flash through the SSI, run from SRAM (the XIP is off
 * while it runs), with the chip select held low by IO_QSPI as the SDK's
 * flash_do_cmd does: n bytes out, n back */
__attribute__((section(".data"), noinline))
static void flash_cmd(const uint8_t *tx, uint8_t *rx, int n)
{
    volatile uint32_t *ss = &IO_QSPI->GPIO_QSPI_SS_CTRL;
    uint32_t c0 = SSI->CTRLR0, spi = SSI->SPI_CTRLR0;
    SSI->SSIENR = 0;
    SSI->CTRLR0 = 7u << 16;                     /* 8-bit frames, TX and RX */
    SSI->SSIENR = 1;
    *ss = (*ss & ~0x300u) | (2u << 8);          /* OUTOVER: low */
    for (int i = 0; i < n; i++) {
        SSI->DR0 = tx[i];
        while (!(SSI->SR & 8))                  /* RFNE */
            ;
        rx[i] = (uint8_t)SSI->DR0;
    }
    *ss = (*ss & ~0x300u) | (3u << 8);          /* OUTOVER: high */
    *ss &= ~0x300u;
    SSI->SSIENR = 0;
    SSI->CTRLR0 = c0;
    SSI->SPI_CTRLR0 = spi;
    SSI->SSIENR = 1;
}

int main(void)
{
    /* ---- the boot, as the bootrom and boot2 leave it ------------------ */
    uint32_t vtor = VTOR, rom_magic = REG(0x10), rom_vec = REG(0x4);
    uint32_t chip_id = SYSINFO->CHIP_ID, platform = SYSINFO->PLATFORM;
    uint32_t reset0 = RESETS->RESET, done0 = RESETS->RESET_DONE;
    uint32_t ssi_ctrlr0 = SSI->CTRLR0, ssi_spi = SSI->SPI_CTRLR0;
    uint32_t ssi_baudr = SSI->BAUDR, ssi_en = SSI->SSIENR;
    uint32_t qspi_ss = IO_QSPI->GPIO_QSPI_SS_CTRL, pad26 = PADS_BANK0->GPIO26;
    uint32_t pad0 = PADS_BANK0->GPIO0;
    uint32_t sio_cpuid = SIO->CPUID, m0_cpuid = CPUID_M0;
    uint32_t xosc_ctrl = XOSC->CTRL, clk_sys_sel = CLOCKS->CLK_SYS_SELECTED;
    uint32_t clk_ref_sel = CLOCKS->CLK_REF_SELECTED;
    uint32_t pll_cs = PLL_SYS->CS, pll_pwr = PLL_SYS->PWR, tick = WATCHDOG->TICK;
    uint32_t psm_done = PSM->DONE;
    /* held in reset: UART0 reads its reset values and ignores writes */
    uint32_t held_cr = UART0->UARTCR;
    UART0->UARTIBRD = 99;
    uint32_t held_ibrd = UART0->UARTIBRD;

    unreset(R_IO_BANK0 | R_PLL_SYS | R_TIMER | R_UART0);
    uint32_t done1 = RESETS->RESET_DONE;
    /* a byte written before clk_peri runs waits in the FIFO */
    UART0->UARTLCR_H = (3u << 5) | (1u << 4);   /* 8 bits, FIFOs */
    UART0->UARTCR = (1u << 0) | (1u << 8);      /* UARTEN, TXE */
    UART0->UARTDR = '<';
    uint32_t fr_noclk = UART0->UARTFR;

    /* ---- the crystal's startup delay, timed on the ring oscillator ---- */
    /* the tick from clk_ref, the ROSC at 6.5 MHz: / 6 */
    WATCHDOG->TICK = (1u << 9) | 6;
    XOSC->STARTUP = 47;                         /* (12 MHz / 1000 + 128) / 256 */
    uint32_t t0 = TIMER->TIMERAWL;
    XOSC->CTRL = (0xfabu << 12) | 0xaa0;        /* ENABLE, 1-15 MHz */
    uint32_t stable0 = XOSC->STATUS;
    while (!(XOSC->STATUS & (1u << 31)))
        ;
    uint32_t xosc_ticks = TIMER->TIMERAWL - t0;

    /* ---- the clocks, as the SDK's clocks_init sets them -------------- */
    XOSC->CTRL = 0xaa0;                         /* 1-15 MHz */
    uint32_t xosc_bad = XOSC->STATUS;           /* ENABLE 0: invalid, enabled */
    XOSC->STARTUP = 47;
    ALIAS_SET(XOSC->CTRL) = 0xfabu << 12;
    while (!(XOSC->STATUS & (1u << 31)))
        ;
    uint32_t xosc_status = XOSC->STATUS;
    XOSC->STATUS = 1u << 24;                    /* BADWRITE, WC */
    uint32_t xosc_status2 = XOSC->STATUS;
    XOSC->COUNT = 120;                          /* 10 us of the crystal */
    uint32_t count_mid = XOSC->COUNT;
    while (XOSC->COUNT)
        ;
    CLOCKS->CLK_SYS_RESUS_CTRL = 0;
    /* clk_sys and clk_ref to their glitchless defaults first */
    ALIAS_CLR(CLOCKS->CLK_SYS_CTRL) = 1;
    while (CLOCKS->CLK_SYS_SELECTED != 1)
        ;
    ALIAS_CLR(CLOCKS->CLK_REF_CTRL) = 3;
    while (CLOCKS->CLK_REF_SELECTED != 1)
        ;
    /* PLL_SYS: 12 MHz / 1 x 125 = 1500 MHz, / 6 / 2 = 125 MHz */
    PLL_SYS->CS = 1;
    PLL_SYS->FBDIV_INT = 125;
    ALIAS_CLR(PLL_SYS->PWR) = (1u << 0) | (1u << 5);   /* PD, VCOPD */
    while (!(PLL_SYS->CS & (1u << 31)))
        ;
    PLL_SYS->PRIM = (6u << 16) | (2u << 12);
    ALIAS_CLR(PLL_SYS->PWR) = 1u << 3;          /* POSTDIVPD */
    /* clk_ref from the crystal */
    CLOCKS->CLK_REF_DIV = 1u << 8;
    CLOCKS->CLK_REF_CTRL = 2;
    while (!(CLOCKS->CLK_REF_SELECTED & (1u << 2)))
        ;
    /* clk_sys from PLL_SYS, through its aux mux */
    CLOCKS->CLK_SYS_DIV = 1u << 8;
    ALIAS_CLR(CLOCKS->CLK_SYS_CTRL) = 1;
    while (!(CLOCKS->CLK_SYS_SELECTED & 1))
        ;
    ALIAS_XOR(CLOCKS->CLK_SYS_CTRL) = (CLOCKS->CLK_SYS_CTRL ^ (0u << 5)) & (7u << 5);
    ALIAS_SET(CLOCKS->CLK_SYS_CTRL) = 1;
    while (!(CLOCKS->CLK_SYS_SELECTED & (1u << 1)))
        ;
    /* clk_peri from clk_sys */
    CLOCKS->CLK_PERI_CTRL = (1u << 11) | (0u << 5);
    /* the microsecond tick: clk_ref / 12 */
    WATCHDOG->TICK = (1u << 9) | 12;

    /* ---- UART0 at 115200, as the SDK's uart_init ---------------------- */
    uint32_t div = 8u * 125000000u / 115200u + 1;
    UART0->UARTIBRD = div >> 7;
    UART0->UARTFBRD = (div & 0x7f) >> 1;
    UART0->UARTLCR_H = (3u << 5) | (1u << 4);   /* 8 bits, FIFOs */
    UART0->UARTCR = (1u << 0) | (1u << 8) | (1u << 9);
    IO_BANK0->GPIO0_CTRL = 2;                   /* UART0 TX */
    IO_BANK0->GPIO1_CTRL = 2;                   /* UART0 RX */
    puts_("pico\n");
    /* EmbSim's ROM, word by word (the test holds it to its source) */
    for (uint32_t a = 0; a < 0x34; a += 4) {
        char nm[8] = "rom.";
        nm[4] = "0123456789abcdef"[a >> 4];
        nm[5] = "0123456789abcdef"[a & 15];
        nm[6] = 0;
        line(nm, REG(a));
    }

    line("boot.VTOR", vtor);
    line("boot.rom-magic", rom_magic);
    line("boot.rom-reset", rom_vec);
    line("boot.CHIP_ID", chip_id);
    line("boot.PLATFORM", platform);
    line("boot.RESET", reset0);
    line("boot.RESET_DONE", done0);
    line("boot.SSI.CTRLR0", ssi_ctrlr0);
    line("boot.SSI.SPI_CTRLR0", ssi_spi);
    line("boot.SSI.BAUDR", ssi_baudr);
    line("boot.SSI.SSIENR", ssi_en);
    line("boot.QSPI_SS_CTRL", qspi_ss);
    line("boot.PAD_GPIO26", pad26);
    line("boot.PAD_GPIO0", pad0);
    line("boot.SIO.CPUID", sio_cpuid);
    line("boot.M0PLUS.CPUID", m0_cpuid);
    line("boot.XOSC.CTRL", xosc_ctrl);
    line("boot.CLK_SYS_SELECTED", clk_sys_sel);
    line("boot.CLK_REF_SELECTED", clk_ref_sel);
    line("boot.PLL_SYS.CS", pll_cs);
    line("boot.PLL_SYS.PWR", pll_pwr);
    line("boot.TICK", tick);
    line("boot.PSM.DONE", psm_done);
    line("held.UARTCR", held_cr);
    line("held.UARTIBRD", held_ibrd);
    line("uart.FR-no-clk_peri", fr_noclk);
    line("resets.RESET_DONE", done1);
    line("xosc.STATUS-starting", stable0);
    /* 47 x 256 crystal cycles, 1.0027 ms: 1086 ticks of 6.5 MHz / 6 */
    line("xosc.startup-1ms", xosc_ticks >= 1084 && xosc_ticks <= 1089);
    line("xosc.STATUS-badwrite", xosc_bad);
    line("xosc.STATUS", xosc_status);
    line("xosc.STATUS-cleared", xosc_status2);
    line("xosc.COUNT-counting", count_mid > 0 && count_mid < 120);
    line("pll.CS", PLL_SYS->CS);
    line("clocks.CLK_REF_SELECTED", CLOCKS->CLK_REF_SELECTED);
    line("clocks.CLK_SYS_SELECTED", CLOCKS->CLK_SYS_SELECTED);
    line("clocks.CLK_PERI_SELECTED", CLOCKS->CLK_PERI_SELECTED);
    line("tick.TICK", WATCHDOG->TICK);
    line("uart.IBRD", UART0->UARTIBRD);
    line("uart.FBRD", UART0->UARTFBRD);
    line("uart.FR", UART0->UARTFR);
    line("uart.PERIPHID0", UART0->UARTPERIPHID0);
    line("uart.PERIPHID1", UART0->UARTPERIPHID1);
    line("uart.PERIPHID2", UART0->UARTPERIPHID2);
    line("uart.PCELLID0", UART0->UARTPCELLID0);
    line("uart.PCELLID3", UART0->UARTPCELLID3);

    /* ---- the frequency counter: each clock in kHz -------------------- */
    static const struct { const char *name; uint32_t src; } fc[] = {
        { "fc0.pll_sys", 1 }, { "fc0.rosc", 3 }, { "fc0.xosc", 5 },
        { "fc0.clk_ref", 8 }, { "fc0.clk_sys", 9 }, { "fc0.clk_peri", 10 },
        { "fc0.clk_usb", 11 },
    };
    for (unsigned i = 0; i < sizeof fc / sizeof fc[0]; i++) {
        CLOCKS->FC0_REF_KHZ = 12000;
        CLOCKS->FC0_SRC = fc[i].src;
        while (CLOCKS->FC0_STATUS & (1u << 8))  /* RUNNING */
            ;
        line(fc[i].name, CLOCKS->FC0_RESULT >> 5);
    }

    /* ---- the atomic aliases, on a scratch register --------------------- */
    WATCHDOG->SCRATCH0 = 0xf0f0f0f0u;
    ALIAS_XOR(WATCHDOG->SCRATCH0) = 0xff00ff00u;
    line("alias.xor", WATCHDOG->SCRATCH0);
    ALIAS_SET(WATCHDOG->SCRATCH0) = 0x0000000fu;
    line("alias.set", WATCHDOG->SCRATCH0);
    ALIAS_CLR(WATCHDOG->SCRATCH0) = 0x0ff00000u;
    line("alias.clr", WATCHDOG->SCRATCH0);
    line("alias.read", ALIAS_SET(WATCHDOG->SCRATCH0));
    /* through the UART's interposer: a read and a write of the result */
    UART0->UARTIMSC = 0x10;
    ALIAS_SET(UART0->UARTIMSC) = 0x20;
    line("alias.uart-set", UART0->UARTIMSC);
    ALIAS_CLR(UART0->UARTIMSC) = 0x30;
    line("alias.uart-clr", UART0->UARTIMSC);

    /* ---- narrow writes, the datasheet's narrow_io_write ---------------- */
    volatile uint32_t *s32 = &WATCHDOG->SCRATCH0;
    volatile uint16_t *s16 = (volatile uint16_t *)s32;
    volatile uint8_t *s8 = (volatile uint8_t *)s32;
    *s32 = 0xdeadbeefu;
    line("narrow.32", *s32);
    line("narrow.8-read", (uint32_t)s8[0] << 24 | (uint32_t)s8[1] << 16 |
                          (uint32_t)s8[2] << 8 | s8[3]);
    s8[0] = 0xa5;
    line("narrow.8-at-0", *s32);
    s8[1] = 0x3c;
    line("narrow.8-at-1", *s32);
    s16[0] = 0xf00d;
    line("narrow.16-at-0", *s32);

    /* ---- the timer: 64 bits, latched, and an alarm --------------------- */
    TIMER->TIMELW = 0xfffffff0u;
    TIMER->TIMEHW = 1;
    uint32_t lo = TIMER->TIMELR;                /* latches the high half */
    delay_us(20);                               /* the low half wraps */
    uint32_t hi = TIMER->TIMEHR;
    line("timer.set-lo-near", lo - 0xfffffff0u < 4);
    line("timer.latched-hi", hi);
    line("timer.carried", TIMER->TIMERAWH);
    line("timer.unlatched-hi", TIMER->TIMEHR);
    /* the watchdog's counter: two a tick (RP2040-E1) */
    WATCHDOG->LOAD = 0x10000;
    WATCHDOG->CTRL = 1u << 30;
    delay_us(100);
    uint32_t wd = WATCHDOG->CTRL & 0xffffff;
    WATCHDOG->CTRL = 0;
    line("wd.counts-twice", 0x10000 - wd >= 198 && 0x10000 - wd <= 206);
    TIMER->INTE = 1;
    NVIC_ICPR = 1u << 0;
    NVIC_ISER = 1u << 0;
    uint32_t target = TIMER->TIMERAWL + 100;
    TIMER->ALARM0 = target;
    line("timer.ARMED", TIMER->ARMED);
    while (!alarm_irqs)
        __asm__ volatile("wfi");
    line("timer.fired-at-target", alarm_at - target < 3);
    line("timer.ARMED-after", TIMER->ARMED);
    line("timer.INTR-after", TIMER->INTR);
    TIMER->ALARM1 = TIMER->TIMERAWL + 1000;
    TIMER->ARMED = 2;                           /* disarmed */
    line("timer.disarmed", TIMER->ARMED);

    /* ---- the LED on GP25, and the pins -------------------------------- */
    IO_BANK0->GPIO25_CTRL = 5;                  /* SIO */
    SIO->GPIO_OE_SET = 1u << 25;
    for (int i = 0; i < 3; i++) {
        SIO->GPIO_OUT_SET = 1u << 25;
        delay_us(500);
        SIO->GPIO_OUT_CLR = 1u << 25;
        delay_us(500);
    }
    SIO->GPIO_OUT_XOR = 1u << 25;
    line("gpio.GPIO25_STATUS", IO_BANK0->GPIO25_STATUS);
    line("gpio.OUT", SIO->GPIO_OUT);
    IO_BANK0->GPIO25_CTRL = 5 | (2u << 8);      /* OUTOVER: low */
    line("gpio.GPIO25_STATUS-low", IO_BANK0->GPIO25_STATUS);
    IO_BANK0->GPIO25_CTRL = 5;
    SIO->GPIO_OUT_CLR = 1u << 25;
    /* inputs: GP2 by its pulls, GP24 the board's VBUS sense */
    IO_BANK0->GPIO2_CTRL = 5;
    PADS_BANK0->GPIO2 = (1u << 6) | (1u << 3);  /* IE, PUE */
    line("gpio.in-pullup", SIO->GPIO_IN >> 2 & 1);
    PADS_BANK0->GPIO2 = (1u << 6) | (1u << 2);  /* IE, PDE */
    line("gpio.in-pulldown", SIO->GPIO_IN >> 2 & 1);
    IO_BANK0->GPIO2_CTRL = 5 | (1u << 16);      /* INOVER: invert */
    line("gpio.in-inverted", SIO->GPIO_IN >> 2 & 1);
    IO_BANK0->GPIO24_CTRL = 5;
    line("gpio.vbus", SIO->GPIO_IN >> 24 & 1);
    /* GP3's rising edge: IO_IRQ_BANK0 */
    IO_BANK0->GPIO3_CTRL = 5;
    SIO->GPIO_OE_SET = 1u << 3;
    IO_BANK0->INTR0 = 0xffffffffu;
    IO_BANK0->PROC0_INTE0 = 8u << 12;           /* GPIO3_EDGE_HIGH */
    NVIC_ICPR = 1u << 13;
    NVIC_ISER = 1u << 13;
    SIO->GPIO_OUT_SET = 1u << 3;
    line("gpio.irqs", gpio_irqs);
    line("gpio.intr", gpio_intr >> 12 & 0xf);
    SIO->GPIO_OUT_CLR = 1u << 3;
    line("gpio.irqs-fall", gpio_irqs);
    line("gpio.INTR0-level", IO_BANK0->INTR0 >> 12 & 0xf);
    /* BOOTSEL: the QSPI chip select let go */
    IO_QSPI->GPIO_QSPI_SS_CTRL = 2u << 12;      /* OEOVER: disable */
    line("gpio.bootsel-in", SIO->GPIO_HI_IN >> 1 & 1);
    IO_QSPI->GPIO_QSPI_SS_CTRL = 0;

    /* ---- the SIO: divider, spinlocks, FIFOs --------------------------- */
    SIO->DIV_UDIVIDEND = 123456789u;
    SIO->DIV_UDIVISOR = 1000;
    uint32_t csr = SIO->DIV_CSR;
    __asm__ volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
    line("div.CSR-at-once", csr);
    line("div.CSR", SIO->DIV_CSR);
    line("div.u-rem", SIO->DIV_REMAINDER);
    line("div.u-quot", SIO->DIV_QUOTIENT);
    line("div.CSR-read", SIO->DIV_CSR);
    SIO->DIV_SDIVIDEND = (uint32_t)-7;
    SIO->DIV_SDIVISOR = 2;
    __asm__ volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
    line("div.s-rem", SIO->DIV_REMAINDER);
    line("div.s-quot", SIO->DIV_QUOTIENT);
    line("lock.claim", SIO->SPINLOCK5);
    line("lock.again", SIO->SPINLOCK5);
    line("lock.ST", SIO->SPINLOCK_ST);
    SIO->SPINLOCK5 = 1;
    line("lock.released", SIO->SPINLOCK_ST);
    line("fifo.ST", SIO->FIFO_ST);
    NVIC_ICPR = 1u << 15;
    NVIC_ISER = 1u << 15;
    for (int i = 0; i < 8; i++)
        SIO->FIFO_WR = (uint32_t)i;
    line("fifo.ST-full", SIO->FIFO_ST);
    SIO->FIFO_WR = 8;
    line("fifo.irqs-wof", sio_irqs);
    line("fifo.ST-after", SIO->FIFO_ST);
    (void)SIO->FIFO_RD;
    line("fifo.irqs-roe", sio_irqs);

    /* ---- the flash, through the SSI ------------------------------------ */
    uint8_t tx[13] = { 0x9f }, rx[13];
    flash_cmd(tx, rx, 4);
    line("flash.jedec", (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3]);
    tx[0] = 0x4b;
    flash_cmd(tx, rx, 13);
    line("flash.uid-hi", (uint32_t)rx[5] << 24 | (uint32_t)rx[6] << 16 |
                         (uint32_t)rx[7] << 8 | rx[8]);
    line("flash.uid-lo", (uint32_t)rx[9] << 24 | (uint32_t)rx[10] << 16 |
                         (uint32_t)rx[11] << 8 | rx[12]);
    tx[0] = 0x35;
    flash_cmd(tx, rx, 2);
    line("flash.SR2", rx[1]);
    tx[0] = 0x03;
    tx[1] = 0;
    tx[2] = 0x01;
    tx[3] = 0x00;
    flash_cmd(tx, rx, 8);
    line("flash.read-vector", (uint32_t)rx[7] << 24 | (uint32_t)rx[6] << 16 |
                              (uint32_t)rx[5] << 8 | rx[4]);
    line("flash.xip-vector", REG(0x10000100u));

    /* ---- the console's input (--input), until a newline or 10 ms ------ */
    UART0->UARTIMSC = 0;
    puts_("rx '");
    uint32_t t = TIMER->TIMERAWL;
    while (TIMER->TIMERAWL - t < 10000) {
        if (UART0->UARTFR & (1u << 4))          /* RXFE */
            continue;
        char c = (char)UART0->UARTDR;
        if (c == '\n')
            break;
        putc_(c);
    }
    puts_("'\n");
    NVIC_ICPR = 0xffffffffu;                    /* nothing left to come */
    puts_("done\n");
#ifdef WATCHDOG_END
    /* the watchdog: 2000 for a millisecond (it counts down twice a tick,
     * RP2040-E1), and the chip resets */
    WATCHDOG->LOAD = 2000;
    WATCHDOG->CTRL = 1u << 30;
#endif
    for (;;)
        ;
}
