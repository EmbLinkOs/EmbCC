/* POSIX's descriptors and filesystem: open, read and write in their
 * forms, dup and fcntl, pipes, locks, stat's fields, links, modes and
 * times, the *at functions relative to a directory descriptor (the real
 * *at calls on Linux, which work with no /proc), and directories as
 * readdir reads them -- `.` and `..` included, positions kept.
 *
 * The expected values are what glibc 2.36 gave for the same calls on the
 * same kernel. Without a filesystem (EMBCC_TEST_HAS_FS unset: the boards)
 * what is asked is that the calls fail and say so. */
// expect-exit: 42
// needs-libc: POSIX's descriptor and file calls, on lib/libc's backends
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef EMBCC_TEST_HAS_FS
static int descriptors(void)
{
    int fd = open("/tmp/f1", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "hello world", 11) != 11 ||
        lseek(fd, 0, SEEK_CUR) != 11)
        return 1;
    char b[32] = { 0 };
    if (pread(fd, b, 5, 6) != 5 || strcmp(b, "world") != 0)
        return 2;
    struct iovec iov[2] = { { "AB", 2 }, { "CD", 2 } };
    lseek(fd, 0, SEEK_END);
    struct stat st;
    if (writev(fd, iov, 2) != 4 || fstat(fd, &st) != 0 || st.st_size != 15 ||
        !S_ISREG(st.st_mode) || (st.st_mode & 0777) != 0644 ||
        st.st_nlink != 1)
        return 3;
    int d = dup(fd), d2 = dup2(fd, 20), d3 = fcntl(fd, F_DUPFD, 30);
    if (d <= fd || d2 != 20 || d3 != 30)
        return 4;
    if (fcntl(fd, F_GETFD) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fd, F_GETFD) != FD_CLOEXEC || fcntl(d, F_GETFD) != 0)
        return 5;
    int fl = fcntl(fd, F_GETFL);
    if ((fl & O_ACCMODE) != O_RDWR || (fl & O_APPEND))
        return 6;
    fcntl(fd, F_SETFL, fl | O_APPEND | O_NONBLOCK);
    fl = fcntl(fd, F_GETFL);
    if (!(fl & O_APPEND) || !(fl & O_NONBLOCK))
        return 7;
    struct flock lk = { F_WRLCK, SEEK_SET, 0, 5, 0 };
    struct flock q = { F_WRLCK, SEEK_SET, 0, 5, 0 };
    /* a process's own locks do not conflict with it: F_UNLCK */
    if (fcntl(fd, F_SETLK, &lk) != 0 || fcntl(fd, F_GETLK, &q) != 0 ||
        q.l_type != F_UNLCK)
        return 8;
    if (ftruncate(fd, 3) != 0 || fstat(fd, &st) != 0 || st.st_size != 3 ||
        fsync(fd) != 0 || fdatasync(fd) != 0)
        return 9;
    errno = 0;
    if (close(999) != -1 || errno != EBADF)
        return 10;
    errno = 0;
    if (dup3(fd, fd, 0) != -1 || errno != EINVAL)
        return 11;
    close(fd), close(d), close(d2), close(d3);

    int p[2];
    if (pipe(p) != 0 || write(p[1], "xyz", 3) != 3)
        return 12;
    close(p[1]);
    memset(b, 0, sizeof b);
    if (read(p[0], b, sizeof b) != 3 || strcmp(b, "xyz") ||
        read(p[0], b, sizeof b) != 0)
        return 13;
    close(p[0]);
    if (pipe2(p, O_CLOEXEC) != 0 || fcntl(p[0], F_GETFD) != FD_CLOEXEC)
        return 14;
    close(p[0]), close(p[1]);
    return 0;
}

static int names(void)
{
    struct stat st;
    char b[64];
    errno = 0;
    if (access("/tmp/f1", R_OK | W_OK) != 0 ||
        access("/tmp/none", F_OK) != -1 || errno != ENOENT)
        return 20;
    errno = 0;
    if (mkdir("/tmp/d", 0755) != 0 || mkdir("/tmp/d", 0755) != -1 ||
        errno != EEXIST)
        return 21;
    memset(b, 0, sizeof b);
    if (symlink("../f1", "/tmp/d/ln") != 0 ||
        readlink("/tmp/d/ln", b, sizeof b) != 5 || strcmp(b, "../f1"))
        return 22;
    if (lstat("/tmp/d/ln", &st) || !S_ISLNK(st.st_mode) ||
        stat("/tmp/d/ln", &st) || st.st_size != 3)
        return 23;
    if (link("/tmp/f1", "/tmp/d/hard") || stat("/tmp/f1", &st) ||
        st.st_nlink != 2 || rename("/tmp/d/hard", "/tmp/d/hard2") ||
        unlink("/tmp/d/hard2"))
        return 24;
    if (chmod("/tmp/f1", 0600) || stat("/tmp/f1", &st) ||
        (st.st_mode & 0777) != 0600 || umask(077) != 022 || umask(022) != 077)
        return 25;
    int dfd = open("/tmp/d", O_RDONLY | O_DIRECTORY);
    errno = 0;
    if (dfd < 0 || open("/tmp/f1", O_RDONLY | O_DIRECTORY) != -1 ||
        errno != ENOTDIR)
        return 26;
    int fd = openat(dfd, "at.txt", O_WRONLY | O_CREAT, 0644);
    if (fd < 0 || fstatat(dfd, "at.txt", &st, 0) || !S_ISREG(st.st_mode) ||
        mkdirat(dfd, "sub", 0755) || unlinkat(dfd, "at.txt", 0) ||
        unlinkat(dfd, "sub", AT_REMOVEDIR))
        return 27;
    close(fd);
    struct timespec ts[2] = { { 1000000, 0 }, { 2000000, 500 } };
    if (utimensat(AT_FDCWD, "/tmp/f1", ts, 0) || stat("/tmp/f1", &st) ||
        st.st_atime != 1000000 || st.st_mtime != 2000000 ||
        st.st_mtim.tv_nsec != 500)
        return 28;
    char cwd[256];
    if (chdir("/tmp/d") || !getcwd(cwd, sizeof cwd) || strcmp(cwd, "/tmp/d"))
        return 29;
    char *dyn = getcwd(NULL, 0);
    if (!dyn || strcmp(dyn, "/tmp/d"))
        return 30;
    free(dyn);
    errno = 0;
    if (getcwd(cwd, 3) || errno != ERANGE || fchdir(dfd) || chdir("/"))
        return 31;
    close(dfd);
    return 0;
}

static int directories(void)
{
    DIR *d = opendir("/tmp/d");
    if (!d || dirfd(d) < 0)
        return 40;
    int dots = 0, other = 0;
    long after_first = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            dots++;
        else if (strcmp(e->d_name, "ln") || e->d_type != DT_LNK || !e->d_ino)
            return 41;
        else
            other++;
        if (after_first < 0)
            after_first = telldir(d);
    }
    if (dots != 2 || other != 1)
        return 42;
    rewinddir(d);
    int again = 0;
    while (readdir(d))
        again++;
    seekdir(d, after_first);
    int rest = 0;
    while (readdir(d))
        rest++;
    if (again != 3 || rest != 2)
        return 43;
    closedir(d);
    struct dirent **list;
    int n = scandir("/tmp/d", &list, NULL, alphasort);
    if (n != 3 || strcmp(list[0]->d_name, ".") || strcmp(list[2]->d_name, "ln"))
        return 44;
    while (n)
        free(list[--n]);
    free(list);
    errno = 0;
    if (opendir("/tmp/nonexistent") || errno != ENOENT)
        return 45;
    return 0;
}
#endif

int main(void)
{
#ifdef EMBCC_TEST_HAS_FS
    int r;
    if ((r = descriptors()) || (r = names()) || (r = directories()))
        return r;
#else
    /* no filesystem: every one fails, and says why */
    struct stat st;
    errno = 0;
    if (stat("/tmp", &st) != -1 || errno == 0)
        return 50;
    errno = 0;
    if (opendir("/") || errno == 0)
        return 51;
    errno = 0;
    if (mkdir("/x", 0755) != -1 || errno == 0)
        return 52;
#endif
    return 42;
}
