/* The seam between the C library and an operating system.
 *
 * Everything above this is portable C; everything below is one small file
 * per OS. A new target implements these and gets the library — the same
 * bargain `src/platform/platform.h` makes for the compiler.
 *
 * The set is deliberately the classic one, because it is the set that is
 * actually sufficient: a hosted C library needs to move bytes to and from
 * file descriptors, to grow its heap, to know the time, and to stop. It
 * does not need anything else, and a backend that offers more invites the
 * library above it to depend on a particular OS.
 *
 * The threading primitives at the end are the one OPTIONAL group. A
 * target without them is not broken -- it is single-threaded, and the
 * C++ library above says so by failing where a thread would have been
 * created rather than by pretending. Every one of them returns ENOSYS
 * when the target provides nothing, and that answer travels up to
 * std::thread's constructor as a system_error. The alternative, a
 * std::thread that runs its function on the calling thread, would be a
 * lie that deadlocks the first time somebody joins from inside it.
 */
#ifndef EMBLIBC_OS_BACKEND_H
#define EMBLIBC_OS_BACKEND_H

#include <stddef.h>

/* The C++ runtime's terminate handler writes its final message through
 * __os_write, for the same reason assert does: a diagnostic that needs a
 * working allocator disappears in the cases worth diagnosing. So this
 * contract is reached from C++ and carries C linkage explicitly. */
#ifdef __cplusplus
extern "C" {
#endif

/* Bytes in and out. Return the count moved, or -1 with errno set. A short
 * write is not an error and the caller loops — which is why stdio, not the
 * backend, owns buffering. */
long __os_write(int fd, const void *buf, size_t n);
long __os_read(int fd, void *buf, size_t n);

/* The flags __os_open takes. They are part of THIS contract, not of any
 * one OS's headers: the library has to name them to ask for "create it,
 * truncate it", and a backend whose kernel numbers them differently
 * translates. The values are the ones Linux x86 chose and nearly every
 * kernel since copied, so most backends translate by doing nothing --
 * EmbLinkOS's numbers are identical, and tests/golden/libc-emblinkos.sh
 * asserts that rather than trusting it. */
#define __OS_O_RDONLY 0x0000
#define __OS_O_WRONLY 0x0001
#define __OS_O_RDWR   0x0002
#define __OS_O_CREAT  0x0040
#define __OS_O_EXCL   0x0080
#define __OS_O_TRUNC  0x0200
#define __OS_O_APPEND 0x0400

/* Files by name. A backend with no filesystem returns -1 and sets ENOSYS;
 * everything that does not need one still works. */
int  __os_open(const char *path, int flags, int mode);
int  __os_close(int fd);
long __os_lseek(int fd, long off, int whence);

/* Files by name, removed and renamed. C11 §7.21.4 requires both, and a
 * target with no way to remove a file returns -1 with ENOSYS rather than
 * pretending to succeed -- a caller that cannot tell a removal from a
 * no-op writes a program that leaves its temporary files behind. */
int  __os_remove(const char *path);
int  __os_rename(const char *from, const char *to);

/* The heap. Returns the previous break, or (void *)-1. A backend that
 * cannot grow returns -1 and malloc reports failure honestly rather than
 * handing back memory it does not have. */
void *__os_sbrk(long increment);

/* Seconds since the epoch, or -1 where the OS has no clock. */
long __os_time(void);

/* Nanoseconds of CPU time used, or -1: what clock() reports. */
long __os_clock_ns(void);

/* Stop. Never returns. */
void __os_exit(int status);

/* Is this descriptor a terminal? stdio uses it to choose line buffering. */
int  __os_isatty(int fd);

/* Random bytes for the few library functions that must not be predictable.
 * 0 on success, -1 when the OS cannot provide them. */
int  __os_getentropy(void *buf, size_t n);

/* ---- threads, and blocking ------------------------------------------------
 *
 * Optional: a target that implements none of these is single-threaded,
 * and each returns -1 with errno ENOSYS there.
 *
 * There are two ideas here and only two. A thread is something that
 * runs a function and can be waited for. Blocking is something that
 * sleeps until a WORD IN MEMORY changes -- the futex operation, which
 * is the one primitive every mutex, condition variable, semaphore and
 * latch in the C++ library is built from. Offering a "mutex" primitive
 * instead would push the policy into the OS and make every target
 * reimplement the same fairness and recursion decisions; offering the
 * word is enough and leaves the policy here.
 */

/* Run fn(arg) on a new thread. 0 on success, with *id set to a handle
 * the target chooses; -1 with errno otherwise. The thread ends when fn
 * returns. */
int  __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg);

/* Wait for it to end. 0, or -1 with errno. Joining a thread twice, or
 * joining a detached one, is the caller's mistake and is not checked
 * here -- the C++ library above checks, because it has the state to. */
int  __os_thread_join(unsigned long id);

/* Give up the claim to join it: its resources are released when it
 * ends. 0, or -1 with errno. */
int  __os_thread_detach(unsigned long id);

/* This thread's handle. 0 on a target with no threads, which is a valid
 * answer: there is exactly one thread and it needs no name. */
unsigned long __os_thread_self(void);

/* Let another runnable thread have the processor. A no-op is a correct
 * implementation. */
void __os_thread_yield(void);

/* Sleep for at least ns nanoseconds. 0, or -1 with errno. */
int  __os_sleep_ns(long ns);

/* Sleep until *addr differs from expected, or until somebody wakes this
 * address, or until timeout_ns has passed (negative: no timeout). The
 * comparison and the sleep must be ATOMIC with respect to a wake --
 * that is the entire difficulty and the entire reason this is a
 * primitive rather than a loop. A spurious return is allowed and every
 * caller here loops. 0, or -1 with errno. */
int  __os_futex_wait(const volatile int *addr, int expected, long timeout_ns);

/* Wake up to `count` waiters on addr (-1: all of them). The number
 * woken, or -1 with errno.
 *
 * A backend must TRANSLATE the -1; it is this seam's spelling, not any
 * kernel's. Linux's FUTEX_WAKE takes a count and stops when it has
 * woken that many, so handing it a negative one wakes nobody -- and
 * returns 0 rather than an error, so the caller learns nothing and its
 * waiters sleep forever. Every broadcast in the C++ library is this
 * call (condition_variable::notify_all, atomic::notify_all, latch,
 * barrier, the static-initialisation guard), so getting it wrong
 * silently hangs all of them. tests/golden/threadsafe.sh checks it. */
int  __os_futex_wake(const volatile int *addr, int count);

/* ---- the filesystem --------------------------------------------------------
 *
 * The second OPTIONAL group, on the same terms as the threads above: a
 * target that implements none of these has no filesystem beyond the
 * file descriptors at the top of this file, each returns -1 with errno
 * ENOSYS, and <filesystem> reports that as a filesystem_error carrying
 * errc::function_not_supported.
 *
 * The set is the one <filesystem> actually needs and no more. What it
 * needs, and why each is here rather than derivable from the others:
 *
 *   stat    everything about a path that is not its contents. exists,
 *           is_directory, file_size and last_write_time are all one
 *           call, so asking four questions costs one syscall rather
 *           than four.
 *   lstat   the same WITHOUT following a symlink. The distinction is
 *           the whole of symlink_status versus status, and a recursive
 *           walk that confuses them follows a loop forever.
 *   opendir/readdir/closedir
 *           directory_iterator, which cannot be built from anything
 *           else: a directory's contents are not readable as a file.
 *   getcwd/chdir
 *           current_path, and what an empty relative path resolves
 *           against.
 *   statfs  space(), which reports three numbers and not one --
 *           "free" and "available to this caller" differ wherever
 *           space is reserved for the superuser.
 *
 * The rest -- mkdir, rmdir, unlink, chmod, truncate, utime, symlink,
 * readlink, link -- are the mutations. Each is one operation because
 * each is one syscall on every OS that has them.
 */

/* What a path is. A symlink reports SYMLINK only from lstat; stat
 * follows it and reports what it points at. */
#define __OS_FT_UNKNOWN   0
#define __OS_FT_NOT_FOUND 1
#define __OS_FT_REGULAR   2
#define __OS_FT_DIRECTORY 3
#define __OS_FT_SYMLINK   4
#define __OS_FT_CHARDEV   5
#define __OS_FT_BLOCKDEV  6
#define __OS_FT_FIFO      7
#define __OS_FT_SOCKET    8

struct __os_fileinfo {
    unsigned long long size;
    long mtime;                /* seconds since the epoch */
    unsigned int mode;         /* the permission bits, POSIX-shaped */
    int type;                  /* __OS_FT_* */
    /* Identity. Two paths name the same file when BOTH match, which is
     * what equivalent() asks and what a comparison of paths cannot
     * answer -- two different paths may be one file. */
    unsigned long long dev, ino;
    unsigned long long nlink;
};

int  __os_stat(const char *path, struct __os_fileinfo *out);
int  __os_lstat(const char *path, struct __os_fileinfo *out);
int  __os_mkdir(const char *path, unsigned mode);
int  __os_rmdir(const char *path);
int  __os_unlink(const char *path);
int  __os_chmod(const char *path, unsigned mode);
int  __os_truncate(const char *path, long long size);
int  __os_utime(const char *path, long mtime);
int  __os_symlink(const char *target, const char *linkpath);
long __os_readlink(const char *path, char *buf, size_t n);
int  __os_link(const char *target, const char *linkpath);
int  __os_getcwd(char *buf, size_t n);
int  __os_chdir(const char *path);
int  __os_statfs(const char *path, unsigned long long *capacity,
                 unsigned long long *freespace, unsigned long long *available);

/* Reading a directory. The handle is whatever the target wants it to
 * be; NULL means failure, with errno set.
 *
 * __os_readdir returns 1 for an entry, 0 at the end, -1 on error --
 * three outcomes, because "no more entries" and "something went wrong"
 * are different and a caller that cannot tell them apart silently
 * truncates a listing. `.` and `..` are NOT returned: every caller
 * filters them, so filtering once here is one place to get it right
 * rather than one per caller. */
void *__os_opendir(const char *path);
int   __os_readdir(void *dir, char *name, size_t n, int *type);
void  __os_closedir(void *dir);

#ifdef __cplusplus
}
#endif

#endif
