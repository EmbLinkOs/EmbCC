/* <termios.h>: terminal modes through the seam's struct, which is
 * glibc's layout -- and the line controls (drain, flush, flow, break)
 * and job control through ioctls, whose requests are Linux's. */
#define _GNU_SOURCE 1
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "posix.h"

_Static_assert(sizeof(struct termios) == sizeof(struct __os_termios),
               "struct termios is not the seam's");

WEAK int tcgetattr(int fd, struct termios *t)
{
    return __os_tcgetattr(fd, (struct __os_termios *)(void *)t);
}

WEAK int tcsetattr(int fd, int action, const struct termios *t)
{
    return __os_tcsetattr(fd, action,
                          (const struct __os_termios *)(const void *)t);
}

/* The speed is the CBAUD bits of c_cflag, as glibc keeps it; the
 * c_ispeed and c_ospeed fields follow along. */
WEAK speed_t cfgetospeed(const struct termios *t) { return t->c_cflag & CBAUD; }
WEAK speed_t cfgetispeed(const struct termios *t) { return cfgetospeed(t); }

WEAK int cfsetospeed(struct termios *t, speed_t s)
{
    if (s & ~CBAUD) {
        errno = EINVAL;
        return -1;
    }
    t->c_cflag = (t->c_cflag & ~CBAUD) | s;
    t->c_ospeed = s;
    return 0;
}

/* 0 means "the same as the output speed", as POSIX says */
WEAK int cfsetispeed(struct termios *t, speed_t s)
{
    if (s & ~CBAUD) {
        errno = EINVAL;
        return -1;
    }
    if (s)
        t->c_cflag = (t->c_cflag & ~CBAUD) | s;
    t->c_ispeed = s ? s : t->c_ospeed;
    return 0;
}

WEAK int cfsetspeed(struct termios *t, speed_t s)
{
    if (cfsetospeed(t, s) < 0)
        return -1;
    return cfsetispeed(t, s);
}

/* the raw mode BSD's cfmakeraw makes */
WEAK void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                    ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

WEAK int tcdrain(int fd) { return __os_ioctl(fd, TCSBRK, (void *)1); }

WEAK int tcflush(int fd, int queue)
{
    return __os_ioctl(fd, TCFLSH, (void *)(long)queue);
}

WEAK int tcflow(int fd, int action)
{
    return __os_ioctl(fd, TCXONC, (void *)(long)action);
}

WEAK int tcsendbreak(int fd, int duration)
{
    (void)duration;
    return __os_ioctl(fd, TCSBRK, (void *)0);
}

WEAK pid_t tcgetpgrp(int fd)
{
    int pgrp;
    return __os_ioctl(fd, TIOCGPGRP, &pgrp) < 0 ? -1 : pgrp;
}

WEAK int tcsetpgrp(int fd, pid_t pgrp)
{
    int p = pgrp;
    return __os_ioctl(fd, TIOCSPGRP, &p);
}

WEAK pid_t tcgetsid(int fd)
{
    int sid;
    return __os_ioctl(fd, TIOCGSID, &sid) < 0 ? -1 : sid;
}

WEAK int tcgetwinsize(int fd, struct winsize *ws)
{
    return __os_ioctl(fd, TIOCGWINSZ, ws);
}

WEAK int tcsetwinsize(int fd, const struct winsize *ws)
{
    return __os_ioctl(fd, TIOCSWINSZ, (void *)ws);
}

WEAK int ttyname_r(int fd, char *buf, size_t n)
{
    int saved = errno;
    if (__os_ttyname(fd, buf, n) == 0)
        return 0;
    int e = errno;
    errno = saved;
    return e;
}

WEAK char *ttyname(int fd)
{
    static char buf[64];
    int e = ttyname_r(fd, buf, sizeof buf);
    if (e) {
        errno = e;
        return NULL;
    }
    return buf;
}
