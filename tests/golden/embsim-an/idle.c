/* idle.c -- the programs tests/golden/embsim-idle.sh ends in a loop that
 * jumps to itself, once nothing can interrupt it any more. Each turns a
 * timer's interrupt on, takes a few ticks, then takes away the last
 * thing that could interrupt the loop -- in a different way each -- and
 * enters it: the run must end at the loop's first turn.
 *
 *   Cortex-M3 (harness)  1  SysTick off, by a write to its CSR
 *   STM32F405            2  TIM2 in one-pulse mode: the timer stops
 *                           itself at its update, and the handler
 *                           touches no device
 *   RV32 (harness)       1  MTIE cleared in mie, MSIE left on (so mie is
 *                           not 0): a CSR, no device touched
 *   AVR (harness)        1  OCIE1A cleared in TIMSK1
 * And for a debugger to change (MODE 3): a timer whose next tick is far
 * off -- SysTick's longest period; mtimecmp 2^31 cycles on -- and the
 * loop entered at once, for gdb to stop, take the interrupt away from
 * (SysTick's TICKINT, a write to memory; mie, a CSR), and resume. */
volatile unsigned ticks;

#if defined(__arm__) || defined(__thumb__)
static void (*vtab[16 + 82])(void) __attribute__((aligned(512)));

/* touches no device: in mode 2 nothing but the update's time passing
 * says the timer has stopped */
void tick(void)
{
    ticks++;
}

#if MODE == 2
extern unsigned __data_load, __data_start, __data_end, __bss_start, __bss_end;
void reset(void);
__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)0x20010000u, (void *)reset };

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end;)
        *d++ = 0;
    vtab[16 + 28] = tick;
    *(void (***)(void))0xE000ED08u = vtab;
    *(volatile unsigned *)0x40023840u |= 1;                 /* RCC: TIM2EN */
    *(volatile unsigned *)0x4000002Cu = 499;                /* ARR */
    *(volatile unsigned *)0x4000000Cu = 1;                  /* DIER: UIE */
    *(volatile unsigned *)0xE000E100u = 1u << 28;           /* NVIC: TIM2 */
    *(volatile unsigned *)0x40000000u = 9;                  /* CR1: CEN, OPM */
    for (;;)
        ;
}
#elif MODE == 3
int main(void)
{
    vtab[15] = tick;
    *(void (***)(void))0xE000ED08u = vtab;
    *(volatile unsigned *)0xE000E014u = 0xffffff;
    *(volatile unsigned *)0xE000E018u = 0;
    *(volatile unsigned *)0xE000E010u = 7;
    for (;;)
        ;
}
#else
int main(void)
{
    vtab[15] = tick;
    *(void (***)(void))0xE000ED08u = vtab;
    *(volatile unsigned *)0xE000E014u = 499;
    *(volatile unsigned *)0xE000E018u = 0;
    *(volatile unsigned *)0xE000E010u = 7;
    while (ticks < 3)
        ;
    *(volatile unsigned *)0xE000E010u = 0;                  /* SysTick off */
    for (;;)
        ;
}
#endif
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

int main(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(tick));
    MTIMECMP[1] = 0xffffffffu;
#if MODE == 3
    MTIMECMP[0] = MTIME[0] + 0x80000000u;
#else
    MTIMECMP[0] = MTIME[0] + 500;
#endif
    MTIMECMP[1] = 0;
    __asm__ volatile("csrs mie, %0" : : "r"(0x88));         /* MTIE, MSIE */
    __asm__ volatile("csrs mstatus, %0" : : "r"(8));
#if MODE != 3
    while (ticks < 3)
        ;
    __asm__ volatile("csrc mie, %0" : : "r"(0x80));         /* MTIE off */
#endif
    for (;;)
        __asm__ volatile("");
}
#else
__attribute__((signal)) void __vector_11(void)
{
    ticks++;
}

int main(void)
{
    *(volatile unsigned char *)0x89 = 1;                    /* OCR1A: 500 */
    *(volatile unsigned char *)0x88 = 0xf4;
    *(volatile unsigned char *)0x6f = 2;                    /* TIMSK1: OCIE1A */
    *(volatile unsigned char *)0x81 = 9;                    /* CTC, /1 */
    __asm__ volatile("sei");
    while (ticks < 3)
        ;
    *(volatile unsigned char *)0x6f = 0;                    /* OCIE1A off */
    for (;;)
        __asm__ volatile("");
}
#endif
