/* tmpnam's names, tmpfile's stream, and fopen's exclusive "wx". A target
 * with no filesystem has no temporary file: tmpfile is NULL there, and the
 * rest is only asked where it opens. Asked of lib/libc, found by a weak
 * reference to a function each of its backends defines, and of a Darwin
 * host's libc (its linker refuses a weak reference nothing defines) -- not
 * of the newlib the x86-64 QEMU harness links, whose tmpnam finds no files
 * to check its names against there. tmpfile and remove are weak references
 * too, so that newlib's, which need an unlink the harness has not got, are
 * not linked; lib/libc's are in the stdio that fopen brings. */
// expect-exit: 42
#include <stdio.h>
#include <string.h>

#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
FILE *tmpfile(void) __attribute__((weak));
int remove(const char *path) __attribute__((weak));
#endif

/* Where the test knows there is a filesystem -- tests/golden/linux-libc-
 * exec.sh boots Linux with an empty /tmp and nothing else in it -- tmpfile
 * must open, and its name must be gone while the stream is still open:
 * /tmp is counted through lib/libc's backend, as this library has no
 * <dirent.h>. */
#ifdef EMBCC_TEST_HAS_FS
void *__os_opendir(const char *path);
int __os_readdir(void *dir, char *name, size_t n, int *type);
void __os_closedir(void *dir);
static int in_tmp(void)
{
    char name[256];
    int type, n = 0;
    void *d = __os_opendir("/tmp");
    if (!d)
        return -1;
    while (__os_readdir(d, name, sizeof name, &type) == 1)
        n++;
    __os_closedir(d);
    return n;
}
#endif

int main(void)
{
#if !defined(__APPLE__)
    if (!&__os_write)
        return 42;
#endif
    char a[L_tmpnam], b[L_tmpnam];
    if (tmpnam(a) != a || tmpnam(b) != b)
        return 1;
    if (strlen(a) == 0 || strlen(a) >= L_tmpnam || strcmp(a, b) == 0)
        return 2;
    char *in = tmpnam(NULL);
    if (!in || strcmp(in, a) == 0 || strcmp(in, b) == 0)
        return 3;

    FILE *f = tmpfile();
#ifdef EMBCC_TEST_HAS_FS
    if (!f || in_tmp() != 0)
        return 12;
#endif
    if (!f)
        return 42;
    static const char msg[] = "temporary, and read back";
    if (fwrite(msg, 1, sizeof msg, f) != sizeof msg)
        return 5;
    rewind(f);
    char got[sizeof msg];
    if (fread(got, 1, sizeof got, f) != sizeof got ||
        memcmp(got, msg, sizeof msg) != 0)
        return 6;
    if (fclose(f) != 0)
        return 7;

    FILE *x = fopen(a, "wx");
    if (!x || fputs("kept", x) < 0 || fclose(x) != 0)
        return 8;
    if (fopen(a, "wbx") || fopen(a, "w+x"))
        return 9;                       /* it exists now */
    FILE *r = fopen(a, "r");
    char kept[8] = "";
    if (!r || !fgets(kept, sizeof kept, r) || strcmp(kept, "kept") != 0)
        return 10;                      /* and was not truncated */
    fclose(r);
#ifdef EMBCC_TEST_HAS_FS
    if (in_tmp() != 1)
        return 13;
#endif
    if (remove(a) != 0)
        return 11;
#ifdef EMBCC_TEST_HAS_FS
    if (in_tmp() != 0)
        return 14;
#endif
    return 42;
}
