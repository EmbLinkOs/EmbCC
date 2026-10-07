/* The Cortex-M7 harness: the Cortex-M4F one on the part with a
 * DOUBLE-precision FPU (FPv5-D16), which is what -mfpu=fpv5-d16 emits for
 * -- vadd.f64 and the rest fault as undefined instructions on an M4F, so
 * the code it makes can only be run here.
 *
 * QEMU's mps2-an500 is the AN500 FPGA image, a Cortex-M7, and its
 * cortex-m7 model has the double-precision unit. Its memory map and UART
 * are the AN386's (see io.c), so this differs from tests/harness/thumb-m4f
 * in the board, in the stack, and in how a program's end is reported:
 *
 *  - the FPU is off at reset, as on every Cortex-M with one: CPACR's CP10
 *    and CP11 to full access, and the barriers, before anything that may
 *    be a VFP instruction. Leaving it out is a UsageFault at the first
 *    double, which on this harness looks exactly like a hang.
 *
 *  - the stack starts at the top of the 4 MiB SSRAM at 0x20000000, not at
 *    64 KiB: the exec corpus links lib/libc, whose bare-metal sbrk grows the
 *    heap from the end of .bss up toward the stack, and some of those
 *    programs allocate more than 64 KiB.
 *
 *  - main's return value is handed to harness_exit (io.c), which prints
 *    it as a sentinel the runner stops at; a program linked with lib/libc
 *    replaces it with exit() (exec.c), so atexit handlers and stdio's
 *    buffers are flushed as on any other target.
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
void harness_exit(int status);

#define SRAM_TOP 0x20400000u

/* Coprocessor Access Control Register: CP10 (bits 21:20) and CP11
 * (bits 23:22) to 0b11, full access. Both halves, or the first
 * instruction that uses the other one faults. */
#define CPACR (*(volatile unsigned *)0xE000ED88u)

__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)SRAM_TOP, (void *)reset };

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;

    CPACR |= (3u << 20) | (3u << 22);
    __asm__ volatile("dsb");
    __asm__ volatile("isb");

    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    harness_exit(main());
    for (;;)
        ;
}
