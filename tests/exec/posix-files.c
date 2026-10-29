/* POSIX's functions that need a filesystem: mkstemp and its family,
 * mkdtemp and mktemp, realpath through ".", ".." and a symlink, fdopen
 * and fileno, and freopen -- which must reopen the SAME stream, so that
 * freopen("x", "w", stdout) leaves stdout itself writing to x (it used
 * to hand back another FILE and close the one it was given).
 *
 * tests/golden/linux-libc-exec.sh runs it on Linux with an empty /tmp,
 * built with EMBCC_TEST_HAS_FS. Without that, there is no filesystem,
 * and what can be asked is that realpath and mkdtemp fail and say so. */
// expect-exit: 42
// needs-libc: mkstemps and the rest, against lib/libc's backends
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef EMBCC_TEST_HAS_FS
/* the six letters that replaced "XXXXXX" -- which may themselves include
 * an X, so "no X left" is not the test */
static int filled(const char *name, size_t suffix)
{
    return memcmp(name + strlen(name) - suffix - 6, "XXXXXX", 6) != 0;
}

int __os_symlink(const char *target, const char *linkpath);
int __os_mkdir(const char *path, unsigned mode);

static int temporaries(void)
{
    char t[] = "/tmp/fileXXXXXX";
    int fd = mkstemp(t);
    if (fd < 0 || strncmp(t, "/tmp/file", 9) != 0 || !filled(t, 0))
        return 1;
    FILE *f = fdopen(fd, "w+");
    if (!f || fileno(f) != fd)
        return 2;
    fprintf(f, "through fdopen\n");
    rewind(f);
    char line[64];
    if (!fgets(line, sizeof line, f) || strcmp(line, "through fdopen\n"))
        return 3;
    fclose(f);
    /* the same template, made again: a different name */
    char u[] = "/tmp/fileXXXXXX";
    if (mkstemp(u) < 0 || strcmp(t, u) == 0)
        return 4;

    char s[] = "/tmp/sXXXXXX.txt";
    if (mkstemps(s, 4) < 0 || strcmp(s + strlen(s) - 4, ".txt") != 0 ||
        !filled(s, 4))
        return 5;
    char bad[] = "/tmp/noxs";
    errno = 0;
    if (mkstemp(bad) != -1 || errno != EINVAL)
        return 6;
    char d[] = "/tmp/dirXXXXXX";
    if (!mkdtemp(d) || mkdtemp((char[]){ "/tmp/no/such/XXXXXX" }))
        return 7;
    char m[] = "/tmp/mtXXXXXX";
    if (mktemp(m) != m || m[0] != '/' || !filled(m, 0))
        return 8;
    return 0;
}

static int resolved(void)
{
    char out[PATH_MAX], in[PATH_MAX];
    if (__os_mkdir("/tmp/r", 0755) || __os_mkdir("/tmp/r/inner", 0755) ||
        __os_symlink("inner", "/tmp/r/link") ||
        __os_symlink("/tmp/r/inner/..", "/tmp/r/up"))
        return 10;
    if (!realpath("/tmp/r/./inner/../inner/", out) ||
        strcmp(out, "/tmp/r/inner") != 0)
        return 11;
    /* ".." after a link leaves what the link points at */
    if (!realpath("/tmp/r/link/..", out) || strcmp(out, "/tmp/r") != 0)
        return 12;
    if (!realpath("/tmp/r/up/link", out) || strcmp(out, "/tmp/r/inner") != 0)
        return 13;
    snprintf(in, sizeof in, "//tmp///r");
    char *p = realpath(in, NULL);
    if (!p || strcmp(p, "/tmp/r") != 0)
        return 14;
    free(p);
    errno = 0;
    if (realpath("/tmp/none/x", out) || errno != ENOENT)
        return 15;
    FILE *f = fopen("/tmp/r/file", "w");
    fclose(f);
    errno = 0;
    if (realpath("/tmp/r/file/x", out) || errno != ENOTDIR)
        return 16;
    if (__os_symlink("loop", "/tmp/r/loop") || realpath("/tmp/r/loop", out) ||
        errno != ELOOP)
        return 17;
    return 0;
}

static int reopened(void)
{
    FILE *a = fopen("/tmp/a.txt", "w");
    if (!a)
        return 20;
    FILE *b = freopen("/tmp/b.txt", "w", a);
    if (b != a)
        return 21;
    fputs("in b", b);
    fclose(b);
    char line[16];
    FILE *r = fopen("/tmp/b.txt", "r");
    if (!r || !fgets(line, sizeof line, r) || strcmp(line, "in b") != 0)
        return 22;
    fclose(r);
    /* stdout itself, which is no slot in the table of streams: the case
     * where a new FILE could never have been the old one */
    if (freopen("/tmp/c.txt", "w", stdout) != stdout)
        return 23;
    printf("to c %d", 3);
    fflush(stdout);
    r = fopen("/tmp/c.txt", "r");
    if (!r || !fgets(line, sizeof line, r) || strcmp(line, "to c 3") != 0)
        return 24;
    fclose(r);
    return 0;
}
#endif

int main(void)
{
#ifdef EMBCC_TEST_HAS_FS
    int r;
    if ((r = temporaries()) || (r = resolved()) || (r = reopened()))
        return r;
#else
    char out[PATH_MAX];
    errno = 0;
    if (realpath("x", out) || errno == 0)
        return 30;
    errno = 0;
    if (mkdtemp((char[]){ "/tmp/dirXXXXXX" }) || errno == 0)
        return 31;
#endif
    return 42;
}
