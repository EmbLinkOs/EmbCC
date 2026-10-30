/* <syslog.h>: RFC 3164's line, "<PRI>Mmm dd hh:mm:ss ident[pid]: text",
 * to the datagram socket /dev/log, as glibc writes it; the connection is
 * made at the first message (or at openlog with LOG_NDELAY) and made
 * again if the logger went away. %m is strerror(errno), taken before
 * anything here can change errno. */
#define _GNU_SOURCE 1
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "posix.h"
#include "../internal/lock.h"

static __lock_t g_log_lock = LOCK_INIT;
static const char *g_ident;
static int g_option, g_facility = LOG_USER, g_mask = 0xff, g_fd = -1;

static void connect_log(void)
{
    if (g_fd >= 0)
        return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/dev/log");
    int types[2] = { SOCK_DGRAM, SOCK_STREAM };
    for (int i = 0; i < 2; i++) {
        int fd = socket(AF_UNIX, types[i] | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            g_fd = fd;
            return;
        }
        close(fd);
    }
}

WEAK void openlog(const char *ident, int option, int facility)
{
    __lock(&g_log_lock);
    g_ident = ident;
    g_option = option;
    if (facility && !(facility & ~LOG_FACMASK))
        g_facility = facility;
    if (option & LOG_NDELAY)
        connect_log();
    __unlock(&g_log_lock);
}

WEAK void closelog(void)
{
    __lock(&g_log_lock);
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    g_ident = NULL;
    __unlock(&g_log_lock);
}

WEAK int setlogmask(int mask)
{
    __lock(&g_log_lock);
    int old = g_mask;
    if (mask)
        g_mask = mask;
    __unlock(&g_log_lock);
    return old;
}

static void write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

WEAK void vsyslog(int priority, const char *format, va_list ap)
{
    int saved = errno;
    if (!(g_mask & LOG_MASK(LOG_PRI(priority))) ||
        (priority & ~(LOG_PRIMASK | LOG_FACMASK)))
        return;
    /* %m first: it is errno as the caller left it */
    char fmt[512];
    size_t o = 0;
    for (const char *p = format; *p && o + 1 < sizeof fmt; p++) {
        if (p[0] == '%' && p[1] == 'm') {
            const char *e = strerror(saved);
            for (; *e && o + 1 < sizeof fmt; e++)
                fmt[o++] = *e == '%' ? '?' : *e;
            p++;
        } else if (p[0] == '%' && p[1] == '%' && o + 2 < sizeof fmt) {
            fmt[o++] = *p++;
            fmt[o++] = *p;
        } else {
            fmt[o++] = *p;
        }
    }
    fmt[o] = 0;

    __lock(&g_log_lock);
    if (!(priority & LOG_FACMASK))
        priority |= g_facility;
    char line[1024], stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || !strftime(stamp, sizeof stamp, "%b %e %T", &tm))
        strcpy(stamp, "Jan  1 00:00:00");
    const char *ident = g_ident ? g_ident : program_invocation_short_name;
    int head = snprintf(line, sizeof line, "<%d>%s ", priority, stamp);
    int msg = head;
    msg += snprintf(line + head, sizeof line - (size_t)head,
                    (g_option & LOG_PID) ? "%s[%d]: " : "%s: ",
                    ident ? ident : "", (int)getpid());
    if (msg >= (int)sizeof line)
        msg = (int)sizeof line - 1;
    vsnprintf(line + msg, sizeof line - (size_t)msg, fmt, ap);
    size_t len = strlen(line);

    if (g_option & LOG_PERROR) {
        write_all(2, line + head, len - (size_t)head);
        if (!len || line[len - 1] != '\n')
            write_all(2, "\n", 1);
    }
    connect_log();
    int sent = g_fd >= 0 && send(g_fd, line, len, MSG_NOSIGNAL) >= 0;
    if (!sent && g_fd >= 0) {          /* the logger restarted: once more */
        close(g_fd);
        g_fd = -1;
        connect_log();
        sent = g_fd >= 0 && send(g_fd, line, len, MSG_NOSIGNAL) >= 0;
    }
    if (!sent) {
        int fd = (g_option & LOG_CONS) ? open("/dev/console",
                                              O_WRONLY | O_NOCTTY | O_CLOEXEC)
                                       : -1;
        if (fd >= 0 || !(g_option & LOG_PERROR)) {
            int to = fd >= 0 ? fd : 2;
            write_all(to, line + head, len - (size_t)head);
            if (!len || line[len - 1] != '\n')
                write_all(to, "\n", 1);
        }
        if (fd >= 0)
            close(fd);
    }
    __unlock(&g_log_lock);
    errno = saved;
}

WEAK void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}
