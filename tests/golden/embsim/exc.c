/* The exception model, run on QEMU and on EmbSim: tests/golden/embsim.sh
 * requires the same output from both. The exec corpus never takes an
 * interrupt, so this is what checks the rest of the core:
 *   - SVC and PendSV;
 *   - NVIC interrupts pended through ISPR and STIR, and nested by
 *     priority;
 *   - PRIMASK and BASEPRI;
 *   - a divide-by-zero UsageFault, handled, then escalated to HardFault
 *     when its handler is disabled;
 *   - SysTick waking WFI;
 *   - a thread on the PSP taking an exception;
 *   - with the FPU, a float live across an interrupt that uses floats
 *     itself (the extended frame).
 * Output and the exit go through semihosting, so the one image runs on
 * any of the boards. Nothing printed depends on time: QEMU's SysTick
 * follows the host's clock. */
#define REG(a) (*(volatile unsigned *)(a))
#define ICSR   REG(0xE000ED04u)
#define SHCSR  REG(0xE000ED24u)
#define CFSR   REG(0xE000ED28u)
#define HFSR   REG(0xE000ED2Cu)
#define CCR    REG(0xE000ED14u)
#define CPACR  REG(0xE000ED88u)
#define SHPR3  REG(0xE000ED20u)
#define ISER0  REG(0xE000E100u)
#define ISPR0  REG(0xE000E200u)
#define STIR   REG(0xE000EF00u)
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)

extern unsigned __data_load, __data_start, __data_end, __bss_start, __bss_end;
void reset(void);
void fault(void);
void hard_fault(void);
void usage_fault(void);
void svc_handler(void);
void pendsv(void);
void systick(void);
void irq_low(void);
void irq_high(void);

#define STACK_TOP 0x20004000u   /* the micro:bit's 16 KiB is the least */

__attribute__((section(".vectors"), used))
void (*const vectors[16 + 8])(void) = {
    (void (*)(void))STACK_TOP, reset, fault, hard_fault,
    fault, fault, usage_fault, 0, 0, 0, 0, svc_handler, fault, 0, pendsv,
    systick,
    fault, fault, fault, irq_low, irq_high, fault, fault, fault,
};

static void semi(unsigned op, const void *arg)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(arg) : "r0", "r1", "memory");
}

static void say(const char *s) { semi(4, s); }

static void sayn(unsigned v)
{
    char b[12];
    int n = 11;
    b[n] = 0;
    do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v);
    say(b + n);
}

static volatile unsigned svcs, pendsvs, ticks, faults, hards;
static volatile char order[16];
static volatile int norder;

void fault(void)
{
    say("unexpected exception\n");
    for (;;)
        ;
}

void svc_handler(void) { svcs++; }
void pendsv(void) { pendsvs++; order[norder++] = 'P'; }
void systick(void) { ticks++; }

/* priority 0x80: pends the 0x40 one, which preempts it at once */
void irq_low(void)
{
    order[norder++] = 'l';
    ISPR0 = 1u << 4;
    order[norder++] = 'L';
}

#if defined(__ARM_FP)
static volatile float hf = 0.25f;
#endif
void irq_high(void)
{
    order[norder++] = 'H';
#if defined(__ARM_FP)
    /* floats in the handler: s0-s15 of the thread must survive this */
    hf = hf * 3.0f + 1.0f;
#endif
}

/* ARMv6-M has no STIR, and its NVIC priorities are written by word */
static void trigger(int n)
{
#if defined(__ARM_ARCH_6M__)
    ISPR0 = 1u << n;
#else
    STIR = (unsigned)n;
#endif
}

static void set_prio(int n, unsigned p)
{
    volatile unsigned *w = (volatile unsigned *)(0xE000E400u + (unsigned)(n & ~3));
    unsigned sh = 8u * (unsigned)(n & 3);
    *w = (*w & ~(0xffu << sh)) | p << sh;
}

#if !defined(__ARM_ARCH_6M__)
/* a divide by zero with the trap on: count it and turn the trap off, so
 * the sdiv runs again on return and gives 0 */
void usage_fault(void)
{
    if (CFSR & (1u << 25))
        faults++;
    CFSR = CFSR;
    CCR &= ~0x10u;
}
#else
void usage_fault(void) { fault(); }
#endif

void hard_fault(void)
{
#if !defined(__ARM_ARCH_6M__)
    if ((HFSR & (1u << 30)) && (CFSR & (1u << 25)))
        hards++;
    HFSR = HFSR;
    CFSR = CFSR;
    CCR &= ~0x10u;
#else
    fault();
#endif
}

static volatile int zero, sink;

static int on_psp(void)
{
    unsigned c;
    __asm__ volatile("mrs %0, control" : "=r"(c));
    return (c & 2) != 0;
}

int main(void)
{
    /* SVC from the main stack */
    __asm__ volatile("svc #1");
    __asm__ volatile("svc #2");
    say("svc "); sayn(svcs); say("\n");

    /* PendSV at the lowest priority runs as soon as it is pended */
    SHPR3 = (SHPR3 & 0x00ffffffu) | 0xc0000000u;
    ICSR = 1u << 28;
    say("pendsv "); sayn(pendsvs); say("\n");

    /* IRQ 3 (low) and 4 (high): nesting, then masking */
    set_prio(3, 0x80);
    set_prio(4, 0x40);
    ISER0 = (1u << 3) | (1u << 4);
    ISPR0 = 1u << 3;
    __asm__ volatile("cpsid i");
    trigger(4);
    order[norder++] = '-';
    __asm__ volatile("cpsie i");
    __asm__ volatile("isb");
#if !defined(__ARM_ARCH_6M__)
    {
        unsigned bp = 0x40;
        __asm__ volatile("msr basepri, %0" : : "r"(bp));
        trigger(4);                 /* priority 0x40 is masked */
        order[norder++] = '=';
        bp = 0;
        __asm__ volatile("msr basepri, %0" : : "r"(bp));
        __asm__ volatile("isb");
    }
#endif
    order[norder] = 0;
    say("irq "); say((const char *)order); say("\n");

#if !defined(__ARM_ARCH_6M__)
    /* a UsageFault, handled; then with its handler off, a HardFault */
    SHCSR |= 1u << 18;
    CCR |= 0x10;
    int q = 7 / zero;
    SHCSR &= ~(1u << 18);
    CCR |= 0x10;
    q += 9 / zero;
    sink = q;
    /* the counts, not the quotient: QEMU (11, mps2-an386) resumes a
     * trapped sdiv with instructions before it in its translation block
     * undone (a `movs r6, #9` and the load of the divisor), so its
     * quotient is 0x40000 / 0x210 where the architecture's, and
     * EmbSim's, is 0 */
    say("faults "); sayn(faults); say(" "); sayn(hards); say("\n");
#endif

    /* SysTick waking WFI three times */
    SYST_RVR = 4000;
    SYST_CVR = 0;
    SYST_CSR = 7;
    while (ticks < 3)
        __asm__ volatile("wfi");
    SYST_CSR = 0;
    say("ticks done\n");

    /* a thread on the PSP takes an SVC and comes back to the PSP. The
     * PSP starts where the MSP is, so this function's frame stays
     * where its code expects it across the switch. */
    {
        unsigned top, ctl = 2;
        __asm__ volatile("mrs %0, msp" : "=r"(top));
        __asm__ volatile("msr psp, %0" : : "r"(top));
        __asm__ volatile("msr control, %0" : : "r"(ctl));
        __asm__ volatile("isb");
        int before = on_psp();
        __asm__ volatile("svc #3");
        int after = on_psp();
        ctl = 0;
        __asm__ volatile("msr control, %0" : : "r"(ctl));
        __asm__ volatile("isb");
        say("psp "); sayn((unsigned)before); sayn((unsigned)after);
        sayn(svcs); say("\n");
    }

#if defined(__ARM_FP)
    /* a float live across an interrupt whose handler uses floats */
    {
        volatile float x = 1.5f;
        float y = x * 3.0f;
        norder = 0;
        trigger(4);
        __asm__ volatile("isb");
        y = y + x;
        say("fp "); sayn((unsigned)(y * 100.0f)); say("\n");
    }
#endif
    return 0;
}

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
#if defined(__ARM_FP)
    CPACR |= (3u << 20) | (3u << 22);
    __asm__ volatile("dsb");
    __asm__ volatile("isb");
#endif
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    static volatile unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = (unsigned)main();
    semi(0x20, (const void *)blk);
    for (;;)
        ;
}
