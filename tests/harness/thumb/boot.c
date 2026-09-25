/* The ARMv7-M test harness: a vector table and a reset handler, in C.
 *
 * A Cortex-M needs no assembler to start. The processor itself fetches
 * the initial stack pointer from the first word of the image and the
 * reset address from the second, so the "startup code" every other
 * architecture writes in assembly is, here, an array of two pointers
 * and an ordinary function.
 *
 * The bracket symbols come from embld (-Tdata): .data is STORED after
 * the text in flash and ADDRESSED in RAM, so the first thing to run has
 * to copy it across and zero .bss. That is the whole of a C runtime on
 * this machine.
 */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;

int main(void);
void reset(void);

/* QEMU's lm3s6965evb has 64 KiB of SRAM at 0x20000000; the stack starts
 * at the top of it and grows down. */
#define SRAM_TOP 0x20010000u

__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)SRAM_TOP, (void *)reset };

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    main();
    /* Stop the machine.
     *
     * A Cortex-M has no way to say "exit" — the reset handler IS the
     * bottom of the call stack. Semihosting would do it, but that takes
     * a `bkpt`, which is assembly this compiler cannot emit yet. So the
     * image faults ON PURPOSE: this vector table has two entries and no
     * HardFault handler, so the fault escalates, the processor locks
     * up, and QEMU stops and returns. The alternative is spinning until
     * the harness's timeout fires, which costs the suite the full
     * timeout for every image that WORKED.
     *
     * Everything the test reads has already been printed by the time
     * this runs, and run.sh judges the output rather than the status.
     *
     * __builtin_trap() rather than a write to some address that ought
     * to be invalid: on a Cortex-M almost every address answers,
     * including zero, where the vector table is. `udf #0` is
     * architecturally undefined and always faults. */
    __builtin_trap();
    for (;;)
        ;
}
