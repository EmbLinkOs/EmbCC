/* The board side of a tests/cxx-embedded program: lib/libc's bare-metal
 * backend writes standard output through write(), which is weak there, and
 * this one sends it to the harness's UART (tests/harness/<board>/io.c).
 * Not part of the host build, where the C library writes to a terminal. */
extern void writec(int c);

long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    (void)fd;
    for (unsigned long i = 0; i < n; i++)
        writec(p[i]);
    return (long)n;
}
