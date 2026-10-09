/* Output and exit for the Xtensa harness: the QEMU sim machine's
 * simcalls (target/xtensa/xtensa-semi.c), which run with -semihosting.
 * `simcall` takes the call number in a2 and its arguments in a3 onward
 * and returns in a2 -- which is where a call8's callee finds its
 * arguments and leaves its result, so the instruction is wrapped in a
 * windowed function (vectors.h, xt_simcall_code) and called through a
 * pointer: SYS_write is 4, SYS_exit 1. */
#include "vectors.h"

typedef int (*simcall_fn)(int nr, int a, int b, int c);
#define SIMCALL ((simcall_fn)(const void *)xt_simcall_code)

void writec(int c)
{
    unsigned char b = (unsigned char)c;
    SIMCALL(4, 1, (int)(unsigned)&b, 1);
}

void puts_(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    SIMCALL(4, 1, (int)(unsigned)s, (int)n);
}

/* The trailing space is part of the interface: the same programs run on
 * the host against hostio.c, whose putn is printf("%ld "). */
void putn(long v)
{
    char b[24];
    int n = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    if (v < 0)
        writec('-');
    do { b[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (n)
        writec(b[--n]);
    writec(' ');
}

/* lib/libc's output, when the program is linked with it: its write() is
 * weak, and this one puts stdout and stderr on the sim console. Weak
 * itself, so a program that brings its own write() links too. */
__attribute__((weak)) long write(int fd, const void *buf, unsigned long n)
{
    (void)fd;
    SIMCALL(4, 1, (int)(unsigned)buf, (int)n);
    return (long)n;
}

/* The end of the run: the sentinel, then QEMU's exit with the status. */
void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status & 0xff);
    puts_("==\n");
    SIMCALL(1, status & 0xff, 0, 0);
    for (;;)
        ;
}
