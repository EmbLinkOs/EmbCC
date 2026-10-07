/* The RISC-V test harness: a startup and a way to stop, in C.
 *
 * The one thing that is NOT C is the first four instructions. A
 * Cortex-M's processor fetches its initial stack pointer from the first
 * word of the image, which is why the ARMv7-M harness needs no assembler
 * at all; RISC-V has no such mechanism -- every register is zero at reset
 * -- and C cannot write sp. So `embld -Tstack` emits the four
 * instructions that set it and jump here, from the same encoder the
 * compiler uses. That is the software half of what the other target gets
 * in hardware, and it keeps this harness buildable by this toolchain
 * alone.
 *
 * The bracket symbols come from embld (-Tdata): .data is STORED after the
 * text and ADDRESSED in RAM, so the first thing to run copies it across
 * and zeroes .bss. That is the whole of a C runtime here.
 */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
/* The static constructors -- a C++ namespace-scope object's, a C
 * __attribute__((constructor)) -- which embld gathers into .init_array
 * between these two symbols. */
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);

/* QEMU's `virt` board has a SiFive test device at 0x100000: a halfword
 * of 0x5555 there exits QEMU with status 0, and 0x3333 with a failure
 * code in the upper bits. So unlike the Cortex-M harness -- which has to
 * fault ON PURPOSE because the reset handler is the bottom of the call
 * stack and there is no way to say "exit" -- this one stops cleanly and
 * the runner can believe the exit status as well as the output. */
#define SIFIVE_TEST (*(volatile unsigned *)0x100000u)
#define TEST_PASS   0x5555u

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    main();
    SIFIVE_TEST = TEST_PASS;
    for (;;)
        ;
}
