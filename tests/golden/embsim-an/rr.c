/* rr.c -- the programs tests/golden/embsim-replay.sh records and replays,
 * in the harness of their core. Each takes input whose arrival the run
 * cannot know in advance, and prints when (in its own time) it came.
 *
 *   Cortex-M3 (lm3s6965evb)
 *     1  the PL011's receive interrupt and SysTick: main sleeps in WFI;
 *        each byte is printed with the ticks counted when it came; 'q'
 *        ends the run
 *     2  semihosting's SYS_READC: three bytes of the host's stdin
 *     4  a debugger's writes: add1's argument, which gdb changes in r0,
 *        and `bonus`, which it sets in memory, make the result
 *     5  mode 1 without SysTick: nothing but a byte can wake the WFI, so
 *        the run waits for the host
 *   Cortex-M4 (mps2-an386)
 *     3  the CMSDK UART's receiver, polled, echoed to its transmitter
 *   RV32 (virt)
 *     1  the NS16550A's receiver, polled (no PLIC), with mcycle's count
 *   AVR (uno)
 *     1  USART0's receive-complete interrupt, with Timer/Counter1 ticks */
void writec(int c);
void puts_(const char *s);
void putn(long v);

volatile unsigned ticks;
volatile int got = -1;
volatile int bonus;

#if defined(__arm__) || defined(__thumb__)
static void semi(unsigned op, const void *arg)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(arg) : "r0", "r1", "memory");
}

static void finish(void)
{
    static const unsigned blk[2] = { 0x20026u, 0 };
    semi(0x20, blk);
}

void systick(void)
{
    ticks++;
}

void uart0(void)
{
    got = (int)(*(volatile unsigned *)0x4000C000u & 0xff);   /* DR: takes it */
}

static void (*vtab[32])(void) __attribute__((aligned(128)));

__attribute__((noinline)) int add1(int x)
{
    return x + 1;
}

static int readc(void)
{
    int r;
    __asm__ volatile("mov r0, #7\n\tmov r1, #0\n\tbkpt #0xab\n\tmov %0, r0"
                     : "=r"(r) : : "r0", "r1", "memory");
    return r;
}

int main(void)
{
#if MODE == 1 || MODE == 5
    vtab[15] = systick;
    vtab[16 + 5] = uart0;
    *(void (***)(void))0xE000ED08u = vtab;                  /* VTOR */
    *(volatile unsigned *)0xE000E100u = 1u << 5;            /* NVIC: UART0 */
    *(volatile unsigned *)0x4000C038u = 0x10;               /* IMSC: RXIM */
#if MODE == 1
    *(volatile unsigned *)0xE000E014u = 9999;               /* SysTick */
    *(volatile unsigned *)0xE000E018u = 0;
    *(volatile unsigned *)0xE000E010u = 7;
#endif
    for (;;) {
        __asm__ volatile("wfi");
        int c = got;
        if (c < 0)
            continue;
        got = -1;
        puts_("got ");
        writec(c);
        puts_(" at tick ");
        putn((long)ticks);
        puts_("\n");
        if (c == 'q')
            break;
    }
#elif MODE == 2
    for (int i = 0; i < 3; i++) {
        int c = readc();
        puts_("read ");
        putn(c);
        puts_("\n");
    }
#elif MODE == 3
    volatile unsigned *u = (volatile unsigned *)0x40004000u;
    u[2] = 3;                                               /* CTRL: TX, RX */
    for (;;) {
        while (!(u[1] & 2))                                 /* STATE: RX full */
            ticks++;
        int c = (int)(u[0] & 0xff);
        u[0] = (unsigned)c;
        if (c == 'q')
            break;
    }
    u[0] = '\n';
    puts_("polls ");
    putn((long)ticks);
    puts_("\n");
#elif MODE == 4
    int r = add1(1) + bonus;
    puts_("result ");
    putn(r);
    puts_("\n");
#endif
    finish();
    return 0;
}
#elif defined(__riscv)
int main(void)
{
    volatile unsigned char *u = (volatile unsigned char *)0x10000000u;
    for (;;) {
        while (!(u[5] & 1))                                 /* LSR: DR */
            ;
        int c = u[0];
        unsigned long cyc;
        __asm__ volatile("csrr %0, mcycle" : "=r"(cyc));
        puts_("got ");
        writec(c);
        puts_(" at cycle ");
        putn((long)cyc);
        puts_("\n");
        if (c == 'q')
            break;
    }
    return 0;
}
#else
__attribute__((signal)) void __vector_18(void)
{
    got = *(volatile unsigned char *)0xc6;                  /* UDR0: takes it */
}

__attribute__((signal)) void __vector_11(void)
{
    ticks++;
}

int main(void)
{
    puts_("ready\n");                  /* the harness turns the UART on, TX alone */
    *(volatile unsigned char *)0xc1 |= 0x90;                /* UCSR0B: RXEN0, RXCIE0 */
    *(volatile unsigned char *)0x89 = 0x27;                 /* OCR1A: 10000 */
    *(volatile unsigned char *)0x88 = 0x10;
    *(volatile unsigned char *)0x6f = 2;                    /* TIMSK1: OCIE1A */
    *(volatile unsigned char *)0x81 = 9;                    /* CTC, /1 */
    __asm__ volatile("sei");
    for (;;) {
        *(volatile unsigned char *)0x53 = 1;                /* SMCR: SE, idle */
        __asm__ volatile("sleep");
        int c = got;
        if (c < 0)
            continue;
        got = -1;
        puts_("got ");
        writec(c);
        puts_(" at tick ");
        putn((long)ticks);
        puts_("\n");
        if (c == 'q')
            break;
    }
    __asm__ volatile("cli");
    return 0;
}
#endif
