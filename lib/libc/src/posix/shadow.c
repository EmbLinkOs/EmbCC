/* <shadow.h>: /etc/shadow, "name:password:lastchg:min:max:warn:inactive:
 * expire:flag", an empty numeric field being -1 (for the flag, ~0).
 * lckpwdf locks /etc/.pwd.lock for the editing that follows, as the
 * tools that change these files expect. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <shadow.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posix.h"

static long num(const char *s)
{
    if (!*s)
        return -1;
    char *end;
    long v = strtol(s, &end, 10);
    return *end ? -1 : v;
}

WEAK int sgetspent_r(const char *s, struct spwd *sp, char *buf, size_t n,
                     struct spwd **res)
{
    *res = NULL;
    size_t len = strcspn(s, "\r\n");
    if (len + 1 > n)
        return ERANGE;
    memcpy(buf, s, len);
    buf[len] = 0;
    char *f[9];
    char *p = buf;
    int k = 0;
    for (; k < 9; k++) {
        f[k] = p;
        char *c = strchr(p, ':');
        if (!c) {
            k++;
            break;
        }
        *c = 0;
        p = c + 1;
    }
    if (k < 2)
        return EINVAL;
    for (int i = k; i < 9; i++)
        f[i] = (char *)"";
    sp->sp_namp = f[0];
    sp->sp_pwdp = f[1];
    sp->sp_lstchg = num(f[2]);
    sp->sp_min = num(f[3]);
    sp->sp_max = num(f[4]);
    sp->sp_warn = num(f[5]);
    sp->sp_inact = num(f[6]);
    sp->sp_expire = num(f[7]);
    sp->sp_flag = *f[8] ? strtoul(f[8], 0, 10) : ~0UL;
    *res = sp;
    return 0;
}

static struct spwd g_sp;
static char g_spbuf[1024];

WEAK struct spwd *sgetspent(const char *s)
{
    struct spwd *r;
    errno = sgetspent_r(s, &g_sp, g_spbuf, sizeof g_spbuf, &r);
    return r;
}

WEAK int fgetspent_r(FILE *f, struct spwd *sp, char *buf, size_t n,
                     struct spwd **res)
{
    char line[1024];
    *res = NULL;
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        int e = sgetspent_r(line, sp, buf, n, res);
        if (e != EINVAL)
            return e;
    }
    return ENOENT;
}

WEAK struct spwd *fgetspent(FILE *f)
{
    struct spwd *r;
    errno = fgetspent_r(f, &g_sp, g_spbuf, sizeof g_spbuf, &r);
    return r;
}

static FILE *g_spent;

WEAK void setspent(void)
{
    if (g_spent)
        rewind(g_spent);
}

WEAK void endspent(void)
{
    if (g_spent)
        fclose(g_spent);
    g_spent = NULL;
}

WEAK int getspent_r(struct spwd *sp, char *buf, size_t n, struct spwd **res)
{
    if (!g_spent && !(g_spent = fopen(SHADOW, "re"))) {
        *res = NULL;
        return errno;
    }
    return fgetspent_r(g_spent, sp, buf, n, res);
}

WEAK struct spwd *getspent(void)
{
    struct spwd *r;
    errno = getspent_r(&g_sp, g_spbuf, sizeof g_spbuf, &r);
    return r;
}

WEAK int getspnam_r(const char *name, struct spwd *sp, char *buf, size_t n,
                    struct spwd **res)
{
    *res = NULL;
    FILE *f = fopen(SHADOW, "re");
    if (!f)
        return errno;
    int e;
    while ((e = fgetspent_r(f, sp, buf, n, res)) == 0)
        if (!strcmp(sp->sp_namp, name))
            break;
    fclose(f);
    if (e == ENOENT)
        e = 0;                 /* not there: *res NULL, no error */
    else if (e == 0 && strcmp(sp->sp_namp, name))
        *res = NULL;
    return e;
}

WEAK struct spwd *getspnam(const char *name)
{
    struct spwd *r;
    int e = getspnam_r(name, &g_sp, g_spbuf, sizeof g_spbuf, &r);
    if (e)
        errno = e;
    return r;
}

static void field(FILE *f, long v)
{
    if (v != -1)
        fprintf(f, "%ld", v);
    fputc(':', f);
}

WEAK int putspent(const struct spwd *sp, FILE *f)
{
    if (!sp || !f || !sp->sp_namp) {
        errno = EINVAL;
        return -1;
    }
    flockfile(f);
    fprintf(f, "%s:%s:", sp->sp_namp, sp->sp_pwdp ? sp->sp_pwdp : "");
    field(f, sp->sp_lstchg);
    field(f, sp->sp_min);
    field(f, sp->sp_max);
    field(f, sp->sp_warn);
    field(f, sp->sp_inact);
    field(f, sp->sp_expire);
    if (sp->sp_flag != ~0UL)
        fprintf(f, "%lu", sp->sp_flag);
    int r = fputc('\n', f) == EOF ? -1 : 0;
    funlockfile(f);
    return r;
}

static int g_lockfd = -1;

WEAK int lckpwdf(void)
{
    if (g_lockfd >= 0)
        return 0;
    int fd = open("/etc/.pwd.lock", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    struct flock l = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    if (fcntl(fd, F_SETLKW, &l) < 0) {
        close(fd);
        return -1;
    }
    g_lockfd = fd;
    return 0;
}

WEAK int ulckpwdf(void)
{
    if (g_lockfd < 0)
        return -1;
    close(g_lockfd);           /* closing it releases the lock */
    g_lockfd = -1;
    return 0;
}
