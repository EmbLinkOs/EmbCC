/* -fstack-protector's guard: __stack_chk_guard, the value every protected
 * function's canary is a copy of (src/ir/irgen.c's ssp_check compares
 * them). A C library that has one -- libSystem, glibc on AArch64, a
 * board's newlib -- is linked first and its wins: these are only the
 * fallback, which is why they are weak.
 *
 * It is set once, before main and before any protected function can
 * have a frame live: on x86-64 Linux to the thread block's (%fs:0x28,
 * which glibc fills from the kernel's AT_RANDOM and GCC's code reads),
 * elsewhere to a value with its low byte zero -- a string overrun
 * cannot reproduce it -- mixed with the stack's address, which is all
 * the entropy a board without a random source offers. A program may set
 * it itself, earlier still. __stack_chk_fail's fallback traps; a C
 * library's (EmbCC's lib/libc/src/stdlib/ssp.c) says why first. */

typedef __UINTPTR_TYPE__ uptr;

__attribute__((weak)) uptr __stack_chk_guard =
    (uptr)0xff0a5ac3e1d40000ull;

__attribute__((weak, noreturn)) void __stack_chk_fail(void)
{
    __builtin_trap();
}

__attribute__((constructor(101), no_instrument_function,
               no_stack_protector))
static void ssp_init(void)
{
#if defined(__x86_64__) && defined(__linux__)
    uptr g;
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(g));
    if (g) {
        __stack_chk_guard = g;
        return;
    }
#endif
    uptr x = (uptr)__builtin_frame_address(0);
    __stack_chk_guard ^= (x << 13) ^ (x >> 7);
    __stack_chk_guard &= ~(uptr)0xff;          /* the terminator byte */
}
