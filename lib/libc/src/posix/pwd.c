/* <pwd.h> and <grp.h>: the user and group databases, /etc/passwd and
 * /etc/group. A system with neither file -- EmbLinkOS has no users --
 * finds nobody, which is the answer the databases give there.
 *
 * The _r forms parse into the caller's buffer; the others into one
 * static buffer each, as POSIX permits. */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "posix.h"

/* ---- users ------------------------------------------------------------------ */

/* "name:passwd:uid:gid:gecos:dir:shell" copied into buf and split there */
static int parse_pw(const char *line, struct passwd *pw, char *buf, size_t n)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    if (len + 1 > n)
        return ERANGE;
    memcpy(buf, line, len);
    buf[len] = 0;
    char *f[7];
    char *p = buf;
    for (int i = 0; i < 7; i++) {
        f[i] = p;
        char *c = i < 6 ? strchr(p, ':') : NULL;
        if (i < 6 && !c)
            return -1;              /* not a passwd line */
        if (c) {
            *c = 0;
            p = c + 1;
        }
    }
    pw->pw_name = f[0];
    pw->pw_passwd = f[1];
    pw->pw_uid = (uid_t)strtoul(f[2], NULL, 10);
    pw->pw_gid = (gid_t)strtoul(f[3], NULL, 10);
    pw->pw_gecos = f[4];
    pw->pw_dir = f[5];
    pw->pw_shell = f[6];
    return 0;
}

/* name or uid, whichever is asked */
static int find_pw(const char *name, uid_t uid, struct passwd *pw, char *buf,
                   size_t n, struct passwd **result)
{
    *result = NULL;
    int saved = errno;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) {
        errno = saved;
        return 0;                   /* no database: nobody */
    }
    char line[1024];
    int r = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        int e = parse_pw(line, pw, buf, n);
        if (e == ERANGE) {
            r = ERANGE;
            break;
        }
        if (e)
            continue;
        if (name ? !strcmp(pw->pw_name, name) : pw->pw_uid == uid) {
            *result = pw;
            break;
        }
    }
    fclose(f);
    errno = saved;
    return r;
}

WEAK int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t n,
                    struct passwd **result)
{
    return find_pw(name, 0, pw, buf, n, result);
}

WEAK int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t n,
                    struct passwd **result)
{
    return find_pw(NULL, uid, pw, buf, n, result);
}

static struct passwd g_pw;
static char g_pwbuf[1024];

WEAK struct passwd *getpwnam(const char *name)
{
    struct passwd *r;
    int e = getpwnam_r(name, &g_pw, g_pwbuf, sizeof g_pwbuf, &r);
    if (e)
        errno = e;
    return r;
}

WEAK struct passwd *getpwuid(uid_t uid)
{
    struct passwd *r;
    int e = getpwuid_r(uid, &g_pw, g_pwbuf, sizeof g_pwbuf, &r);
    if (e)
        errno = e;
    return r;
}

static FILE *g_pwent;

WEAK void setpwent(void)
{
    if (g_pwent)
        rewind(g_pwent);
}

WEAK void endpwent(void)
{
    if (g_pwent)
        fclose(g_pwent);
    g_pwent = NULL;
}

WEAK struct passwd *getpwent(void)
{
    if (!g_pwent && !(g_pwent = fopen("/etc/passwd", "r")))
        return NULL;
    char line[1024];
    while (fgets(line, sizeof line, g_pwent))
        if (line[0] != '#' &&
            parse_pw(line, &g_pw, g_pwbuf, sizeof g_pwbuf) == 0)
            return &g_pw;
    return NULL;
}

/* ---- groups ------------------------------------------------------------------- */

/* "name:passwd:gid:mem1,mem2" into buf: the strings, then the aligned
 * member vector after them */
static int parse_gr(const char *line, struct group *g, char *buf, size_t n)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    size_t members = 1;
    for (size_t i = 0; i < len; i++)
        members += line[i] == ',';
    size_t strs = (len + 1 + sizeof(char *) - 1) & ~(sizeof(char *) - 1);
    if (strs + (members + 1) * sizeof(char *) > n)
        return ERANGE;
    memcpy(buf, line, len);
    buf[len] = 0;
    char *f[4], *p = buf;
    for (int i = 0; i < 4; i++) {
        f[i] = p;
        char *c = i < 3 ? strchr(p, ':') : NULL;
        if (i < 3 && !c)
            return -1;
        if (c) {
            *c = 0;
            p = c + 1;
        }
    }
    char **mem = (char **)(void *)(buf + strs);
    size_t k = 0;
    for (char *m = f[3]; *m; ) {
        mem[k++] = m;
        char *c = strchr(m, ',');
        if (!c)
            break;
        *c = 0;
        m = c + 1;
    }
    mem[k] = NULL;
    g->gr_name = f[0];
    g->gr_passwd = f[1];
    g->gr_gid = (gid_t)strtoul(f[2], NULL, 10);
    g->gr_mem = mem;
    return 0;
}

static int find_gr(const char *name, gid_t gid, struct group *g, char *buf,
                   size_t n, struct group **result)
{
    *result = NULL;
    int saved = errno;
    FILE *f = fopen("/etc/group", "r");
    if (!f) {
        errno = saved;
        return 0;
    }
    char line[1024];
    int r = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        int e = parse_gr(line, g, buf, n);
        if (e == ERANGE) {
            r = ERANGE;
            break;
        }
        if (e)
            continue;
        if (name ? !strcmp(g->gr_name, name) : g->gr_gid == gid) {
            *result = g;
            break;
        }
    }
    fclose(f);
    errno = saved;
    return r;
}

WEAK int getgrnam_r(const char *name, struct group *g, char *buf, size_t n,
                    struct group **result)
{
    return find_gr(name, 0, g, buf, n, result);
}

WEAK int getgrgid_r(gid_t gid, struct group *g, char *buf, size_t n,
                    struct group **result)
{
    return find_gr(NULL, gid, g, buf, n, result);
}

static struct group g_gr;
static char g_grbuf[2048];

WEAK struct group *getgrnam(const char *name)
{
    struct group *r;
    int e = getgrnam_r(name, &g_gr, g_grbuf, sizeof g_grbuf, &r);
    if (e)
        errno = e;
    return r;
}

WEAK struct group *getgrgid(gid_t gid)
{
    struct group *r;
    int e = getgrgid_r(gid, &g_gr, g_grbuf, sizeof g_grbuf, &r);
    if (e)
        errno = e;
    return r;
}

static FILE *g_grent;

WEAK void setgrent(void)
{
    if (g_grent)
        rewind(g_grent);
}

WEAK void endgrent(void)
{
    if (g_grent)
        fclose(g_grent);
    g_grent = NULL;
}

WEAK struct group *getgrent(void)
{
    if (!g_grent && !(g_grent = fopen("/etc/group", "r")))
        return NULL;
    char line[1024];
    while (fgets(line, sizeof line, g_grent))
        if (line[0] != '#' &&
            parse_gr(line, &g_gr, g_grbuf, sizeof g_grbuf) == 0)
            return &g_gr;
    return NULL;
}

/* The groups user is in: group first, then each group that lists the
 * user, at most *n of them; the count, or -1 with *n the count needed. */
WEAK int getgrouplist(const char *user, gid_t group, gid_t *groups, int *n)
{
    int k = 0, cap = *n;
    if (k < cap)
        groups[k] = group;
    k++;
    FILE *f = fopen("/etc/group", "r");
    char line[1024], buf[2048];
    struct group g;
    while (f && fgets(line, sizeof line, f)) {
        if (parse_gr(line, &g, buf, sizeof buf) || g.gr_gid == group)
            continue;
        for (char **m = g.gr_mem; *m; m++)
            if (!strcmp(*m, user)) {
                if (k < cap)
                    groups[k] = g.gr_gid;
                k++;
                break;
            }
    }
    if (f)
        fclose(f);
    int fit = k <= cap;
    *n = k;
    return fit ? k : -1;
}

WEAK int setgroups(size_t n, const gid_t *groups)
{
    return __os_setgroups((int)n, (const unsigned *)groups);
}

WEAK int initgroups(const char *user, gid_t group)
{
    gid_t list[NGROUPS_MAX];
    int n = NGROUPS_MAX;
    if (getgrouplist(user, group, list, &n) < 0)
        n = NGROUPS_MAX;
    return setgroups((size_t)n, list);
}
