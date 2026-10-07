/* The backend for a part with no operating system: a microcontroller, a
 * QEMU board, anything EmbCC's embedded targets run on.
 *
 * It needs NOTHING from the program to link. The classic calls a
 * bare-metal port supplies -- write, read, sbrk, _exit and the rest -- are
 * defined here WEAKLY, with the defaults a bare part can honour, so a
 * program that defines write() over its UART (or a semihosting call, or a
 * ring buffer) gets printf there, and one that defines nothing still links
 * and runs: output discarded, input at end of file. newlib's libnosys is
 * the same arrangement.
 *
 * Weak DEFINITIONS, not weak references tested at run time (posixlike's
 * way): an undefined weak function is address 0, and a RISC-V `call` from
 * an image at 0x80000000 cannot reach it -- the link fails even though the
 * call is never made.
 *
 *   the heap is the RAM between the end of the image (embld's _end) and
 *   the stack, which grows down toward it; sbrk refuses to come within a
 *   margin of the current stack pointer, so malloc returns NULL rather
 *   than handing out the stack;
 *   _exit stops in a loop;
 *   there is one thread, no clock and no filesystem: each of those is -1
 *   with errno ENOSYS, as backend.h asks.
 */
#include "../backend.h"

#include <errno.h>
#include <stddef.h>

#define WEAK __attribute__((weak))

static int nosys(void)
{
    errno = ENOSYS;
    return -1;
}

WEAK long write(int fd, const void *buf, unsigned long n)
{
    (void)buf;
    if (fd == 1 || fd == 2)
        return (long)n;                 /* nowhere to go: discarded */
    errno = EBADF;
    return -1;
}

WEAK long read(int fd, void *buf, unsigned long n)
{
    (void)buf;
    (void)n;
    if (fd == 0)
        return 0;                       /* end of file */
    errno = EBADF;
    return -1;
}

WEAK int open(const char *path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    return nosys();
}

WEAK int close(int fd)        { return fd >= 0 && fd <= 2 ? 0 : nosys(); }
WEAK long lseek(int fd, long off, int whence)
{
    (void)fd;
    (void)off;
    (void)whence;
    return nosys();
}

/* The console is a terminal: stdio line-buffers it, so a line reaches the
 * UART when it ends rather than when a buffer fills. */
WEAK int isatty(int fd)       { return fd >= 0 && fd <= 2; }

WEAK void _exit(int status)
{
    (void)status;
    for (;;) {}
}

/* ---- the heap --------------------------------------------------------------
 *
 * From the end of the image upward. The stack is the other end of the
 * same RAM on every part EmbCC targets, so the limit is where the stack
 * is NOW, less a margin for the calls still to come -- the check a
 * bare-metal sbrk makes, and the only one there is without a linker
 * script naming a heap. A part whose stack is elsewhere defines sbrk(). */
extern char _end[];

#define HEAP_MARGIN 512

WEAK void *sbrk(long increment)
{
    static char *brk;
    char here, *old;
    if (!brk)
        /* from a 16-byte boundary: malloc hands out its blocks from here
         * aligned for max_align_t (16, lib/libc/src/stdlib/malloc.c), and
         * the image may end at any word */
        brk = _end + (-(unsigned long)_end & 15);
    old = brk;
    if (increment < 0 ? (unsigned long)-increment > (unsigned long)(brk - _end)
                      : brk + increment + HEAP_MARGIN > &here) {
        errno = ENOMEM;
        return (void *)-1;
    }
    brk += increment;
    return old;
}

long __os_write(int fd, const void *buf, size_t n)
{
    return write(fd, buf, (unsigned long)n);
}
long __os_read(int fd, void *buf, size_t n)
{
    return read(fd, buf, (unsigned long)n);
}
int __os_open(const char *path, int flags, int mode)
{
    return open(path, flags, mode);
}
int __os_close(int fd)                        { return close(fd); }
long __os_lseek(int fd, long off, int whence) { return lseek(fd, off, whence); }
int __os_remove(const char *path)             { (void)path; return nosys(); }
int __os_rename(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return nosys();
}
void *__os_sbrk(long increment)               { return sbrk(increment); }
long __os_time(void)                          { return nosys(); }
long __os_clock_ns(void)                      { return nosys(); }
void __os_exit(int status)
{
    _exit(status);
    for (;;) {}
}
int __os_isatty(int fd)                       { return isatty(fd); }
int __os_getentropy(void *buf, size_t n)
{
    (void)buf;
    (void)n;
    return nosys();
}

/* ---- one thread --------------------------------------------------------------
 * Nobody else runs, so nobody could change a word this thread waits on:
 * ENOSYS, which the callers above turn into a spin or a deadlock
 * diagnosis rather than an endless sleep (posixlike's reasoning). */
int __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg)
{
    (void)id;
    (void)fn;
    (void)arg;
    return nosys();
}
int __os_thread_join(unsigned long id)   { (void)id; return nosys(); }
int __os_thread_detach(unsigned long id) { (void)id; return nosys(); }
unsigned long __os_thread_self(void)     { return 0ul; }
void __os_thread_yield(void)             {}
int __os_sleep_ns(long ns)               { (void)ns; return nosys(); }
int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    (void)addr;
    (void)expected;
    (void)timeout_ns;
    return nosys();
}
int __os_futex_wake(const volatile int *addr, int count)
{
    (void)addr;
    (void)count;
    return nosys();
}

/* ---- no filesystem ----------------------------------------------------------- */
struct __os_fileinfo;
int __os_stat(const char *p, struct __os_fileinfo *o)
{
    (void)p;
    (void)o;
    return nosys();
}
int __os_lstat(const char *p, struct __os_fileinfo *o)
{
    (void)p;
    (void)o;
    return nosys();
}
int __os_mkdir(const char *p, unsigned m)    { (void)p; (void)m; return nosys(); }
int __os_rmdir(const char *p)                { (void)p; return nosys(); }
int __os_unlink(const char *p)               { (void)p; return nosys(); }
int __os_chmod(const char *p, unsigned m)    { (void)p; (void)m; return nosys(); }
int __os_truncate(const char *p, long long s)
{
    (void)p;
    (void)s;
    return nosys();
}
int __os_utime(const char *p, long t)        { (void)p; (void)t; return nosys(); }
int __os_symlink(const char *t, const char *l)
{
    (void)t;
    (void)l;
    return nosys();
}
long __os_readlink(const char *p, char *b, size_t n)
{
    (void)p;
    (void)b;
    (void)n;
    return nosys();
}
int __os_link(const char *t, const char *l)  { (void)t; (void)l; return nosys(); }
int __os_getcwd(char *b, size_t n)           { (void)b; (void)n; return nosys(); }
int __os_chdir(const char *p)                { (void)p; return nosys(); }
int __os_statfs(const char *p, unsigned long long *capacity,
                unsigned long long *free_, unsigned long long *avail)
{
    (void)p;
    (void)capacity;
    (void)free_;
    (void)avail;
    return nosys();
}
void *__os_opendir(const char *p)            { (void)p; nosys(); return NULL; }
int __os_readdir(void *d, char *name, size_t n, int *type)
{
    (void)d;
    (void)name;
    (void)n;
    (void)type;
    return nosys();
}
void __os_closedir(void *d)                  { (void)d; }
