/* <sys/mman.h>: mapped memory, and POSIX shared memory objects -- which
 * are files under /dev/shm, as Linux and glibc have them. */
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "posix.h"

WEAK void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return __os_mmap(addr, len, prot, flags, fd, off);
}

WEAK int munmap(void *addr, size_t len) { return __os_munmap(addr, len); }

WEAK int mprotect(void *addr, size_t len, int prot)
{
    return __os_mprotect(addr, len, prot);
}

WEAK int msync(void *addr, size_t len, int flags)
{
    return __os_msync(addr, len, flags);
}

WEAK int madvise(void *addr, size_t len, int advice)
{
    return __os_madvise(addr, len, advice);
}

/* advice again, returning its error */
WEAK int posix_madvise(void *addr, size_t len, int advice)
{
    int saved = errno;
    int r = __os_madvise(addr, len, advice) < 0 ? errno : 0;
    errno = saved;
    return r == ENOSYS ? 0 : r;
}

WEAK int mlock(const void *addr, size_t len) { return __os_mlock(addr, len, 1); }
WEAK int munlock(const void *addr, size_t len) { return __os_mlock(addr, len, 0); }

/* "/name": one component, which becomes /dev/shm/name */
static const char *shm_path(const char *name, char *buf)
{
    while (*name == '/')
        name++;
    size_t n = strlen(name);
    if (!n || strchr(name, '/') || n > NAME_MAX) {
        errno = EINVAL;
        return NULL;
    }
    memcpy(buf, "/dev/shm/", 9);
    memcpy(buf + 9, name, n + 1);
    return buf;
}

WEAK int shm_open(const char *name, int flags, mode_t mode)
{
    char buf[NAME_MAX + 10];
    const char *p = shm_path(name, buf);
    return p ? __os_open(p, flags | O_NOFOLLOW | O_CLOEXEC, (int)mode) : -1;
}

WEAK int shm_unlink(const char *name)
{
    char buf[NAME_MAX + 10];
    const char *p = shm_path(name, buf);
    return p ? __os_unlink(p) : -1;
}
