/* The syscall floor for the x86-64 test harness: newlib's bare POSIX names
 * over two QEMU ISA devices.
 *
 * Output goes to the debug console, port 0xE9 (QEMU -debugcon), which needs
 * no UART setup. Exit goes to isa-debug-exit at port 0xF4 — but that device
 * turns a written value v into the process status (v << 1) | 1, truncated to
 * 8 bits, so it cannot carry an arbitrary exit code (the suite has a test
 * that must exit 191). The real code is therefore printed first as a marker
 * line, `@@EMBCC-EXIT n@@`, which tests/run.sh reads back; the device's own
 * status only says that the guest exited on purpose rather than hanging or
 * triple-faulting.
 *
 * Same shape as ../aarch64/semihost.c, and the same reason for existing:
 * this newlib expects EmbLinkOS's bare syscall names, not libgloss's.
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEBUGCON  0xE9
#define EXIT_PORT 0xF4

static inline void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void outl(unsigned short port, unsigned int v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static char last = '\n';   /* the last byte written, so the exit marker can
                            * start on a fresh line without adding one */

static void put(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        outb(DEBUGCON, (unsigned char)s[i]);
    if (n)
        last = s[n - 1];
}

int write(int fd, const void *buf, size_t len)
{
    (void)fd;
    put(buf, len);
    return (int)len;
}

int read(int fd, void *buf, size_t len)
{
    (void)fd; (void)buf; (void)len;
    return 0;
}

int close(int fd) { (void)fd; return 0; }

off_t lseek(int fd, off_t off, int whence)
{
    (void)fd; (void)off; (void)whence;
    errno = ESPIPE;
    return (off_t)-1;
}

int fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;       /* a character device: unbuffered stdout */
    st->st_blksize = 1024;
    return 0;
}

int isatty(int fd) { (void)fd; return 1; }
int getpid(void) { return 1; }

int kill(int pid, int sig)
{
    (void)pid; (void)sig;
    errno = EINVAL;
    return -1;
}

extern char __heap_start[], __heap_end[];

void *sbrk(ptrdiff_t incr)
{
    static char *brk;
    if (brk == 0)
        brk = __heap_start;
    if (incr < 0 || (size_t)(__heap_end - brk) < (size_t)incr) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *prev = brk;
    brk += incr;
    return prev;
}

void _exit(int code)
{
    char buf[32];
    int n = 0;
    unsigned int v = (unsigned int)code & 0xff;
    if (last != '\n')
        put("\n", 1);
    const char *head = "@@EMBCC-EXIT ";
    while (head[n]) n++;
    put(head, (size_t)n);
    n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) outb(DEBUGCON, (unsigned char)buf[--n]);
    put("@@\n", 3);
    outl(EXIT_PORT, 0);
    for (;;)
        __asm__ volatile("hlt");
}
