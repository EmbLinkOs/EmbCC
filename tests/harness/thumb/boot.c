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
    /* Nothing to return to: the reset handler IS the bottom of the
     * call stack. The harness stops the machine on a timeout, having
     * already read the output. */
    for (;;)
        ;
}
