/* The backend for a target that offers the classic syscall set: the QEMU
 * test harness, EmbLinkOS, and anything Unix-shaped.
 *
 * It is this short on purpose. Everything above it -- buffering, formatting,
 * the allocator's policy -- is portable and shared; what a new OS has to
 * write is this file and nothing else (os/backend.h).
 */
#include "../backend.h"

#include <errno.h>

/* Provided by the target: the raw calls, with the classic signatures. A
 * bare-metal harness implements them over a serial port; an OS over its
 * syscalls. */
extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long);
extern int  open(const char *, int, int);
extern int  close(int);
extern long lseek(int, long, int);
extern void *sbrk(long);
extern void _exit(int);
extern int  isatty(int);

long __os_write(int fd, const void *buf, size_t n)
{
    return write(fd, buf, (unsigned long)n);
}

long __os_read(int fd, void *buf, size_t n)
{
    return read(fd, buf, (unsigned long)n);
}

int  __os_open(const char *path, int flags, int mode)
{
    return open(path, flags, mode);
}

int  __os_close(int fd)  { return close(fd); }

long __os_lseek(int fd, long off, int whence)
{
    return lseek(fd, off, whence);
}

void *__os_sbrk(long increment) { return sbrk(increment); }

void __os_exit(int status) { _exit(status); for (;;) {} }

int __os_isatty(int fd) { return isatty(fd); }

/* The bare harness has no filesystem removal -- it has no filesystem.
 * Reporting that is the honest answer; inventing success would leave a
 * caller believing a file is gone. */
int __os_remove(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int __os_rename(const char *from, const char *to)
{
    (void)from; (void)to;
    errno = ENOSYS;
    return -1;
}

/* No clock on a bare harness. Reporting -1 is the honest answer, and
 * time()/clock() pass it through rather than inventing a number. */
long __os_time(void)     { errno = ENOSYS; return -1; }
long __os_clock_ns(void) { errno = ENOSYS; return -1; }

int __os_getentropy(void *buf, size_t n)
{
    (void)buf; (void)n;
    errno = ENOSYS;
    return -1;
}

/* ---- threads ---------------------------------------------------------------
 *
 * Optional, and taken from the target if it has them. Each is declared
 * WEAK: a target that provides the symbol gets real threads, and one
 * that does not links anyway and gets ENOSYS. That is what keeps a
 * freestanding harness -- which has one core, one stack and no
 * scheduler -- linkable against the same library as a hosted OS.
 *
 * The names are the classic ones so that a target already implementing
 * a Unix-shaped kernel has nothing to write: clone/join/yield and the
 * futex pair.
 */
extern int __attribute__((weak))
    thread_create(unsigned long *, void (*)(void *), void *);
extern int __attribute__((weak)) thread_join(unsigned long);
extern int __attribute__((weak)) thread_detach(unsigned long);
extern unsigned long __attribute__((weak)) thread_self(void);
extern void __attribute__((weak)) thread_yield(void);
extern int __attribute__((weak)) nanosleep_ns(long);
extern int __attribute__((weak)) futex_wait(const volatile int *, int, long);
extern int __attribute__((weak)) futex_wake(const volatile int *, int);

int __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg)
{
    if (!thread_create) {
        errno = ENOSYS;
        return -1;
    }
    return thread_create(id, fn, arg);
}

int __os_thread_join(unsigned long id)
{
    if (!thread_join) {
        errno = ENOSYS;
        return -1;
    }
    return thread_join(id);
}

int __os_thread_detach(unsigned long id)
{
    if (!thread_detach) {
        errno = ENOSYS;
        return -1;
    }
    return thread_detach(id);
}

/* Zero is a valid answer on a target with one thread: there is exactly
 * one and it needs no name. */
unsigned long __os_thread_self(void)
{
    return thread_self ? thread_self() : 0ul;
}

/* A no-op is a correct yield when there is nobody to yield to. */
void __os_thread_yield(void)
{
    if (thread_yield)
        thread_yield();
}

int __os_sleep_ns(long ns)
{
    if (!nanosleep_ns) {
        errno = ENOSYS;
        return -1;
    }
    return nanosleep_ns(ns);
}

/* Without a futex there is nobody who could change the word: this
 * thread is the only one running, so waiting for another to act would
 * never end. ENOSYS says so, and the callers above turn it into a spin
 * or a deadlock diagnosis rather than an endless sleep. */
int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    if (!futex_wait) {
        errno = ENOSYS;
        return -1;
    }
    return futex_wait(addr, expected, timeout_ns);
}

int __os_futex_wake(const volatile int *addr, int count)
{
    if (!futex_wake) {
        errno = ENOSYS;
        return -1;
    }
    return futex_wake(addr, count);
}
