/* fault.c -- the faults tests/golden/embsim-fault.sh has EmbSim report,
 * in the harness of the Cortex-M3 or of RV32. Each MODE takes one, at
 * the end of main -> level1 -> level2 -> crash:
 *
 *   Cortex-M3, with a vector table in RAM (the harness's has two entries)
 *     1  a load from 0x30000000, where nothing is: a BusFault (enabled in
 *        SHCSR), whose handler prints and exits through semihosting
 *     2  a divide by zero with CCR.DIV_0_TRP: a UsageFault, which is not
 *        enabled, so it escalates to HardFault, which has no handler
 *     3  the load of mode 1 from the SysTick handler, interrupting spin:
 *        a HardFault, and the backtrace through the exception's frame
 *   RV32
 *     1  the load from 0x30000000: a load access fault, with mtvec 0
 *     2  a write to a CSR there is not: an illegal instruction, with a
 *        handler that ends the run through the test device
 * The comments in capitals mark the lines the report's backtrace must
 * name. */
void puts_(const char *s);

volatile unsigned zero;
volatile unsigned sink;

__attribute__((noinline)) unsigned crash(volatile unsigned *p)
{
#if MODE == 2 && !defined(__riscv)
    return 100 / zero;                                  /* DIV */
#elif MODE == 2
    __asm__ volatile("csrw 0x7ff, zero");               /* CSR */
    return 0;
#else
    return *p + 1;                                      /* LOAD */
#endif
}

__attribute__((noinline)) unsigned level2(unsigned x)
{
    unsigned r = crash((volatile unsigned *)0x30000000u) + x;  /* CALL2 */
    sink = r;
    return r;
}

__attribute__((noinline)) unsigned level1(unsigned x)
{
    unsigned r = level2(x + 1) * 2;                     /* CALL1 */
    sink = r;
    return r;
}

#ifndef __riscv
static void semi_exit(void)
{
    static const unsigned blk[2] = { 0x20026u, 0 };
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(0x20u), "r"(blk) : "r0", "r1", "memory");
}

void bus_fault(void)
{
    puts_("bus fault handled\n");
    semi_exit();
}

void tick(void)
{
    sink = level2(7);                                   /* TICK */
}

__attribute__((noinline)) void spin(void)
{
    for (;;)
        sink++;                                         /* SPIN */
}

static void (*vtab[16])(void) __attribute__((aligned(128)));

static void setup(void)
{
    vtab[5] = bus_fault;
    vtab[15] = tick;
    *(void (***)(void))0xE000ED08u = vtab;                  /* VTOR */
#if MODE == 1
    *(volatile unsigned *)0xE000ED24u |= 1u << 17;          /* SHCSR: BUSFAULTENA */
#elif MODE == 2
    *(volatile unsigned *)0xE000ED14u |= 1u << 4;           /* CCR: DIV_0_TRP */
#elif MODE == 3
    *(volatile unsigned *)0xE000E014u = 999;                /* SysTick */
    *(volatile unsigned *)0xE000E018u = 0;
    *(volatile unsigned *)0xE000E010u = 7;
    spin();
#endif
}
#else
__attribute__((interrupt("machine"), aligned(4))) void trap(void)
{
    puts_("illegal instruction handled\n");
    *(volatile unsigned *)0x100000u = 0x5555;               /* the test device: pass */
}

static void setup(void)
{
#if MODE == 2
    __asm__ volatile("csrw mtvec, %0" : : "r"(trap));
#endif
}
#endif

int main(void)
{
    setup();
    sink = level1(3);                                   /* CALL0 */
    puts_("not reached\n");
    return 0;
}
