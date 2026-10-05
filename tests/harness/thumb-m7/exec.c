/* What a program linked with lib/libc needs from the Cortex-M7 harness:
 * the three hooks the bare-metal backend leaves weak
 * (lib/libc/os/baremetal/backend.c). write() goes to the UART, _exit()
 * prints the status as io.c's sentinel, and main's return goes through
 * exit() so atexit handlers run and stdio is flushed first -- which is
 * what tests/exec's `// expect-exit: N` means on every other target. */
void writec(int c);
void puts_(const char *s);
void putn(long v);
void exit(int status);

long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    unsigned long k;
    if (fd != 1 && fd != 2)
        return -1;
    for (k = 0; k < n; k++)
        writec(p[k]);
    return (long)n;
}

void _exit(int status)
{
    puts_("\n==EXIT ");
    putn(status);
    puts_("==\n");
    for (;;)
        ;
}

void harness_exit(int status)
{
    exit(status);
}
