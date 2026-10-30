/* <utmpx.h> and <utmp.h>: the login records, one fixed-size record
 * after another in /var/run/utmp (or the file utmpxname names), read and
 * rewritten in place; and wtmp, which is only ever appended to. A system
 * without the file has no one logged in. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>
#include <utmpx.h>
#include <sys/time.h>

#include "posix.h"

static char g_path[256] = "/var/run/utmp";
static int g_fd = -1;
static struct utmpx g_ut;

static int ut_open(void)
{
    if (g_fd < 0) {
        g_fd = open(g_path, O_RDWR | O_CLOEXEC);
        if (g_fd < 0)
            g_fd = open(g_path, O_RDONLY | O_CLOEXEC);
    }
    return g_fd;
}

WEAK void setutxent(void)
{
    if (g_fd >= 0)
        lseek(g_fd, 0, SEEK_SET);
}

WEAK void endutxent(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
}

WEAK int utmpxname(const char *file)
{
    if (strlen(file) >= sizeof g_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    endutxent();
    strcpy(g_path, file);
    return 0;
}

WEAK struct utmpx *getutxent(void)
{
    if (ut_open() < 0)
        return NULL;
    return read(g_fd, &g_ut, sizeof g_ut) == (ssize_t)sizeof g_ut ? &g_ut : NULL;
}

/* the record ut names: same type for the system's own records, same id
 * for any of the four process kinds */
static int same_id(const struct utmpx *a, const struct utmpx *ut)
{
    if (ut->ut_type >= RUN_LVL && ut->ut_type <= OLD_TIME)
        return a->ut_type == ut->ut_type;
    if (ut->ut_type >= INIT_PROCESS && ut->ut_type <= DEAD_PROCESS)
        return a->ut_type >= INIT_PROCESS && a->ut_type <= DEAD_PROCESS &&
               !strncmp(a->ut_id, ut->ut_id, sizeof ut->ut_id);
    return 0;
}

WEAK struct utmpx *getutxid(const struct utmpx *ut)
{
    struct utmpx *e;
    while ((e = getutxent()))
        if (same_id(e, ut))
            return e;
    return NULL;
}

WEAK struct utmpx *getutxline(const struct utmpx *ut)
{
    struct utmpx *e;
    while ((e = getutxent()))
        if ((e->ut_type == LOGIN_PROCESS || e->ut_type == USER_PROCESS) &&
            !strncmp(e->ut_line, ut->ut_line, sizeof ut->ut_line))
            return e;
    return NULL;
}

/* over the record getutxid would find from where reading has got to, or
 * at the end -- POSIX's rule, and glibc's; under a write lock so two
 * logins do not take the same empty slot */
WEAK struct utmpx *pututxline(const struct utmpx *ut)
{
    struct utmpx copy = *ut;
    if (ut_open() < 0)
        return NULL;
    struct flock l = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    fcntl(g_fd, F_SETLKW, &l);
    struct utmpx e;
    off_t at = -1, pos = lseek(g_fd, 0, SEEK_CUR);
    while (read(g_fd, &e, sizeof e) == (ssize_t)sizeof e) {
        if (same_id(&e, &copy)) {
            at = pos;
            break;
        }
        pos += (off_t)sizeof e;
    }
    if (at < 0)
        at = lseek(g_fd, 0, SEEK_END);
    int ok = at >= 0 && pwrite(g_fd, &copy, sizeof copy, at) == (ssize_t)sizeof copy;
    l.l_type = F_UNLCK;
    fcntl(g_fd, F_SETLK, &l);
    if (!ok)
        return NULL;
    lseek(g_fd, at + (off_t)sizeof copy, SEEK_SET);
    g_ut = copy;
    return &g_ut;
}

WEAK void updwtmpx(const char *file, const struct utmpx *ut)
{
    int fd = open(file, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        return;
    write(fd, ut, sizeof *ut);
    close(fd);
}

/* ---- <utmp.h>'s names ---------------------------------------------------------- */

WEAK void setutent(void) { setutxent(); }
WEAK void endutent(void) { endutxent(); }
WEAK struct utmp *getutent(void) { return getutxent(); }
WEAK struct utmp *getutid(const struct utmp *ut) { return getutxid(ut); }
WEAK struct utmp *getutline(const struct utmp *ut) { return getutxline(ut); }
WEAK struct utmp *pututline(const struct utmp *ut) { return pututxline(ut); }
WEAK int utmpname(const char *file) { return utmpxname(file); }
WEAK void updwtmp(const char *file, const struct utmp *ut) { updwtmpx(file, ut); }

static int found(struct utmp *e, struct utmp *buf, struct utmp **res)
{
    if (!e) {
        *res = NULL;
        return -1;
    }
    *buf = *e;
    *res = buf;
    return 0;
}

WEAK int getutent_r(struct utmp *buf, struct utmp **res)
{
    return found(getutxent(), buf, res);
}

WEAK int getutid_r(const struct utmp *ut, struct utmp *buf, struct utmp **res)
{
    return found(getutxid(ut), buf, res);
}

WEAK int getutline_r(const struct utmp *ut, struct utmp *buf, struct utmp **res)
{
    return found(getutxline(ut), buf, res);
}

WEAK void logwtmp(const char *line, const char *name, const char *host)
{
    struct utmpx u;
    memset(&u, 0, sizeof u);
    u.ut_type = name && *name ? USER_PROCESS : DEAD_PROCESS;
    u.ut_pid = getpid();
    strncpy(u.ut_line, line, sizeof u.ut_line);
    strncpy(u.ut_user, name, sizeof u.ut_user);
    strncpy(u.ut_host, host, sizeof u.ut_host);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    u.ut_tv.tv_sec = (int32_t)tv.tv_sec;
    u.ut_tv.tv_usec = (int32_t)tv.tv_usec;
    updwtmpx(_PATH_WTMP, &u);
}

/* a login: in utmp, on the terminal's own line, and in wtmp */
WEAK void login(const struct utmp *ut)
{
    struct utmpx u = *ut;
    u.ut_type = USER_PROCESS;
    u.ut_pid = getpid();
    const char *tty = ttyname(0);
    if (!tty)
        tty = ttyname(1);
    if (tty) {
        if (!strncmp(tty, "/dev/", 5))
            tty += 5;
        strncpy(u.ut_line, tty, sizeof u.ut_line);
    }
    setutxent();
    pututxline(&u);
    endutxent();
    updwtmpx(_PATH_WTMP, &u);
}

WEAK int logout(const char *line)
{
    struct utmpx key, *e;
    memset(&key, 0, sizeof key);
    strncpy(key.ut_line, line, sizeof key.ut_line);
    setutxent();
    int r = 0;
    if ((e = getutxline(&key))) {
        struct utmpx u = *e;
        u.ut_type = DEAD_PROCESS;
        memset(u.ut_user, 0, sizeof u.ut_user);
        memset(u.ut_host, 0, sizeof u.ut_host);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        u.ut_tv.tv_sec = (int32_t)tv.tv_sec;
        u.ut_tv.tv_usec = (int32_t)tv.tv_usec;
        r = pututxline(&u) != NULL;
    }
    endutxent();
    return r;
}
