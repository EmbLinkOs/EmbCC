/* The Xtensa (QEMU sim, de212 core) test harness: a startup and a way to
 * stop, in C.
 *
 * QEMU loads the image (-kernel) into the sim machine's RAM at its link
 * address and starts it at the ELF entry, which is `embld -Tstack`'s
 * stub: it sets sp, sets PS for the windowed ABI (WOE, user vector mode,
 * interrupt level 0) and calls _start with callx8 -- so _start is an
 * ordinary windowed C function. This then installs the exception vectors
 * (vectors.h: the window overflow and underflow handlers every program
 * needs once its calls nest deeper than the 32 physical registers, the
 * Alloca exception's handler, and a fault report), copies .data (which
 * moves nothing in a RAM image), zeroes .bss, runs the static
 * constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL --
 * `==EXIT n ==` with main's result -- which the runner stops at
 * (tests/harness/qrun.sh --until); io.c's _exit prints it and then ends
 * QEMU with the sim machine's exit simcall.
 *
 * Built with -DHARNESS_LIBC for a program linked with lib/libc: then
 * main's result goes through exit(), so atexit handlers run and stdio is
 * flushed before the sentinel. */
#include "vectors.h"

extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);
void _exit(int status);
#ifdef HARNESS_LIBC
void exit(int status);
#endif

/* The vectors go where VECBASE says, and VECBASE is set to say so (its
 * reset value on the de212 is the same address, but the startup should
 * not depend on that). */
static void install_vectors(void)
{
    volatile unsigned char *v = (volatile unsigned char *)XT_VEC_BASE;
    void (*setvb)(unsigned) = (void (*)(unsigned))(const void *)xt_vecbase_code;
    for (unsigned k = 0; k < sizeof xt_vectors; k++)
        v[k] = xt_vectors[k];
    setvb(XT_VEC_BASE);
}

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    int r;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    install_vectors();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
