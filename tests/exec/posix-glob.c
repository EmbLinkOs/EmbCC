/* glob, nftw and ftw over a small tree, and syslog's LOG_PERROR line:
 * every answer is glibc 2.36's for the same calls. */
// expect-exit: 42
// needs-libc: <glob.h>, <ftw.h> and <syslog.h>, from lib/libc
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef EMBCC_TEST_HAS_FS
#define R "/tmp/gt"

static void put(const char *p)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

static void tree(void)
{
    mkdir(R, 0755);
    mkdir(R "/d1", 0755);
    mkdir(R "/d2", 0755);
    mkdir(R "/d1/sub", 0755);
    put(R "/a.c");
    put(R "/b.c");
    put(R "/c.h");
    put(R "/.hidden.c");
    put(R "/d1/x.c");
    put(R "/d1/sub/y.c");
    put(R "/d2/z.txt");
    put(R "/we*rd");
    symlink("d1", R "/link");
}

static int same(glob_t *g, const char *const *want)
{
    size_t n = 0;
    while (want[n])
        n++;
    if (g->gl_pathc != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(g->gl_pathv[g->gl_offs + i], want[i]))
            return 0;
    return g->gl_pathv[g->gl_offs + n] == NULL;
}

static int globs(void)
{
    glob_t g;
    if (glob(R "/*.c", 0, NULL, &g) ||
        !same(&g, (const char *[]){ R "/a.c", R "/b.c", NULL }))
        return 1;
    globfree(&g);
    if (glob(R "/*/*.c", 0, NULL, &g) ||
        !same(&g, (const char *[]){ R "/d1/x.c", R "/link/x.c", NULL }))
        return 2;
    globfree(&g);
    if (glob(R "/d*", GLOB_MARK, NULL, &g) ||
        !same(&g, (const char *[]){ R "/d1/", R "/d2/", NULL }))
        return 3;
    globfree(&g);
    /* a leading dot only by a dot; GLOB_PERIOD lets a wildcard have it */
    if (glob(R "/.*.c", 0, NULL, &g) ||
        !same(&g, (const char *[]){ R "/.hidden.c", NULL }))
        return 4;
    globfree(&g);
    if (glob(R "/*.c", GLOB_PERIOD, NULL, &g) ||
        !same(&g, (const char *[]){ R "/.hidden.c", R "/a.c", R "/b.c", NULL }))
        return 5;
    globfree(&g);
    if (glob(R "/*.zz", 0, NULL, &g) != GLOB_NOMATCH)
        return 6;
    if (glob(R "/*.zz", GLOB_NOCHECK, NULL, &g) ||
        !same(&g, (const char *[]){ R "/*.zz", NULL }))
        return 7;
    globfree(&g);
    /* an escaped star is a star */
    if (glob(R "/we\\*rd", 0, NULL, &g) ||
        !same(&g, (const char *[]){ R "/we*rd", NULL }))
        return 8;
    globfree(&g);
    /* offsets, then appending */
    g.gl_offs = 2;
    if (glob(R "/a.c", GLOB_DOOFFS, NULL, &g) ||
        glob(R "/c.*", GLOB_DOOFFS | GLOB_APPEND, NULL, &g) ||
        g.gl_pathv[0] || g.gl_pathv[1] ||
        !same(&g, (const char *[]){ R "/a.c", R "/c.h", NULL }))
        return 9;
    globfree(&g);
    /* GNU's braces, each alternative in order */
    if (glob(R "/{c,a}.?", GLOB_BRACE, NULL, &g) ||
        !same(&g, (const char *[]){ R "/c.h", R "/a.c", NULL }))
        return 10;
    globfree(&g);
    if (glob(R "/d?", GLOB_ONLYDIR, NULL, &g) ||
        !same(&g, (const char *[]){ R "/d1", R "/d2", NULL }) ||
        !(g.gl_flags & GLOB_MAGCHAR))
        return 11;
    globfree(&g);
    setenv("HOME", R, 1);
    if (glob("~/b.*", GLOB_TILDE, NULL, &g) ||
        !same(&g, (const char *[]){ R "/b.c", NULL }))
        return 12;
    globfree(&g);
    if (!glob_pattern_p("a[bc]", 1) || glob_pattern_p("a\\*", 1) ||
        !glob_pattern_p("a\\*", 0))
        return 13;
    return 0;
}

static char g_seen[512];
static int g_n;

static int note(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)st;
    char b[96];
    snprintf(b, sizeof b, "%d:%d:%s:%s;", type, f->level, p + strlen(R),
             p + f->base);
    strcat(g_seen, b);
    g_n++;
    return 0;
}

static int prune(const char *p, const struct stat *st, int type,
                 struct FTW *f)
{
    (void)st;
    (void)f;
    g_n++;
    if (type == FTW_D && !strcmp(p + f->base, "d1"))
        return FTW_SKIP_SUBTREE;
    return 0;
}

static int g_old;
static int old_fn(const char *p, const struct stat *st, int type)
{
    (void)p;
    (void)st;
    if (type == FTW_F)
        g_old++;
    return 0;
}

static int walks(void)
{
    /* the order inside a directory is readdir's, so count and check
     * shapes rather than a sequence */
    if (nftw(R "/d1", note, 4, FTW_PHYS) || g_n != 4 ||
        !strstr(g_seen, "1:0:/d1:d1;") || !strstr(g_seen, "0:1:/d1/x.c:x.c;") ||
        !strstr(g_seen, "1:1:/d1/sub:sub;") ||
        !strstr(g_seen, "0:2:/d1/sub/y.c:y.c;"))
        return 20;
    g_seen[0] = 0;
    g_n = 0;
    if (nftw(R "/d1", note, 4, FTW_PHYS | FTW_DEPTH) || g_n != 4 ||
        !strstr(g_seen, "5:0:/d1:d1;") ||
        strstr(g_seen, "5:0:/d1:d1;") != g_seen + strlen(g_seen) - 11)
        return 21;
    /* FTW_PHYS: the link is a link; without it, a directory */
    g_seen[0] = 0;
    g_n = 0;
    nftw(R, note, 4, FTW_PHYS);
    if (!strstr(g_seen, "4:1:/link:link;"))
        return 22;
    g_n = 0;
    if (nftw(R, prune, 4, FTW_PHYS | FTW_ACTIONRETVAL) || g_n != 10)
        return 23;
    if (ftw(R "/d1", old_fn, 4) || g_old != 2)
        return 24;
    if (nftw(R "/nothere", note, 4, 0) != -1 || errno != ENOENT)
        return 25;
    return 0;
}

static int logs(void)
{
    int p[2];
    if (pipe(p))
        return 30;
    int saved = dup(2);
    dup2(p[1], 2);
    openlog("gtest", LOG_PERROR, LOG_USER);
    errno = ENOENT;
    syslog(LOG_ERR, "x=%d %m", 5);
    setlogmask(LOG_UPTO(LOG_WARNING));
    syslog(LOG_INFO, "not shown");
    closelog();
    dup2(saved, 2);
    close(p[1]);
    char b[200] = { 0 };
    ssize_t n = read(p[0], b, sizeof b - 1);
    close(p[0]);
    return n > 0 && !strcmp(b, "gtest: x=5 No such file or directory\n") ? 0 : 31;
}

#endif

int main(void)
{
#ifdef EMBCC_TEST_HAS_FS
    tree();
    int r;
    if ((r = globs()) || (r = walks()) || (r = logs()))
        return r;
#else
    /* no filesystem: what needs none -- the pattern test, and a pattern
     * that matches nothing kept by GLOB_NOCHECK */
    glob_t g;
    if (!glob_pattern_p("a[bc]", 1) || glob_pattern_p("a\\*", 1) ||
        glob("/x/*.c", GLOB_NOCHECK, NULL, &g) || g.gl_pathc != 1 ||
        strcmp(g.gl_pathv[0], "/x/*.c"))
        return 1;
    globfree(&g);
#endif
    return 42;
}
