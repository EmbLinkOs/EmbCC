/* The ESP32-C3-DevKitM-1, as the ESP32-C3 Technical Reference Manual
 * (v1.4) has it: built from the header, startup and linker script embsvd
 * writes from Espressif's esp32c3.svd. Each line is "M NAME VALUE", the
 * value c3.txt holds, worked out from the TRM (and the SVD where the TRM
 * gives none); "rx" lines show what UART0 received (--input), and stderr
 * the board's RGB LED, which a SYSTIMER interrupt blinks every 1 ms.
 * Output is UART0's, polled on STATUS.TXFIFO_CNT; the run ends with
 * RTC_CNTL's software reset. */
#include "esp32c3.h"

#define LED (1u << 8)                   /* GPIO8: the RGB LED's DIN */

static void putc_(char c)
{
    while ((UART0->STATUS >> 16 & 0x3ff) >= 126)
        ;
    UART0->FIFO = (uint8_t)c;
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

/* ---- the core's CSRs ---------------------------------------------------- */

#define CSRR(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define CSRW(csr, v) __asm__ volatile("csrw " #csr ", %0" : : "r"((uint32_t)(v)))

/* the performance counter: mpccr, counting what mpcer selects */
#define cycles() CSRR(0x7e2)

/* exceptions: the cause and value of the last, and the count */
static volatile uint32_t exc_n, exc_cause, exc_tval, exc_spare;

__attribute__((interrupt)) void Exception_Handler(void)
{
    uint32_t epc = CSRR(mepc);
    exc_n++;
    exc_cause = CSRR(mcause);
    exc_tval = CSRR(mtval);
    /* past the instruction, read through DRAM, the data bus's view of
     * the IRAM it ran from (0x700000 below) */
    uint16_t h = *(volatile uint16_t *)(epc - 0x700000u);
    CSRW(mepc, epc + ((h & 3) == 3 ? 4 : 2));
}

/* ---- the interrupt matrix's tests: software interrupts ------------------ */

static volatile uint32_t order, order_n, sw_cause, edge_n, level_n;

static void from_cpu(int n, int v)
{
    (&SYSTEM->CPU_INTR_FROM_CPU_0)[n] = (uint32_t)v;
}

static void took(void)
{
    uint32_t c = CSRR(mcause);
    sw_cause = c;
    order = order << 4 | (c & 31);
    order_n++;
}

__attribute__((interrupt)) void CPU_INT2_Handler(void) { took(); from_cpu(2, 0); }
__attribute__((interrupt)) void CPU_INT3_Handler(void) { took(); from_cpu(0, 0); }
__attribute__((interrupt)) void CPU_INT4_Handler(void) { took(); from_cpu(1, 0); }

/* an edge type: claimed, then cleared by toggling CPU_INT_CLEAR */
__attribute__((interrupt)) void CPU_INT5_Handler(void)
{
    edge_n++;
    INTERRUPT_CORE0->CPU_INT_CLEAR = 1u << 5;
    INTERRUPT_CORE0->CPU_INT_CLEAR = 0;
}

/* a level type: taken again while its source is high */
__attribute__((interrupt)) void CPU_INT6_Handler(void)
{
    if (++level_n == 3)
        from_cpu(3, 0);
}

/* ---- the RGB LED: an SK68XXMINI-HS on GPIO8, at 160 MHz ------------------ */

/* one bit: high for 0.3 us (a 0) or 0.6 us (a 1), 1.25 us in all, timed
 * by the cycle counter at 160 MHz */
static void led_bit(int b)
{
    GPIO->OUT_W1TS = LED;
    uint32_t t = cycles();
    while (cycles() - t < (b ? 96u : 48u))
        ;
    GPIO->OUT_W1TC = LED;
    while (cycles() - t < 200u)
        ;
}

static void led_rgb(uint32_t rgb)
{
    uint32_t grb = (rgb >> 8 & 0xff) << 16 | (rgb >> 16 & 0xff) << 8 | (rgb & 0xff);
    for (int i = 23; i >= 0; i--)
        led_bit((int)(grb >> i & 1));
}

static volatile uint32_t ticks, tick1;
static uint32_t unit_lo(int u);

/* SYSTIMER's comparator 0, every 1 ms: the LED on and off; UNIT0 at the
 * first, which the test holds the board's time to */
__attribute__((interrupt)) void CPU_INT1_Handler(void)
{
    SYSTIMER->INT_CLR = 1;
    if (++ticks == 1)
        tick1 = unit_lo(0);
    led_rgb(ticks & 1 ? 0x201008u : 0);
    if (ticks == 1)
        led_rgb(0x0000ffu);                     /* DOUT's, for a next LED */
}

/* ---- SYSTIMER -------------------------------------------------------------- */

static uint32_t unit_lo(int u)
{
    volatile uint32_t *op = u ? &SYSTIMER->UNIT1_OP : &SYSTIMER->UNIT0_OP;
    *op = 1u << 30;                             /* UPDATE */
    while (!(*op >> 29 & 1))                    /* VALUE_VALID */
        ;
    return u ? SYSTIMER->UNIT1_VALUE_LO : SYSTIMER->UNIT0_VALUE_LO;
}

/* comparator 1, on UNIT1, in target mode: TARGET1 is (hi, lo) */
static void target1(uint32_t hi, uint32_t lo)
{
    SYSTIMER->TARGET1_HI = hi;
    SYSTIMER->TARGET1_LO = lo;
    SYSTIMER->COMP1_LOAD = 1;
}

/* the UNIT0 counts in 20000 CPU cycles, in thousands */
static uint32_t clock_check(void)
{
    uint32_t t0 = unit_lo(0), c0 = cycles();
    while (cycles() - c0 < 20000u)
        ;
    return (unit_lo(0) - t0) / 1000;
}

int main(void)
{
    /* what the ROM leaves running: the watchdogs, off (EmbSim models
     * none of them; on the chip this comes first) */
    RTC_CNTL->WDTWPROTECT = 0x50d83aa1u;
    RTC_CNTL->WDTCONFIG0 = 0;
    RTC_CNTL->SWD_WPROTECT = 0x8f1d312au;
    RTC_CNTL->SWD_CONF |= 1u << 31;             /* SWD_AUTO_FEED_EN */
    TIMG0->WDTWPROTECT = 0x50d83aa1u;
    TIMG0->WDTCONFIG0 = 0;

    /* UART0 before its first byte */
    uint32_t u_raw0 = UART0->INT_RAW, u_status0 = UART0->STATUS;
    M("uart-int-raw", u_raw0);
    M("uart-status", u_status0);
    M("uart-clkdiv", UART0->CLKDIV);
    M("uart-conf0", UART0->CONF0);
    M("uart-conf1", UART0->CONF1);
    M("uart-int-raw-sent", UART0->INT_RAW);
    UART0->INT_CLR = 0x4002;
    M("uart-int-raw-cleared", UART0->INT_RAW);  /* TXFIFO_EMPTY again */
    UART0->CONF1 = 0xc060u & ~(0x1ffu << 9);    /* TXFIFO_EMPTY_THRHD 0 */
    UART0->INT_RAW = 0x4002;                    /* the putc's TX_DONE, by W1C */
    M("uart-int-raw-thrhd0", UART0->INT_RAW);
    UART0->CONF1 = 0xc060u;
    UART0->INT_ENA = 1u << 14;                  /* TX_DONE */
    putc_('\n');
    M("uart-int-st", UART0->INT_ST);
    M("intr-status0-uart0", INTERRUPT_CORE0->INTR_STATUS_REG_0 & 1u << 21);
    UART0->INT_ENA = 0;
    UART0->INT_CLR = 0xffffffffu;

    /* the core */
    M("misa", CSRR(misa));
    CSRW(misa, 0);
    M("misa-written", CSRR(misa));
    M("mvendorid", CSRR(mvendorid));
    M("marchid", CSRR(marchid));
    M("mimpid", CSRR(mimpid));
    M("mhartid", CSRR(mhartid));
    uint32_t vt = CSRR(mtvec);
    M("mtvec-mode", vt & 0xff);
    CSRW(mtvec, 0x123);
    M("mtvec-0x123", CSRR(mtvec));
    CSRW(mtvec, vt);
    __asm__ volatile("csrc mstatus, %0" : : "r"(0x1800u));
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x1000u));
    M("mstatus-mpp-hi", CSRR(mstatus) & 0x1800);
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x0800u));
    M("mstatus-mpp-lo", CSRR(mstatus) & 0x1800);
    M("mpcmr", CSRR(0x7e1));
    M("mpcer", CSRR(0x7e0));
    CSRW(0x7e0, 2);                             /* INST */
    uint32_t n;
    __asm__ volatile("csrw 0x7e2, zero\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
                     "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tcsrr %0, 0x7e2" : "=r"(n));
    M("mpccr-10-insns", n);
    CSRW(0x7e1, 0);                             /* COUNT_EN off */
    n = CSRR(0x7e2);
    __asm__ volatile("nop\n\tnop\n\tnop");
    M("mpccr-stopped", CSRR(0x7e2) - n);
    CSRW(0x7e1, 3);
    CSRW(0x7e2, 0xfffffffeu);
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop");
    M("mpccr-saturated", CSRR(0x7e2));
    CSRW(0x7e1, 1);                             /* COUNT_SAT off: wraps */
    CSRW(0x7e2, 0xfffffffeu);
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop");
    M("mpccr-wrapped", CSRR(0x7e2) < 16);
    CSRW(0x7e1, 3);
    CSRW(0x7e0, 1);                             /* CYCLE, from here on */
    CSRW(0x803, 0x1ffu);
    M("cpu-gpio-oen", CSRR(0x803));
    CSRW(0x3b0, 0x12345678u);
    M("pmpaddr0", CSRR(0x3b0));

    /* exceptions: the cause and the value */
    __asm__ volatile("csrr t0, 0x304" : : : "t0");      /* mie: none */
    M("exc-mie-cause", exc_cause);
    M("exc-mie-tval", exc_tval);
    __asm__ volatile("csrr t0, 0x7b0" : : : "t0");      /* dcsr: debug only */
    M("exc-dcsr-tval", exc_tval);
    __asm__ volatile("csrw 0xf11, zero");                /* mvendorid: RO */
    M("exc-mvendorid-tval", exc_tval);
    __asm__ volatile("csrw 0x804, zero");                /* cpu_gpio_in: RO */
    M("exc-gpio-in-tval", exc_tval);
    __atomic_fetch_add(&exc_spare, 1, __ATOMIC_RELAXED); /* no A: amoadd.w */
    M("exc-amo-cause", exc_cause);
    M("exc-amo-tval-op", exc_tval & 0xf800707fu);       /* its funct5 and opcode */
    __asm__ volatile("ecall");
    M("exc-ecall-cause", exc_cause);
    __asm__ volatile("ebreak");
    M("exc-ebreak-cause", exc_cause);
    /* half a semihosting call is a breakpoint: an ebreak needs both its
     * markers to be one */
    exc_cause = 0;
    __asm__ volatile("ebreak\n\tsrai x0, x0, 7");
    M("exc-ebreak-no-slli-cause", exc_cause);
    exc_cause = 0;
    __asm__ volatile("slli x0, x0, 0x1f\n\tebreak");
    M("exc-ebreak-no-srai-cause", exc_cause);
    M("exc-count", exc_n);

    /* SYSTEM, the clock */
    M("sysclk-conf", SYSTEM->SYSCLK_CONF);
    M("cpu-per-conf", SYSTEM->CPU_PER_CONF);
    M("systimer-k-20mhz", clock_check());

    /* SYSTIMER */
    M("st-conf", SYSTIMER->CONF);
    M("st-unit1", unit_lo(1));                  /* UNIT1 is not counting */
    SYSTIMER->UNIT1_LOAD_HI = 0xabcdeu;
    SYSTIMER->UNIT1_LOAD_LO = 0x12345678u;
    SYSTIMER->UNIT1_LOAD = 1;
    M("st-unit1-loaded-lo", unit_lo(1));
    M("st-unit1-loaded-hi", SYSTIMER->UNIT1_VALUE_HI);
    SYSTIMER->TARGET1_CONF = 1u << 31;          /* UNIT1, target mode */
    target1(0xabcdeu, 0x12345678u + 5);
    SYSTIMER->CONF |= 1u << 23;                 /* TARGET1_WORK_EN */
    M("st-raw-ahead", SYSTIMER->INT_RAW);
    target1(0xabcdeu, 0x12345678u);
    M("st-raw-reached", SYSTIMER->INT_RAW);
    M("st-st-disabled", SYSTIMER->INT_ST);
    SYSTIMER->INT_ENA = 2;
    M("st-st-enabled", SYSTIMER->INT_ST);
    SYSTIMER->INT_CLR = 2;
    M("st-raw-once", SYSTIMER->INT_RAW);
    target1(0xabcdeu, 0x12345678u - 100);
    M("st-raw-passed", SYSTIMER->INT_RAW);
    SYSTIMER->INT_RAW = 2;
    M("st-raw-w1c", SYSTIMER->INT_RAW);
    /* past by 2^51 - 2: at once; by 2^51 - 1 or 2^51: after the wrap */
    target1(0xabcdeu - 0x80000u, 0x12345678u + 2);
    M("st-raw-2^51-2", SYSTIMER->INT_RAW);
    SYSTIMER->INT_CLR = 2;
    target1(0xabcdeu - 0x80000u, 0x12345678u + 1);
    M("st-raw-2^51-1", SYSTIMER->INT_RAW);
    target1(0xabcdeu - 0x80000u, 0x12345678u);
    M("st-raw-2^51", SYSTIMER->INT_RAW);
    target1(0xabcdeu, 0x12345678u + 1600);      /* 0.1 ms ahead */
    SYSTIMER->CONF |= 1u << 29;                 /* UNIT1_WORK_EN */
    uint32_t c0 = cycles();
    while (!(SYSTIMER->INT_RAW & 2))
        ;
    M("st-alarm-100-cycles", (cycles() - c0 + 50) / 100);   /* 0.1 ms at 20 MHz */
    SYSTIMER->INT_CLR = 2;
    SYSTIMER->CONF &= ~(1u << 23 | 1u << 29);
    SYSTIMER->INT_ENA = 0;

    /* the interrupt matrix */
    INTERRUPT_CORE0->CPU_INTR_FROM_CPU_0_MAP = 3;
    INTERRUPT_CORE0->CPU_INTR_FROM_CPU_1_MAP = 4;
    INTERRUPT_CORE0->CPU_INTR_FROM_CPU_2_MAP = 2;
    INTERRUPT_CORE0->CPU_INT_PRI_2 = 7;
    INTERRUPT_CORE0->CPU_INT_PRI_3 = 5;
    INTERRUPT_CORE0->CPU_INT_PRI_4 = 5;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 0x1c;
    from_cpu(0, 1);
    from_cpu(1, 1);
    from_cpu(2, 1);
    M("intr-status1", INTERRUPT_CORE0->INTR_STATUS_REG_1);
    M("eip", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_THRESH = 6;
    M("eip-thresh6", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_THRESH = 5;
    M("eip-thresh5", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_THRESH = 0;
    INTERRUPT_CORE0->CPU_INT_PRI_4 = 0;
    M("eip-pri0", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_PRI_4 = 5;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 0x14;
    M("eip-disabled", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_ENABLE = 0x1c;
    __asm__ volatile("csrs mstatus, 8");
    __asm__ volatile("csrc mstatus, 8");
    M("order", order);
    M("order-n", order_n);
    M("int-mcause", sw_cause);
    M("eip-after", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);

    /* an edge type */
    INTERRUPT_CORE0->CPU_INTR_FROM_CPU_3_MAP = 5;
    INTERRUPT_CORE0->CPU_INT_TYPE = 1u << 5;
    INTERRUPT_CORE0->CPU_INT_PRI_5 = 1;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 1u << 5;
    from_cpu(3, 1);
    from_cpu(3, 0);
    M("edge-latched", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_CLEAR = 1u << 5;
    INTERRUPT_CORE0->CPU_INT_CLEAR = 0;
    M("edge-unclaimed-clear", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    INTERRUPT_CORE0->CPU_INT_ENABLE = 0;
    INTERRUPT_CORE0->CPU_INT_CLEAR = 1u << 5;
    INTERRUPT_CORE0->CPU_INT_CLEAR = 0;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 1u << 5;
    M("edge-flushed", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    from_cpu(3, 1);
    from_cpu(3, 0);
    __asm__ volatile("csrs mstatus, 8");
    __asm__ volatile("csrc mstatus, 8");
    M("edge-taken", edge_n);
    /* a level type */
    INTERRUPT_CORE0->CPU_INTR_FROM_CPU_3_MAP = 6;
    INTERRUPT_CORE0->CPU_INT_TYPE = 0;
    INTERRUPT_CORE0->CPU_INT_PRI_6 = 1;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 1u << 6;
    from_cpu(3, 1);
    __asm__ volatile("csrs mstatus, 8");
    __asm__ volatile("csrc mstatus, 8");
    M("level-taken", level_n);
    INTERRUPT_CORE0->CPU_INTR_FROM_CPU_3_MAP = 0;

    /* WFI with MIE off: woken by comparator 2's alarm, pending */
    SYSTIMER->TARGET2_CONF = 0;                 /* UNIT0, target mode */
    uint32_t now = unit_lo(0);
    SYSTIMER->TARGET2_HI = SYSTIMER->UNIT0_VALUE_HI;
    SYSTIMER->TARGET2_LO = now + 16000;         /* 1 ms */
    SYSTIMER->COMP2_LOAD = 1;
    SYSTIMER->CONF |= 1u << 22;
    SYSTIMER->INT_ENA = 4;
    INTERRUPT_CORE0->SYSTIMER_TARGET2_INT_MAP = 7;
    INTERRUPT_CORE0->CPU_INT_PRI_7 = 1;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 1u << 7;
    CSRW(0x7e0, 16);                            /* IDLE */
    CSRW(0x7e2, 0);
    __asm__ volatile("wfi");
    uint32_t idle = CSRR(0x7e2);
    M("wfi-woke", (unit_lo(0) - now) >= 16000);
    M("wfi-eip", INTERRUPT_CORE0->CPU_INT_EIP_STATUS);
    M("wfi-idle-k-cycles", idle / 1000);        /* ~1 ms at 20 MHz */
    /* CYCLE does not count WFI's: another 1 ms */
    SYSTIMER->INT_CLR = 4;
    now = unit_lo(0);
    SYSTIMER->TARGET2_HI = SYSTIMER->UNIT0_VALUE_HI;
    SYSTIMER->TARGET2_LO = now + 16000;
    SYSTIMER->COMP2_LOAD = 1;
    CSRW(0x7e0, 1);
    CSRW(0x7e2, 0);
    __asm__ volatile("wfi");
    M("wfi-cycle-k-cycles", CSRR(0x7e2) / 1000);
    SYSTIMER->INT_CLR = 4;
    SYSTIMER->INT_ENA = 0;
    SYSTIMER->CONF &= ~(1u << 22);
    INTERRUPT_CORE0->SYSTIMER_TARGET2_INT_MAP = 0;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 0;

    /* GPIO and IO_MUX */
    M("gpio-in", GPIO->IN);
    for (int i = 0; i < 22; i++) {
        char nm[] = "iomux-gpio00";
        nm[10] = (char)('0' + i / 10);
        nm[11] = (char)('0' + i % 10);
        M(nm, (&IO_MUX->GPIO0)[i]);
    }
    M("gpio-func10-out-sel", GPIO->FUNC10_OUT_SEL_CFG);
    GPIO->ENABLE_W1TS = 1u << 10 | 1u << 4 | 1u << 0;
    GPIO->OUT_W1TS = 1u << 10;
    M("gpio10-high", GPIO->IN >> 10 & 1);
    GPIO->OUT_W1TC = 1u << 10;
    M("gpio10-low", GPIO->IN >> 10 & 1);
    GPIO->FUNC10_OUT_SEL_CFG = 0x180;           /* INV_SEL */
    M("gpio10-inverted", GPIO->IN >> 10 & 1);
    GPIO->FUNC10_OUT_SEL_CFG = 0x80;
    GPIO->PIN10 = 1u << 2;                      /* PAD_DRIVER: open drain */
    GPIO->OUT_W1TS = 1u << 10;
    M("gpio10-od-released", GPIO->IN >> 10 & 1);
    IO_MUX->GPIO10 |= 1u << 8;                  /* FUN_WPU */
    M("gpio10-od-pulled-up", GPIO->IN >> 10 & 1);
    GPIO->OUT_W1TC = 1u << 10;
    M("gpio10-od-low", GPIO->IN >> 10 & 1);
    GPIO->PIN10 = 0;
    GPIO->OUT_W1TS = 1u << 4 | 1u << 0;
    M("gpio4-mtms", GPIO->IN >> 4 & 1);         /* function 0 is MTMS */
    IO_MUX->GPIO4 = (IO_MUX->GPIO4 & ~(7u << 12)) | 1u << 12;
    M("gpio4-gpio", GPIO->IN >> 4 & 1);
    M("gpio0-no-ie", GPIO->IN & 1);
    IO_MUX->GPIO0 |= 1u << 9;                   /* FUN_IE */
    M("gpio0-ie", GPIO->IN & 1);
    M("gpio-enable", GPIO->ENABLE);
    GPIO->OUT_W1TC = 0xffffffffu;
    GPIO->ENABLE_W1TC = 0xffffffffu;
    GPIO->OUT_W1TS = 0xffffffffu;
    M("gpio-out-all", GPIO->OUT);
    GPIO->OUT_W1TC = 0xffffffffu;
    M("gpio-out-none", GPIO->OUT);

    /* 40 MHz, then the PLL's 80 and 160 MHz (on the chip the BBPLL is
     * powered and calibrated first, as ESP-IDF's rtc_clk does; EmbSim does
     * not model that) */
    SYSTEM->SYSCLK_CONF = 0;                    /* XTAL_CLK / 1 */
    M("systimer-k-40mhz", clock_check());
    SYSTEM->CPU_PER_CONF = 0xc;                 /* CPUPERIOD_SEL 0: 80 MHz */
    SYSTEM->SYSCLK_CONF = 1u << 10;             /* SOC_CLK_SEL: PLL_CLK */
    M("systimer-k-80mhz", clock_check());
    SYSTEM->CPU_PER_CONF = 0xd;                 /* CPUPERIOD_SEL 1: 160 MHz */
    M("systimer-k-160mhz", clock_check());

    /* the LED, blinked by comparator 0 in period mode, every 1 ms */
    GPIO->ENABLE_W1TS = LED;
    SYSTIMER->TARGET0_CONF = 1u << 30 | 16000;  /* period mode, 1 ms */
    SYSTIMER->COMP0_LOAD = 1;
    SYSTIMER->CONF |= 1u << 24;
    SYSTIMER->INT_ENA = 1;
    INTERRUPT_CORE0->SYSTIMER_TARGET0_INT_MAP = 1;
    INTERRUPT_CORE0->CPU_INT_PRI_1 = 3;
    INTERRUPT_CORE0->CPU_INT_ENABLE = 1u << 1;
    /* a pulse of 2 us first: not an SK6812 bit */
    uint32_t t = cycles();
    GPIO->OUT_W1TS = LED;
    while (cycles() - t < 320u)
        ;
    GPIO->OUT_W1TC = LED;
    __asm__ volatile("csrs mstatus, 8");
    while (ticks < 6)
        __asm__ volatile("wfi");
    __asm__ volatile("csrc mstatus, 8");
    M("ticks", ticks);
    line("t", "tick1-us", tick1 / 16);

    /* UART0's receiver: the run's --input */
    uint32_t cnt = UART0->STATUS & 0x3ff;
    line("rx", "count", cnt);
    UART0->CONF1 = (UART0->CONF1 & ~0x1ffu) | 2;        /* RXFIFO_FULL_THRHD */
    UART0->INT_CLR = 1;
    line("rx", "full-over-2", UART0->INT_RAW & 1);
    UART0->CONF1 = (UART0->CONF1 & ~0x1ffu) | 3;
    UART0->INT_CLR = 1;
    line("rx", "full-over-3", UART0->INT_RAW & 1);
    uint32_t got = 0;
    for (uint32_t i = 0; i < cnt && i < 2; i++)
        got = got << 8 | (UART0->FIFO & 0xff);
    line("rx", "bytes", got);
    line("rx", "left", UART0->STATUS & 0x3ff);
    UART0->CONF0 |= 1u << 17;                   /* RXFIFO_RST */
    UART0->CONF0 &= ~(1u << 17);
    line("rx", "reset", UART0->STATUS & 0x3ff);

    puts_("end\n");
    RTC_CNTL->OPTIONS0 = 1u << 31;              /* SW_SYS_RST */
    puts_("not reset\n");
    for (;;)
        ;
}
