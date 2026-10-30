/* Pseudo-terminals: POSIX's posix_openpt, grantpt, unlockpt and ptsname,
 * and BSD's openpty, forkpty and login_tty over them. */
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <pty.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "posix.h"

WEAK int posix_openpt(int flags) { return __os_openpt(flags); }

/* Where the kernel makes the other side with the right owner and mode --
 * Linux's devpts does -- there is nothing to grant; the call still
 * checks that fd is a controlling side. */
WEAK int grantpt(int fd)
{
    char name[64];
    if (__os_ptsname(fd, name, sizeof name) < 0) {
        if (errno == ENOTTY)
            errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK int unlockpt(int fd) { return __os_unlockpt(fd); }

WEAK int ptsname_r(int fd, char *buf, size_t n)
{
    int saved = errno;
    if (__os_ptsname(fd, buf, n) == 0)
        return 0;
    int e = errno;
    errno = saved;
    return e;
}

WEAK char *ptsname(int fd)
{
    static char buf[64];
    int e = ptsname_r(fd, buf, sizeof buf);
    if (e) {
        errno = e;
        return NULL;
    }
    return buf;
}

WEAK int openpty(int *master, int *slave, char *name, const struct termios *t,
                 const struct winsize *ws)
{
    char buf[64];
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    if (grantpt(m) < 0 || unlockpt(m) < 0 || ptsname_r(m, buf, sizeof buf)) {
        close(m);
        return -1;
    }
    int s = open(buf, O_RDWR | O_NOCTTY);
    if (s < 0) {
        close(m);
        return -1;
    }
    if (t)
        tcsetattr(s, TCSANOW, t);
    if (ws)
        ioctl(s, TIOCSWINSZ, ws);
    if (name)
        strcpy(name, buf);
    *master = m;
    *slave = s;
    return 0;
}

/* the terminal fd as this process's controlling one, and its standard
 * input, output and error */
WEAK int login_tty(int fd)
{
    setsid();
    if (ioctl(fd, TIOCSCTTY, 0) < 0)
        return -1;
    for (int i = 0; i < 3; i++)
        if (dup2(fd, i) < 0)
            return -1;
    if (fd > 2)
        close(fd);
    return 0;
}

WEAK pid_t forkpty(int *master, char *name, const struct termios *t,
                   const struct winsize *ws)
{
    int m, s;
    if (openpty(&m, &s, name, t, ws) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(m);
        close(s);
        return -1;
    }
    if (pid == 0) {
        close(m);
        if (login_tty(s) < 0)
            _exit(1);
        return 0;
    }
    close(s);
    *master = m;
    return pid;
}
