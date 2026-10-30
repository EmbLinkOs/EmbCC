/* XSI's fmtmsg: "label: SEVERITY: text" and "TO FIX: action  tag", to
 * standard error (MM_PRINT) and to /dev/console (MM_CONSOLE), with only
 * the parts $MSGVERB names when it names any -- label, severity, text,
 * action, tag, colon-separated. */
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <fmtmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WEAK __attribute__((weak))

static int wanted(const char *verb, const char *part)
{
    if (!verb || !*verb)
        return 1;
    size_t n = strlen(part);
    for (const char *p = verb; *p;) {
        size_t k = strcspn(p, ":");
        if (k == n && !strncmp(p, part, n))
            return 1;
        p += k;
        if (*p)
            p++;
    }
    return 0;
}

static int compose(char *buf, size_t n, const char *verb, const char *label,
                   int severity, const char *text, const char *action,
                   const char *tag)
{
    static const char *const sev[] = { 0, "HALT", "ERROR", "WARNING", "INFO" };
    int use_l = label && wanted(verb, "label");
    int use_s = severity > 0 && severity < 5 && wanted(verb, "severity");
    int use_t = text && wanted(verb, "text");
    int use_a = action && wanted(verb, "action");
    int use_g = tag && wanted(verb, "tag");
    size_t k = 0;
    k += (size_t)snprintf(buf + k, n - k, "%s%s", use_l ? label : "",
                          use_l && (use_s || use_t) ? ": " : "");
    k += (size_t)snprintf(buf + k, n - k, "%s%s", use_s ? sev[severity] : "",
                          use_s && use_t ? ": " : "");
    if (use_t)
        k += (size_t)snprintf(buf + k, n - k, "%s", text);
    if (use_a || use_g)
        k += (size_t)snprintf(buf + k, n - k, "%s", k ? "\n" : "");
    if (use_a)
        k += (size_t)snprintf(buf + k, n - k, "TO FIX: %s", action);
    if (use_g)
        k += (size_t)snprintf(buf + k, n - k, "%s%s", use_a ? "  " : "", tag);
    if (k < n)
        buf[k++] = '\n';
    buf[k < n ? k : n - 1] = 0;
    return (int)k;
}

WEAK int fmtmsg(long cls, const char *label, int severity, const char *text,
                const char *action, const char *tag)
{
    char buf[1024];
    int failed_print = 0, failed_console = 0;
    if (cls & MM_PRINT) {
        compose(buf, sizeof buf, getenv("MSGVERB"), label, severity, text,
                action, tag);
        if (fputs(buf, stderr) == EOF)
            failed_print = 1;
    }
    if (cls & MM_CONSOLE) {
        int len = compose(buf, sizeof buf, 0, label, severity, text, action, tag);
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (fd < 0 || write(fd, buf, (size_t)len) != len)
            failed_console = 1;
        if (fd >= 0)
            close(fd);
    }
    if (failed_print && failed_console)
        return MM_NOTOK;
    if (failed_print)
        return MM_NOMSG;
    if (failed_console)
        return MM_NOCON;
    return MM_OK;
}
