/* The firmware tests/golden/embsim-gdb.sh debugs, on QEMU's stub and on
 * EmbSim's GDB server. Built with -g -O0 for the M3 (integer `real`) and
 * for the M4 with its FPU (float `real`), so neither needs lib/rt; and
 * for RISC-V, RV32 soft-float (integer) and RV64 with D (float), where
 * reset is entered from embld's -Tstack stub and the test device ends
 * the run.
 *
 * main's result is the exit status (semihosting's SYS_EXIT_EXTENDED, or
 * the test device's code): 71 with an integer `real` and 65 with a float
 * one when nothing is changed, 92 and 76 when the debugger sets
 * origin.x to 10 -- so the status says whether a write the debugger
 * made reached the program. VARIANT changes it again (81
 * for 5 on the M3), for the image `load` must replace. `spin` and `idle`
 * hold the program in a loop, and in a WFI nothing can end, until a
 * debugger that has interrupted it clears them (SPIN and IDLE set them
 * from the start, for a debugger that attaches to a running image);
 * `trap` locks the core up (an undefined instruction, and no fault
 * handler). */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
int main(void);
void reset(void);

#ifndef __riscv
__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)0x20010000u, (void *)reset };
#endif

#if defined(__ARM_FP) || defined(__riscv_flen)
typedef float real;
#define SCALE 1.5f
#define QUARTER 0.25f
#else
typedef int real;
#define SCALE 3
#define QUARTER 1
#endif

#ifndef VARIANT
#define VARIANT 0
#endif
#ifndef SPIN
#define SPIN 0
#endif
#ifndef IDLE
#define IDLE 0
#endif

struct pt { int x; int y; };

volatile int counter;
volatile int trap;
volatile unsigned ticks;
int table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };      /* first in .data */
struct pt origin = { 3, -4 };
real scale = SCALE;
volatile int spin = SPIN, idle = IDLE;

#ifndef __riscv
static unsigned blk[2];

static void semi(unsigned op, const void *a)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(a) : "r0", "r1", "memory");
}
#endif

int compute(int a, int b)
{
    int t = a * b;
    counter += t;
    return t + 1;
}

real fmix(real x, real y)
{
    real r = x * scale + y;
    return r;
}

int sum_table(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += table[i];                  /* SUM-LINE */
    return s;
}

int main(void)
{
    int acc = VARIANT;
    for (int i = 0; i < 3; i++)
        acc += compute(i, i + 2);
    table[2] = acc;
    acc += sum_table(8);
    acc += (int)fmix((real)origin.x, QUARTER);
    while (spin) ticks++;
    while (idle) __asm__ volatile("wfi");
    if (trap)
        __builtin_trap();
    return acc;
}

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
#ifdef __ARM_FP
    *(volatile unsigned *)0xE000ED88u |= 0xFu << 20;    /* CPACR: the FPU */
#endif
#ifdef __riscv_flen
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x2000u));  /* FS: the FPU */
#endif
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end;)
        *d++ = 0;
#ifdef __riscv
    *(volatile unsigned *)0x100000u = ((unsigned)main() & 0x7f) << 16 | 0x3333u;
#else
    blk[0] = 0x20026u;
    blk[1] = (unsigned)main() & 0x7f;
    semi(0x20, blk);
#endif
    for (;;)
        ;
}
