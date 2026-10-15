/* The nRF52840 DK, as the nRF52840 Product Specification (v1.1) has it:
 * built from the header, startup and linker script embsvd writes from
 * Nordic's nrf52840.svd, for ARMv6-M and 16 KiB of RAM, so the one image
 * runs on QEMU's micro:bit too, whose nRF51 has the same UART, GPIO and
 * TIMER at the same addresses.
 *
 * Each line is "Q NAME VALUE" or "M NAME VALUE":
 *   Q  QEMU's nRF51 models it as the nRF52840's PS has it: EmbSim must
 *      print what QEMU prints;
 *   M  the nRF52840's alone (CLOCK, RTC, UARTE, P1, FICR) or modelled
 *      otherwise by QEMU: dk.txt holds the PS's values.
 * Output is UART0's (ENABLE 4: the nRF51's UART), polled on TXDRDY. The
 * Q lines come first: QEMU's run ends at "q-end", before the FICR, which
 * an nRF51 does not have there. */
#include "nrf52840.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define NVIC_ISER REG(0xE000E100u)
#define NVIC_ICPR REG(0xE000E280u)

static void putc_(char c)
{
    NRF_UART0->EVENTS_TXDRDY = 0;
    NRF_UART0->TXD = (uint8_t)c;
    for (int i = 0; i < 100000 && !NRF_UART0->EVENTS_TXDRDY; i++)
        ;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void line(char tag, const char *name, uint32_t v)
{
    putc_(tag);
    putc_(' ');
    puts_(name);
    putc_(' ');
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 0xf]);
    putc_('\n');
}

#define Q(n, v) line('Q', n, v)
#define M(n, v) line('M', n, v)

static volatile uint32_t tim_irqs, rtc_irqs, rtc_at;

void TIMER1_IRQHandler(void)
{
    if (NRF_TIMER1->EVENTS_COMPARE[0]) {
        NRF_TIMER1->EVENTS_COMPARE[0] = 0;
        NRF_P0->OUT ^= 1u << 13;                /* LED1 */
        tim_irqs++;
    }
}

void RTC0_IRQHandler(void)
{
    if (NRF_RTC0->EVENTS_COMPARE[0]) {
        NRF_RTC0->EVENTS_COMPARE[0] = 0;
        NRF_TIMER2->TASKS_CAPTURE[0] = 1;
        rtc_at = NRF_TIMER2->CC[0];
        rtc_irqs++;
    }
}

/* EasyDMA reads RAM: the bytes to send are copied there */
static char txbuf[16], rxbuf[8];

int main(void)
{
    NRF_UART0->PSEL.TXD = 6;                    /* the DK's VCOM */
    NRF_UART0->PSEL.RXD = 8;
    NRF_UART0->BAUDRATE = 0x01d7e000;           /* 115200 */
    NRF_UART0->ENABLE = 4;
    NRF_UART0->TASKS_STARTTX = 1;
    puts_("nrf52840dk\n");
    M("UART0.ENABLE", NRF_UART0->ENABLE);       /* QEMU's reads 0 */
    M("UART0.BAUDRATE", NRF_UART0->BAUDRATE);   /* QEMU's keeps its reset value */
    Q("UART0.EVENTS_TXDRDY", NRF_UART0->EVENTS_TXDRDY);

    /* ---- GPIO --------------------------------------------------------------- */
    Q("P0.PIN_CNF0-reset", NRF_P0->PIN_CNF[0]);
    Q("P0.DIR-reset", NRF_P0->DIR);
    NRF_P0->DIRSET = (1u << 13) | (1u << 14);   /* LED1, LED2 */
    Q("P0.DIR", NRF_P0->DIR);
    Q("P0.PIN_CNF13", NRF_P0->PIN_CNF[13]);
    NRF_P0->OUTSET = 3u << 13;                  /* both off: they light low */
    NRF_P0->OUTCLR = 1u << 14;                  /* LED2 on */
    Q("P0.OUT", NRF_P0->OUT & (3u << 13));
    Q("P0.IN-outputs", NRF_P0->IN & (3u << 13));
    NRF_P0->PIN_CNF[3] = 3u << 2;               /* input, connected, pull-up */
    NRF_P0->PIN_CNF[4] = 1u << 2;               /* pull-down */
    Q("P0.IN-pulls", NRF_P0->IN & (3u << 3));
    NRF_P0->PIN_CNF[5] = (3u << 2) | 2u;        /* pull-up, input disconnected */
    M("P0.IN-disconnected", NRF_P0->IN >> 5 & 1);
    NRF_P0->PIN_CNF[11] = 3u << 2;              /* BUTTON1: pull-up */
    M("P0.IN-button1", NRF_P0->IN >> 11 & 1);
    /* an open-drain output (S0D1) at 1 is let go: its pull-up holds it */
    NRF_P0->PIN_CNF[6] = 1u | (3u << 2) | (6u << 8);
    NRF_P0->OUTSET = 1u << 6;
    M("P0.IN-open-drain", NRF_P0->IN >> 6 & 1);
    NRF_P0->OUTCLR = 1u << 6;
    M("P0.IN-open-drain-low", NRF_P0->IN >> 6 & 1);
    NRF_P1->DIRSET = 1u << 2;
    NRF_P1->OUTSET = 1u << 2;
    M("P1.IN", NRF_P1->IN & 4);
    NRF_P0->OUTSET = 1u << 14;                  /* LED2 off */

    /* ---- TIMER0 as a counter -------------------------------------------- */
    NRF_TIMER0->MODE = 1;                       /* Counter */
    NRF_TIMER0->BITMODE = 1;                    /* 8 bits */
    NRF_TIMER0->CC[0] = 5;
    NRF_TIMER0->CC[1] = 2;
    NRF_TIMER0->SHORTS = 1u << 0;               /* COMPARE0_CLEAR */
    NRF_TIMER0->TASKS_START = 1;
    for (int i = 0; i < 7; i++)
        NRF_TIMER0->TASKS_COUNT = 1;
    NRF_TIMER0->TASKS_CAPTURE[2] = 1;
    Q("TIMER0.count-after-7", NRF_TIMER0->CC[2]);
    Q("TIMER0.COMPARE0", NRF_TIMER0->EVENTS_COMPARE[0]);
    Q("TIMER0.COMPARE1", NRF_TIMER0->EVENTS_COMPARE[1]);
    NRF_TIMER0->SHORTS = 0;
    NRF_TIMER0->TASKS_CLEAR = 1;
    for (int i = 0; i < 300; i++)
        NRF_TIMER0->TASKS_COUNT = 1;
    NRF_TIMER0->TASKS_CAPTURE[2] = 1;
    Q("TIMER0.8-bit-wrap", NRF_TIMER0->CC[2]);
    NRF_TIMER0->TASKS_STOP = 1;
    NRF_TIMER0->TASKS_COUNT = 1;
    NRF_TIMER0->TASKS_CAPTURE[2] = 1;
    M("TIMER0.stopped", NRF_TIMER0->CC[2]);     /* QEMU's counts stopped */
    NRF_TIMER0->INTENSET = 1u << 17;
    Q("TIMER0.INTENSET", NRF_TIMER0->INTENSET);
    NRF_TIMER0->INTENCLR = 1u << 17;
    Q("TIMER0.INTENCLR", NRF_TIMER0->INTENCLR);
    /* the rest is the nRF52840's: an nRF51 has no FICR words there */
    puts_("q-end\n");

    /* ---- the nRF52840's own: FICR, CLOCK ---------------------------------- */
    M("FICR.INFO.PART", NRF_FICR->INFO.PART);
    M("FICR.INFO.RAM", NRF_FICR->INFO.RAM);
    M("FICR.INFO.FLASH", NRF_FICR->INFO.FLASH);
    M("FICR.CODEPAGESIZE", NRF_FICR->CODEPAGESIZE);
    M("CLOCK.HFCLKSTAT-reset", NRF_CLOCK->HFCLKSTAT);
    /* the HFXO, timed by TIMER2 at 1 MHz: HFXODEBOUNCE 0x10, 256 us */
    NRF_TIMER2->MODE = 0;
    NRF_TIMER2->BITMODE = 3;
    NRF_TIMER2->PRESCALER = 4;                  /* 16 MHz / 16 */
    NRF_TIMER2->TASKS_CLEAR = 1;
    NRF_TIMER2->TASKS_START = 1;
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    for (int i = 0; i < 100000 && !NRF_CLOCK->EVENTS_HFCLKSTARTED; i++)
        ;
    NRF_TIMER2->TASKS_CAPTURE[0] = 1;
    uint32_t hf_us = NRF_TIMER2->CC[0];
    M("CLOCK.HFCLKSTAT", NRF_CLOCK->HFCLKSTAT);
    M("CLOCK.HFCLKRUN", NRF_CLOCK->HFCLKRUN);
    M("CLOCK.hfxo-256us", hf_us >= 255 && hf_us <= 260);
    NRF_CLOCK->LFCLKSRC = 1;                    /* the DK's 32.768 kHz crystal */
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    for (int i = 0; i < 100000 && !NRF_CLOCK->EVENTS_LFCLKSTARTED; i++)
        ;
    M("CLOCK.LFCLKSTAT", NRF_CLOCK->LFCLKSTAT);
    M("CLOCK.LFCLKSRCCOPY", NRF_CLOCK->LFCLKSRCCOPY);

    /* ---- RTC0: 32 counts of 32768 Hz, timed by TIMER2 --------------------- */
    NRF_RTC0->PRESCALER = 0;
    NRF_RTC0->CC[0] = 32;
    NRF_RTC0->INTENSET = 1u << 16;              /* COMPARE0 */
    NVIC_ICPR = 1u << RTC0_IRQn;
    NVIC_ISER = 1u << RTC0_IRQn;
    NRF_TIMER2->TASKS_CLEAR = 1;
    NRF_RTC0->TASKS_START = 1;
    NRF_RTC0->PRESCALER = 99;                   /* ignored: it runs */
    for (int i = 0; i < 100000 && !rtc_irqs; i++)
        ;
    M("RTC0.PRESCALER", NRF_RTC0->PRESCALER);
    M("RTC0.COUNTER-at-compare", NRF_RTC0->COUNTER >= 32 && NRF_RTC0->COUNTER <= 33);
    /* 32 / 32768 s = 976.6 us */
    M("RTC0.compare-977us", rtc_at >= 975 && rtc_at <= 979);
    M("RTC0.TICK-off", NRF_RTC0->EVENTS_TICK);  /* neither EVTEN nor INTEN */
    NRF_RTC0->EVTENSET = 1u << 0;
    for (int i = 0; i < 100000 && !NRF_RTC0->EVENTS_TICK; i++)
        ;
    M("RTC0.TICK-on", NRF_RTC0->EVENTS_TICK);
    NRF_RTC0->TASKS_TRIGOVRFLW = 1;
    M("RTC0.TRIGOVRFLW", NRF_RTC0->COUNTER);
    NRF_RTC0->TASKS_STOP = 1;

    /* ---- TIMER1 at 1 ms, LED1 toggling ---------------------------------- */
    NRF_P0->OUTSET = 1u << 13;
    NRF_TIMER1->MODE = 0;
    NRF_TIMER1->BITMODE = 3;
    NRF_TIMER1->PRESCALER = 4;                  /* 1 MHz */
    NRF_TIMER1->CC[0] = 1000;
    NRF_TIMER1->SHORTS = 1u << 0;
    NRF_TIMER1->INTENSET = 1u << 16;
    NVIC_ICPR = 1u << TIMER1_IRQn;
    NVIC_ISER = 1u << TIMER1_IRQn;
    NRF_TIMER1->TASKS_START = 1;
    for (int i = 0; i < 1000000 && tim_irqs < 4; i++)
        ;
    NRF_TIMER1->TASKS_STOP = 1;
    M("TIMER1.irqs", tim_irqs);

    /* ---- UARTE0: the same space as UART0, EasyDMA from RAM --------------- */
    NRF_UART0->TASKS_STOPTX = 1;
    NRF_UART0->ENABLE = 0;
    NRF_UARTE0->ENABLE = 8;
    /* from flash: EasyDMA cannot reach it (a warning, nothing sent) */
    static const char rom_msg[] = "from flash\n";
    NRF_UARTE0->TXD.PTR = (uint32_t)rom_msg;
    NRF_UARTE0->TXD.MAXCNT = 11;
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NRF_UARTE0->TASKS_STARTTX = 1;
    for (int i = 0; i < 100000 && !NRF_UARTE0->EVENTS_ENDTX; i++)
        ;
    uint32_t rom_amount = NRF_UARTE0->TXD.AMOUNT;
    const char *msg = "uarte says hi\n";
    for (int i = 0; msg[i]; i++)
        txbuf[i] = msg[i];
    NRF_UARTE0->TXD.PTR = (uint32_t)txbuf;
    NRF_UARTE0->TXD.MAXCNT = 14;
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NRF_UARTE0->TASKS_STARTTX = 1;
    for (int i = 0; i < 100000 && !NRF_UARTE0->EVENTS_ENDTX; i++)
        ;
    uint32_t amount = NRF_UARTE0->TXD.AMOUNT, started = NRF_UARTE0->EVENTS_TXSTARTED;
    /* the input (--input), by EasyDMA: 4 bytes into rxbuf */
    NRF_UARTE0->RXD.PTR = (uint32_t)rxbuf;
    NRF_UARTE0->RXD.MAXCNT = 4;
    NRF_UARTE0->EVENTS_ENDRX = 0;
    NRF_UARTE0->TASKS_STARTRX = 1;
    for (int i = 0; i < 100000 && !NRF_UARTE0->EVENTS_ENDRX; i++)
        ;
    uint32_t rx_amount = NRF_UARTE0->RXD.AMOUNT;
    NRF_UARTE0->TASKS_STOPRX = 1;
    NRF_UARTE0->ENABLE = 0;
    NRF_UART0->ENABLE = 4;
    NRF_UART0->TASKS_STARTTX = 1;
    M("UARTE0.TXD.AMOUNT-flash", rom_amount);
    M("UARTE0.TXD.AMOUNT", amount);
    M("UARTE0.EVENTS_TXSTARTED", started);
    puts_("rx '");
    for (uint32_t i = 0; i < rx_amount && i < 4; i++)
        putc_(rxbuf[i]);
    puts_("'\n");
    NVIC_ICPR = 0xffffffffu;
    puts_("done\n");
    for (;;)
        ;
}
