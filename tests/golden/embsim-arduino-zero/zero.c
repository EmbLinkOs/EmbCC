/* The Arduino Zero's ATSAMD21G18A, as its datasheet (Atmel-42181G) has
 * it: built from the header, startup and linker script embsvd writes from
 * Microchip's ATSAMD21G18A.svd. Each line is "M NAME VALUE", the value
 * zero.txt holds, worked out from the datasheet (the SVD where it gives
 * none); "rx" lines show what SERCOM5 received (--input), and stderr the
 * board's LEDs -- L blinked every 1 ms by TC3's interrupt at 48 MHz --
 * and EmbSim's warnings. Output is SERCOM5's (the Zero's EDBG port),
 * polled on DRE; the run ends with a reset request (AIRCR). Built with
 * ERRATUM_9905 it writes DFLLMUL before the DFLL is requested, which
 * freezes the part. */
#include "samd21.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define NVIC_ISER REG(0xE000E100u)
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define AIRCR REG(0xE000ED0Cu)

static void putc_(char c)
{
    while (!(SERCOM5->USART.INTFLAG & 1))      /* DRE */
        ;
    SERCOM5->USART.DATA = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void line(const char *tag, const char *name, uint32_t v)
{
    puts_(tag);
    putc_(' ');
    puts_(name);
    putc_(' ');
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 0xf]);
    putc_('\n');
}

#define M(n, v) line("M", n, v)

/* ---- the clocks ----------------------------------------------------------- */

/* a generic clock (table 14-4's ID) on a generator */
static void gclk(uint32_t id, uint32_t gen)
{
    GCLK->CLKCTRL = (uint16_t)(id | gen << 8 | 1u << 14);
}

/* the generator's register, read through its ID */
static uint32_t genctrl(uint32_t id)
{
    *(volatile uint8_t *)&GCLK->GENCTRL = (uint8_t)id;
    return GCLK->GENCTRL;
}

static uint32_t gendiv(uint32_t id)
{
    *(volatile uint8_t *)&GCLK->GENDIV = (uint8_t)id;
    return GCLK->GENDIV;
}

static uint32_t clkctrl(uint32_t id)
{
    *(volatile uint8_t *)&GCLK->CLKCTRL = (uint8_t)id;
    return GCLK->CLKCTRL;
}

/* TC3 counts OSCULP32K (generator 2, 32.768 kHz), read continuously */
static uint32_t ulp(void)
{
    return TC3->COUNT16.COUNT;
}

/* the CPU's clock in units of 10 kHz: SysTick's cycles over 328 counts of
 * the 32 kHz reference (10 ms) */
static uint32_t cpu_10khz(void)
{
    SYST_RVR = 0xffffff;
    SYST_CVR = 0;
    SYST_CSR = 5;                               /* the CPU's clock, on */
    uint32_t c0 = ulp();
    while (ulp() == c0)
        ;
    uint32_t s0 = SYST_CVR, c1 = ulp();
    while ((uint16_t)(ulp() - c1) < 328)
        ;
    uint32_t cycles = s0 - SYST_CVR;
    SYST_CSR = 0;
    return (uint32_t)(((uint64_t)cycles * 32768 / 328 + 5000) / 10000);
}

/* ---- TC3: the LED, every 1 ms ----------------------------------------------- */

static volatile uint32_t ticks;

void TC3_IRQHandler(void)
{
    if (TC3->COUNT16.INTFLAG & 0x10) {          /* MC0 */
        TC3->COUNT16.INTFLAG = 0x10;
        PORT->OUTTGL0 = 1u << 17;               /* L */
        ticks++;
    }
}

int main(void)
{
    /* the reset values, before anything is touched */
    uint32_t r_cpusel = PM->CPUSEL, r_apbamask = PM->APBAMASK, r_apbbmask = PM->APBBMASK,
             r_apbcmask = PM->APBCMASK, r_ahbmask = PM->AHBMASK, r_rcause = PM->RCAUSE;
    uint32_t r_pclksr = SYSCTRL->PCLKSR, r_osc8m = SYSCTRL->OSC8M & 0xffff;
    uint32_t r_xosc32k = SYSCTRL->XOSC32K, r_dfllctrl = SYSCTRL->DFLLCTRL;
    uint32_t r_nvmb = NVMCTRL->CTRLB, r_param = NVMCTRL->PARAM, r_nvmflag = NVMCTRL->INTFLAG;
    uint32_t r_dir = PORT->DIR0, r_in = PORT->IN0, r_pincfg = PORT->PINCFG0_17;
    uint32_t r_gen0 = genctrl(0), r_gen1 = genctrl(1), r_gen2 = genctrl(2), r_gen3 = genctrl(3);
    uint32_t r_div1 = gendiv(1), r_clk_wdt = clkctrl(3), r_clk_s5 = clkctrl(0x19);
    /* a SERCOM with its APB clock masked: its registers cannot be written */
    SERCOM0->USART.CTRLA = 1u << 2;
    uint32_t masked = SERCOM0->USART.CTRLA;

    /* SERCOM5, the EDBG port: its APB clock, GCLK_SERCOM5_CORE on
     * generator 0, a USART with the internal clock */
    PM->APBCMASK |= 1u << 7 | 1u << 11 | 1u << 12 | 1u << 13;  /* SERCOM5, TC3-5 */
    gclk(0x19, 0);
    SERCOM5->USART.CTRLA = 1u << 2 | 1u << 30 | 3u << 20 | 1u << 16;  /* RXPO 3, TXPO 1 */
    SERCOM5->USART.CTRLB = 1u << 16 | 1u << 17;
    SERCOM5->USART.BAUD = 50436;                /* 9600 at 1 MHz (25.6.2.3) */
    SERCOM5->USART.CTRLA |= 2;

#ifdef ERRATUM_9905
    /* a DFLL register written before the DFLL is requested: the part
     * freezes */
    puts_("before\n");
    SYSCTRL->DFLLMUL = 1464;
    puts_("after\n");
#endif
    M("pm-cpusel", r_cpusel);
    M("pm-ahbmask", r_ahbmask);
    M("pm-apbamask", r_apbamask);
    M("pm-apbbmask", r_apbbmask);
    M("pm-apbcmask", r_apbcmask);
    M("pm-rcause", r_rcause);
    M("sysctrl-pclksr", r_pclksr);
    M("sysctrl-osc8m", r_osc8m);
    M("sysctrl-xosc32k", r_xosc32k);
    M("sysctrl-dfllctrl", r_dfllctrl);
    M("nvmctrl-ctrlb", r_nvmb);
    M("nvmctrl-param", r_param);
    M("nvmctrl-intflag", r_nvmflag);
    M("port-dir0", r_dir);
    M("port-in0", r_in);
    M("port-pincfg17", r_pincfg);
    M("gclk-genctrl0", r_gen0);
    M("gclk-genctrl1", r_gen1);
    M("gclk-genctrl2", r_gen2);
    M("gclk-genctrl3", r_gen3);
    M("gclk-gendiv1", r_div1);
    M("gclk-clkctrl-wdt", r_clk_wdt);
    M("gclk-clkctrl-sercom5", r_clk_s5);
    M("sercom0-masked", masked);

    /* GCLK, written and read back through the IDs */
    GCLK->GENDIV = 4 | 5u << 8;                 /* generator 4: DIV 5 */
    GCLK->GENCTRL = 4 | 6u << 8 | 1u << 16 | 1u << 20;  /* OSC8M, DIVSEL */
    M("gclk-gendiv4", gendiv(4));
    M("gclk-genctrl4", genctrl(4));
    GCLK->GENDIV = 2 | 0xffu << 8;              /* generator 2: 5 DIV bits */
    M("gclk-gendiv2-5bits", gendiv(2));
    GCLK->GENDIV = 2;
    M("gclk-status", GCLK->STATUS);
    /* PM */
    PM->INTFLAG = 1;
    PM->CPUSEL = 0;
    M("pm-ckrdy", PM->INTFLAG & 1);

    /* TC3 on the 32 kHz reference, read continuously */
    gclk(0x1b, 2);                              /* GCLK_TCC2_TC3: OSCULP32K */
    TC3->COUNT16.CTRLA = 0;
    TC3->COUNT16.CTRLA = 2;
    M("tc3-count-unsynced", TC3->COUNT16.COUNT);
    TC3->COUNT16.READREQ = 0x10 | 1u << 15;     /* RREQ of COUNT */
    M("tc3-count-synced-nonzero", TC3->COUNT16.COUNT != 0);
    TC3->COUNT16.READREQ = 0x10 | 1u << 14;     /* RCONT */

    /* the CPU's clock: 1 MHz out of reset */
    M("cpu-10khz-reset", cpu_10khz());
    SYSCTRL->OSC8M &= ~(3u << 8);               /* PRESC 0: 8 MHz */
    M("cpu-10khz-osc8m", cpu_10khz());
    /* the DFLL in open loop, 48 MHz -- with no flash wait state yet */
    SYSCTRL->DFLLCTRL = 0;                      /* ONDEMAND off (erratum 9905) */
    while (!(SYSCTRL->PCLKSR & 1u << 4))
        ;
    SYSCTRL->DFLLCTRL = 2;
    GCLK->GENCTRL = 0 | 7u << 8 | 1u << 16;     /* generator 0: DFLL48M */
    M("cpu-10khz-dfll-open", cpu_10khz());
    NVMCTRL->CTRLB = 1u << 1;                   /* RWS 1 */

    /* XOSC32K: ready 2048 + 3 cycles after it is enabled (STARTUP 2) */
    SYSCTRL->XOSC32K = 1u << 1 | 1u << 2 | 1u << 3 | 2u << 8;
    uint32_t t0 = ulp();
    while (!(SYSCTRL->PCLKSR & 1u << 1))
        ;
    M("xosc32k-startup-16cycles", (uint16_t)(ulp() - t0) / 16);
    M("sysctrl-intflag-xosc32krdy", SYSCTRL->INTFLAG & 2);
    SYSCTRL->INTFLAG = 2;
    M("sysctrl-intflag-cleared", SYSCTRL->INTFLAG & 2);

    /* the DFLL in closed loop: no lock without its reference */
    SYSCTRL->DFLLMUL = 1464 | 0x1fu << 16 | 0x1fu << 26;   /* 47.972352 MHz */
    SYSCTRL->DFLLCTRL = 2 | 4;                  /* closed loop */
    t0 = ulp();
    while ((uint16_t)(ulp() - t0) < 33)         /* 1 ms */
        ;
    M("dfll-lock-without-ref", SYSCTRL->PCLKSR & 0xc0);
    GCLK->GENCTRL = 1 | 5u << 8 | 1u << 16;     /* generator 1: XOSC32K */
    gclk(0, 1);                                 /* GCLK_DFLL48M_REF */
    t0 = ulp();
    while ((SYSCTRL->PCLKSR & 0xc0) != 0xc0)
        ;
    uint32_t lock = (uint16_t)(ulp() - t0);
    M("dfll-lock-200us", lock >= 6 && lock <= 7);
    M("cpu-10khz-dfll-closed", cpu_10khz());

    /* PORT */
    PORT->DIRSET0 = 1u << 17;
    PORT->OUTSET0 = 1u << 17;                   /* L on */
    M("port-in-no-inen", PORT->IN0 >> 17 & 1);
    PORT->PINCFG0_17 = 1u << 1;                 /* INEN */
    M("port-in-inen", PORT->IN0 >> 17 & 1);
    PORT->DIRCLR0 = 1u << 10;
    PORT->PINCFG0_10 = 1u << 1 | 1u << 2;       /* INEN, PULLEN */
    PORT->OUTSET0 = 1u << 10;
    M("port-pull-up", PORT->IN0 >> 10 & 1);
    PORT->OUTCLR0 = 1u << 10;
    PORT->OUTSET0 = 0;
    M("port-pull-down", PORT->IN0 >> 10 & 1);
    PORT->PINCFG0_10 = 1u << 1;
    PORT->OUTSET0 = 1u << 10;
    M("port-floating", PORT->IN0 >> 10 & 1);
    PORT->DIRTGL0 = 1u << 10 | 1u << 11;
    PORT->OUTTGL0 = 1u << 11;
    M("port-dir-toggled", PORT->DIR0 & 0xc00);
    M("port-out-toggled", PORT->OUT0 & 0xc00);
    PORT->DIRCLR0 = 1u << 10 | 1u << 11;
    PORT->OUTCLR0 = 1u << 10 | 1u << 11;
    /* WRCONFIG: PA20 and PA21 (the upper half, HWSEL) with INEN and PULLEN,
     * and peripheral function C */
    PORT->WRCONFIG0 = 1u << 31 | 1u << 30 | 1u << 28 | 2u << 24 | 1u << 18 | 1u << 17 |
                      (1u << 4 | 1u << 5);
    M("port-wrconfig-pincfg20", PORT->PINCFG0_20);
    M("port-wrconfig-pincfg21", PORT->PINCFG0_21);
    M("port-wrconfig-pmux10", PORT->PMUX0_10);
    /* the IOBUS: the same registers */
    PORT_IOBUS->OUTSET0 = 1u << 15;
    M("port-iobus-out", PORT->OUT0 >> 15 & 1);
    PORT->OUTCLR0 = 1u << 15;
    M("port-iobus-read", PORT_IOBUS->OUT0 >> 15 & 1);
    /* a byte written to OUTSET's third byte */
    PORT->OUTCLR0 = 1u << 17;
    ((volatile uint8_t *)&PORT->OUTSET0)[2] = 0x02;
    M("port-byte-outset", PORT->OUT0 >> 17 & 1);
    /* the RX LED, lit low; and L let go by PMUXEN */
    PORT->DIRSET1 = 1u << 3;
    PORT->PINCFG0_17 = 1u << 0 | 1u << 1;       /* PMUXEN: L is the peripheral's */
    M("port-pmuxen-released", PORT->IN0 >> 17 & 1);
    PORT->PINCFG0_17 = 1u << 1;

    /* SERCOM5 */
    M("sercom-intflag", SERCOM5->USART.INTFLAG);
    SERCOM5->USART.INTFLAG = 2;                 /* TXC, cleared */
    M("sercom-txc-cleared", SERCOM5->USART.INTFLAG & 2);
    SERCOM5->USART.BAUD = 1234;                 /* enable-protected */
    M("sercom-baud-protected", SERCOM5->USART.BAUD);
    SERCOM5->USART.CTRLA = (SERCOM5->USART.CTRLA & ~(7u << 2)) | 3u << 2 | 2;
    M("sercom-mode-protected", SERCOM5->USART.CTRLA >> 2 & 7);
    SERCOM5->USART.INTENSET = 0x05;
    SERCOM5->USART.INTENCLR = 0x01;
    M("sercom-inten", SERCOM5->USART.INTENSET);
    M("sercom-intenclr-reads", SERCOM5->USART.INTENCLR);
    SERCOM5->USART.INTENCLR = 0xff;
    M("sercom-syncbusy", SERCOM5->USART.SYNCBUSY);

    /* TC4 */
    gclk(0x1c, 2);                              /* GCLK_TC4_TC5: OSCULP32K */
    M("tc4-status-reset", TC4->COUNT16.STATUS);
    M("tc4-ctrlbclr-reset", TC4->COUNT16.CTRLBCLR);
    /* MFRQ: CC0 the top, OVF and MC0 at the wrap; MC1 as it leaves CC1 */
    TC4->COUNT16.CC0 = 99;
    TC4->COUNT16.CC1 = 49;
    TC4->COUNT16.CTRLA = 1u << 5 | 2;
    TC4->COUNT16.READREQ = 0x10 | 1u << 14;
    while (TC4->COUNT16.COUNT < 50)
        ;
    M("tc-mc1-before-top", TC4->COUNT16.INTFLAG & 0x31);
    while (!(TC4->COUNT16.INTFLAG & 1))
        ;
    M("tc-wrap-flags", TC4->COUNT16.INTFLAG & 0x31);
    M("tc-wrapped-count-low", TC4->COUNT16.COUNT < 5);
    /* enable-protected: PRESCALER is not written while enabled */
    TC4->COUNT16.CTRLA = 1u << 5 | 7u << 8 | 2;
    M("tc-prescaler-protected", TC4->COUNT16.CTRLA >> 8 & 7);
    /* STOP, RETRIGGER */
    TC4->COUNT16.CTRLBSET = 2u << 6;
    M("tc-stop", TC4->COUNT16.STATUS >> 3 & 1);
    uint32_t held = TC4->COUNT16.COUNT;
    t0 = ulp();
    while ((uint16_t)(ulp() - t0) < 10)
        ;
    M("tc-stopped-holds", TC4->COUNT16.COUNT == held);
    TC4->COUNT16.CTRLBSET = 1u << 6;
    M("tc-retrigger-runs", TC4->COUNT16.STATUS >> 3 & 1);
    /* one-shot: stops at the wrap */
    TC4->COUNT16.INTFLAG = 0xff;
    TC4->COUNT16.CTRLBSET = 1u << 2;
    TC4->COUNT16.CTRLBSET = 1u << 6;
    while (!(TC4->COUNT16.INTFLAG & 1))
        ;
    t0 = ulp();
    while ((uint16_t)(ulp() - t0) < 10)
        ;
    M("tc-oneshot-stopped", TC4->COUNT16.STATUS >> 3 & 1);
    M("tc-oneshot-count", TC4->COUNT16.COUNT);
    M("tc-ctrlb-reads", TC4->COUNT16.CTRLBCLR);
    TC4->COUNT16.CTRLBCLR = 1u << 2;
    /* counting down: from the top, round to the top */
    TC4->COUNT16.CTRLA = 0;
    TC4->COUNT16.INTFLAG = 0xff;
    TC4->COUNT16.CTRLBSET = 1;                  /* DIR */
    TC4->COUNT16.COUNT = 3;
    TC4->COUNT16.CTRLA = 1u << 5 | 2;
    while (!(TC4->COUNT16.INTFLAG & 1))
        ;
    M("tc-down-wraps-to-top", TC4->COUNT16.COUNT > 90);
    TC4->COUNT16.CTRLA = 0;
    TC4->COUNT16.CTRLBCLR = 1;
    /* SWRST, then COUNT8: PER the top */
    TC4->COUNT8.CTRLA = 1;
    TC4->COUNT8.CTRLA = 1u << 2;                /* MODE COUNT8, NFRQ */
    M("tc-count8-per-reset", TC4->COUNT8.PER);
    TC4->COUNT8.PER = 9;
    TC4->COUNT8.INTFLAG = 0xff;
    TC4->COUNT8.CTRLA = 1u << 2 | 2;
    t0 = ulp();
    while (!(TC4->COUNT8.INTFLAG & 1))
        ;
    M("tc-count8-period", (uint16_t)(ulp() - t0) <= 11);
    TC4->COUNT8.CTRLA = 0;
    /* COUNT32: TC4 the master, TC5 its slave */
    TC4->COUNT32.CTRLA = 2u << 2;
    M("tc5-slave", TC5->COUNT16.STATUS >> 4 & 1);
    TC4->COUNT32.COUNT = 0xfffffff0u;
    TC4->COUNT32.CTRLA = 2u << 2 | 2;
    t0 = ulp();
    while ((uint16_t)(ulp() - t0) < 20)
        ;
    TC4->COUNT32.READREQ = 0x10 | 1u << 15;
    M("tc4-count32-wrapped", TC4->COUNT32.COUNT < 0x100);
    TC4->COUNT32.CTRLA = 0;                     /* off: MODE is enable-protected */
    M("tc5-slave-of-stopped-tc4", TC5->COUNT16.STATUS >> 4 & 1);
    TC4->COUNT32.CTRLA = 0;                     /* now MODE COUNT16 */
    M("tc5-not-slave", TC5->COUNT16.STATUS >> 4 & 1);

    /* L every 1 ms: TC3 at 48 MHz / 64 = 750 kHz, MFRQ, CC0 749 -- from a
     * software reset, so that neither its count nor a flag is left over */
    TC3->COUNT16.CTRLA = 0;
    TC3->COUNT16.CTRLA = 1;                     /* SWRST */
    gclk(0x1b, 0);
    TC3->COUNT16.CC0 = 749;
    TC3->COUNT16.INTENSET = 0x10;
    TC3->COUNT16.CTRLA = 1u << 5 | 5u << 8 | 2;
    NVIC_ISER = 1u << 18;
    __asm__ volatile("cpsie i");
    while (ticks < 6)
        __asm__ volatile("wfi");
    __asm__ volatile("cpsid i");
    M("ticks", ticks);

    /* SERCOM5's receiver: the run's --input */
    uint32_t got = 0, n = 0;
    while ((SERCOM5->USART.INTFLAG & 4) && n < 2) {
        got = got << 8 | (SERCOM5->USART.DATA & 0xff);
        n++;
    }
    line("rx", "bytes", got);
    line("rx", "more", SERCOM5->USART.INTFLAG >> 2 & 1);
    SERCOM5->USART.CTRLB = 1u << 16;            /* RXEN off: the buffer is emptied */
    line("rx", "flushed", SERCOM5->USART.INTFLAG >> 2 & 1);

    puts_("end\n");
    AIRCR = 0x05fa0004;                         /* SYSRESETREQ */
    puts_("not reset\n");
    for (;;)
        ;
}
