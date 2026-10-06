/* The Cortex-M4F harness: like the ARMv7-M one, plus the FPU.
 *
 * Two things differ from tests/harness/thumb, and both are properties of
 * the PART rather than of the compiler:
 *
 *  - the FPU is off at reset. CPACR's CP10 and CP11 fields each have to
 *    be set to full access before any VFP instruction executes; without
 *    that the first `vldr` takes a UsageFault, which on this harness
 *    looks exactly like a hang. It is the single easiest thing to forget
 *    when moving code from an M3 to an M4F, so it is done here, first,
 *    before main can reach any floating point.
 *
 *  - QEMU's mps2-an386 is a different board: its UART is the CMSDK one
 *    at 0x40004000 rather than the lm3s6965's at 0x4000C000, and its
 *    RAM is elsewhere. See io.c and run.sh.
 *
 * This board is a Cortex-M4 WITH the FPU (FPv4-SP-D16), which is what
 * NUCLEO-F446RE is, so it is the machine that can actually run what the
 * hard-float work emits.
 */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;

int main(void);
void reset(void);

/* mps2-an386 has its SSRAM at 0x20000000, 4 MiB of it; the stack starts
 * at the top of the first 64 KiB and grows down, which matches what the
 * ARMv7-M harness does and keeps the two link scripts the same shape.
 * -DSRAM_TOP= moves it: big-copy.c in the exec corpus has 240 KiB of
 * arrays (exec-boards.sh). */
#ifndef SRAM_TOP
#define SRAM_TOP 0x20010000u
#endif

/* Coprocessor Access Control Register. CP10 is bits 21:20 and CP11 bits
 * 23:22; 0b11 in each is "full access". Both must be set, not just CP10:
 * the FPU is addressed as two coprocessors and half-enabling it faults
 * on the first instruction that uses the other half. */
#define CPACR (*(volatile unsigned *)0xE000ED88u)

__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)SRAM_TOP, (void *)reset };

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;

    /* Before anything else: a VFP instruction in the .data copy below
     * would fault, and so would one the compiler hoisted into it. */
    CPACR |= (3u << 20) | (3u << 22);
    /* The ARM ARM requires a synchronisation barrier after enabling the
     * FPU before the first instruction that uses it. Without them the
     * enable can still be in flight. */
    __asm__ volatile("dsb");
    __asm__ volatile("isb");

    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    main();
    for (;;)
        ;
}
