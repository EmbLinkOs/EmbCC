/* prof.c -- the program tests/golden/embsim-profile.sh holds EmbSim's
 * --profile to: calls of known number along known paths, on each core
 * in its harness.
 *
 *   main -> a (3 times) -> leaf          a call that is not the last thing
 *   main -> b (once) -> leaf (twice)
 *   main -> rec -> rec -> rec -> rec -> rec   recursion, five frames deep
 *   main -> leaf, through a pointer      BLX, JALR, ICALL
 *   main -> spin, interrupted by tick    a timer's interrupt: SysTick on
 *                                        the Cortex-M, the CLINT's mtimecmp
 *                                        on RISC-V, Timer/Counter1 on AVR
 *
 * It prints the sum the calls return (37) and the ticks taken. */
void putn(long v);

volatile int sink;
volatile int ticks;

__attribute__((noinline)) int leaf(int x)
{
    sink = x;
    return x + 1;
}

__attribute__((noinline)) int a(int x)
{
    return leaf(x) * 2;
}

__attribute__((noinline)) int b(int x)
{
    int s = leaf(x);
    s += leaf(s);
    return s;
}

__attribute__((noinline)) int rec(int n)
{
    if (n == 0)
        return 0;
    return rec(n - 1) + 1;
}

__attribute__((noinline)) void spin(void)
{
    for (int i = 0; i < 10; i++)
        sink = i;
}

static int (*volatile fp)(int) = leaf;

#if defined(__arm__) || defined(__thumb__)
/* SysTick, through a vector table in RAM (the harness's has two entries) */
void tick(void)
{
    ticks++;
}

static void (*vtab[16])(void) __attribute__((aligned(128)));

static void timer_start(void)
{
    vtab[15] = tick;
    *(void (***)(void))0xE000ED08u = vtab;          /* VTOR */
    *(volatile unsigned *)0xE000E014u = 499;        /* RVR */
    *(volatile unsigned *)0xE000E018u = 0;          /* CVR */
    *(volatile unsigned *)0xE000E010u = 7;          /* CSR: on, TICKINT */
}

static void timer_stop(void)
{
    *(volatile unsigned *)0xE000E010u = 0;
}
#elif defined(__riscv)
#define MTIMECMP ((volatile unsigned *)0x2004000u)
#define MTIME ((volatile unsigned *)0x200bff8u)

__attribute__((interrupt("machine"), aligned(4))) void tick(void)
{
    unsigned t = MTIME[0] + 500;
    ticks++;
    MTIMECMP[1] = 0xffffffffu;
    MTIMECMP[0] = t;
    MTIMECMP[1] = 0;
}

static void timer_start(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(tick));
    MTIMECMP[1] = 0xffffffffu;
    MTIMECMP[0] = MTIME[0] + 500;
    MTIMECMP[1] = 0;
    __asm__ volatile("csrs mie, %0" : : "r"(0x80));     /* MTIE */
    __asm__ volatile("csrs mstatus, %0" : : "r"(8));    /* MIE */
}

static void timer_stop(void)
{
    __asm__ volatile("csrc mstatus, %0" : : "r"(8));
    __asm__ volatile("csrc mie, %0" : : "r"(0x80));
}
#else
/* Timer/Counter1's compare match A, CTC on OCR1A, at the core's clock */
__attribute__((signal)) void __vector_11(void)
{
    ticks++;
}

static void timer_start(void)
{
    *(volatile unsigned char *)0x89 = 1;            /* OCR1AH: 500 */
    *(volatile unsigned char *)0x88 = 0xf4;         /* OCR1AL */
    *(volatile unsigned char *)0x80 = 0;            /* TCCR1A */
    *(volatile unsigned char *)0x6f = 2;            /* TIMSK1: OCIE1A */
    *(volatile unsigned char *)0x81 = 9;            /* TCCR1B: CTC, /1 */
    __asm__ volatile("sei");
}

static void timer_stop(void)
{
    __asm__ volatile("cli");
    *(volatile unsigned char *)0x81 = 0;
    *(volatile unsigned char *)0x6f = 0;
}
#endif

int main(void)
{
    int s = 0;
    for (int i = 0; i < 3; i++)
        s += a(i);
    s += b(5);
    s += rec(4);
    s += fp(7);
    timer_start();
    while (ticks < 3)
        spin();
    timer_stop();
    putn(s);
    putn(ticks);
    return 0;
}
