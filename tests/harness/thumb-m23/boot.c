/* The ARMv8-M Baseline (Cortex-M23) test harness.
 *
 * QEMU has no Cortex-M23 model, so Baseline code runs on the Cortex-M33 of
 * QEMU's mps2-an505 (the board tests/harness/thumb-m33 uses). The M33
 * executes every Baseline instruction -- Baseline is a subset of Mainline --
 * so what this harness can NOT show is an instruction Baseline lacks: that is
 * the job of the encoding scan in tests/golden/thumbv8mbase-exec.sh, which
 * disassembles every object for thumbv8m.base. What it can show is what the
 * code computes.
 *
 * One property of the M23 is made true here as well: a Cortex-M23 faults on
 * every unaligned word or halfword access, where the M33 performs them unless
 * CCR.UNALIGN_TRP is set. So it is set, first, and an unaligned access the
 * backend makes faults here as it would on the part.
 *
 * As in tests/harness/thumb-m0, a fault is reported (io.c's harness_fault,
 * exit status 125) and main's return value is the program's exit status,
 * printed as `==EXIT n==` for run.sh.
 *
 * The image lives at the board's SECURE alias, 0x10000000, because the core
 * leaves reset in the Secure state (see ../thumb-m33/link.sh).
 */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;

int main(void);
void reset(void);
void harness_fault(void);
void _exit(int status);

#ifndef SRAM_TOP
#define SRAM_TOP 0x10200000u
#endif

/* The Configuration and Control Register; bit 3 is UNALIGN_TRP. */
#define CCR (*(volatile unsigned *)0xE000ED14u)

__attribute__((section(".vectors"), used))
void *const vectors[4] = {
    (void *)SRAM_TOP, (void *)reset, (void *)harness_fault,
    (void *)harness_fault
};

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    CCR |= 1u << 3;
    __asm__ volatile("dsb");
    __asm__ volatile("isb");
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    _exit(main());
}
