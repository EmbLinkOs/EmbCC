/* The ARMv6-M test harness: QEMU's BBC micro:bit, an nRF51822 -- a
 * Cortex-M0 with 256 KiB of flash at 0 and 16 KiB of SRAM at 0x20000000.
 *
 * As on the Cortex-M3 board (../thumb/boot.c) the startup is C: the core
 * fetches its stack pointer and reset address from the first two words of
 * the image. This table has the fault vectors as well, so a fault -- an
 * undefined instruction, an unaligned access -- is reported as one
 * (io.c's harness_fault, exit status 125) instead of locking the core up.
 *
 * main's return value is the program's exit status, reported by _exit()
 * as `==EXIT n==` on the UART: that is what run.sh reads.
 */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
/* The static constructors -- a C++ namespace-scope object's, a C
 * __attribute__((constructor)) -- which embld gathers into .init_array
 * between these two symbols. */
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void reset(void);
void harness_fault(void);
void uart_init(void);
void _exit(int status);

/* 16 KiB, the part's own; the exec corpus raises it to 64 KiB, as the
 * Cortex-M3 board has (run.sh: EMBCC_M0_SRAM, and -DSRAM_TOP here). */
#ifndef SRAM_TOP
#define SRAM_TOP 0x20004000u
#endif

__attribute__((section(".vectors"), used))
void *const vectors[4] = {
    (void *)SRAM_TOP, (void *)reset, (void *)harness_fault,
    (void *)harness_fault
};

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    uart_init();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    _exit(main());
}
