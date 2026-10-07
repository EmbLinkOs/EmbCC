/* The TriCore (QEMU tricore_testboard) test harness: a startup and a way
 * to stop, in C.
 *
 * QEMU loads the image (-kernel) into the board's code RAM at its link
 * address and starts it at the ELF entry, which is `embld -Tstack`'s
 * stub: it sets A10, links the context-save areas (--csa) into the free
 * list every CALL draws from, turns call-depth counting off, and jumps
 * here -- with JI, not CALL, so this function must never return. Then
 * this copies .data (which moves nothing in a RAM image), zeroes .bss,
 * runs the static constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL --
 * `==EXIT n==` with main's result -- which the runner stops at
 * (tests/harness/qrun.sh --until); io.c's _exit prints it and then ends
 * QEMU through the board's test device.
 *
 * Built with -DHARNESS_LIBC for a program linked with lib/libc: then main's
 * result goes through exit(), so atexit handlers run and stdio is flushed
 * before the sentinel. */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
/* The static constructors (`__attribute__((constructor))`), which embld
 * gathers into .init_array between these two symbols. */
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);
void _exit(int status);
#ifdef HARNESS_LIBC
void exit(int status);
#endif

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    int r;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
