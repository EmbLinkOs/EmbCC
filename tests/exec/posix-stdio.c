/* POSIX's streams that are not files, and the stdio functions beyond C's:
 * fmemopen (its NUL rules are glibc's since 2.22), open_memstream,
 * fopencookie, getline and getdelim, asprintf and dprintf, flockfile --
 * which nests, so a thread holding a stream may still printf to it --
 * and more streams open at once than FOPEN_MAX.
 *
 * The expected values are glibc 2.36's but for one: open_memstream's
 * length after a write in the middle, where glibc answers what POSIX says
 * it must not (see `grows`). */
// expect-exit: 42
// needs-libc: POSIX stdio the harnesses' newlib has not got all of
#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same(const char *got, const char *want, size_t n)
{
    return memcmp(got, want, n) == 0;
}

static int fixed(void)
{
    char buf[16];
    /* "w": a NUL after the data, and never past the size */
    memset(buf, 'x', sizeof buf);
    FILE *f = fmemopen(buf, 8, "w");
    if (!f)
        return 1;
    fputs("abc", f);
    fflush(f);
    if (!same(buf, "abc\0xxxxxx", 10))
        return 2;
    fputs("defghijk", f);
    fflush(f);
    if (!same(buf, "abcdefg\0xx", 10) || ftell(f) != 8)
        return 3;
    fclose(f);

    /* "a": from the first NUL */
    memset(buf, 'x', sizeof buf);
    strcpy(buf, "hi");
    f = fmemopen(buf, 8, "a");
    fputs("yo", f);
    fclose(f);
    if (!same(buf, "hiyo\0xxxxx", 10))
        return 4;

    /* "r": the whole buffer, seekable within it and not beyond */
    strcpy(buf, "0123456789");
    f = fmemopen(buf, 10, "r");
    char line[32];
    fseek(f, 4, SEEK_SET);
    if (fread(line, 1, 3, f) != 3 || !same(line, "456", 3) || ftell(f) != 7)
        return 5;
    errno = 0;
    if (fseek(f, 11, SEEK_SET) != -1 || errno != EINVAL)
        return 6;
    fseek(f, -2, SEEK_END);
    if (fgetc(f) != '8' || fgetc(f) != '9' || fgetc(f) != EOF || !feof(f))
        return 7;
    fclose(f);

    /* "w+": written, then read back through the same stream */
    memset(buf, 'x', sizeof buf);
    f = fmemopen(buf, 12, "w+");
    fprintf(f, "n=%d", 42);
    rewind(f);
    int v = 0;
    if (fscanf(f, "n=%d", &v) != 1 || v != 42)
        return 8;
    fclose(f);
    if (!same(buf, "n=42\0xxxxxxx", 12))
        return 9;

    /* no buffer: one of its own, freed at fclose */
    f = fmemopen(NULL, 16, "w+");
    fputs("scratch", f);
    rewind(f);
    if (!fgets(line, sizeof line, f) || strcmp(line, "scratch") != 0)
        return 10;
    fclose(f);
    return 0;
}

static int grows(void)
{
    char *p = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&p, &n);
    if (!f)
        return 20;
    fprintf(f, "hello %s", "world");
    fflush(f);
    if (strcmp(p, "hello world") != 0 || n != 11)
        return 21;
    fseek(f, 2, SEEK_SET);
    fflush(f);
    if (n != 2)                            /* the position, not the length */
        return 22;
    fputs("LL", f);
    /* POSIX: a write moves the length only when it passes it, so the end
     * is still 11. (glibc 2.36 answers 4.) */
    fseek(f, 0, SEEK_END);
    fflush(f);
    if (strcmp(p, "heLLo world") != 0 || n != 11)
        return 23;
    fseek(f, 15, SEEK_SET);                /* past the end: a gap of zeros */
    fputc('!', f);
    fclose(f);
    if (n != 16 || !same(p, "heLLo world\0\0\0\0!", 17))
        return 24;
    free(p);
    return 0;
}

static int lines(void)
{
    /* "one\n", "t\0o\n" (a NUL inside a line), 300 letters, "last" */
    static char text[400];
    size_t tn = 0;
    memcpy(text + tn, "one\nt\0o\n", 8);
    tn += 8;
    for (int i = 0; i < 300; i++)
        text[tn++] = (char)('a' + i % 26);
    text[tn++] = '\n';
    memcpy(text + tn, "last", 4);
    tn += 4;
    FILE *f = fmemopen(text, tn, "r");
    char *lp = NULL;
    size_t cap = 0;
    static const ssize_t want[] = { 4, 4, 301, 4 };
    for (int i = 0; i < 4; i++)
        if (getline(&lp, &cap, f) != want[i])
            return 30 + i;
    if (memcmp(lp, "last", 5) != 0 || cap < 302)
        return 34;
    if (getline(&lp, &cap, f) != -1 || !feof(f))
        return 35;
    rewind(f);
    if (getdelim(&lp, &cap, 'e', f) != 3 || strcmp(lp, "one") != 0)
        return 36;
    fclose(f);
    free(lp);

    /* a NULL line pointer: getline allocates, even at end of file */
    f = fmemopen((char[]){ "x" }, 1, "r");
    lp = NULL;
    cap = 0;
    if (getline(&lp, &cap, f) != 1 || getline(&lp, &cap, f) != -1 || !lp)
        return 37;
    free(lp);
    fclose(f);
    return 0;
}

static int formatted(void)
{
    char *s = NULL;
    if (asprintf(&s, "%05d|%-4s|%x", 42, "ab", 255) != 13 ||
        strcmp(s, "00042|ab  |ff") != 0)
        return 40;
    free(s);
    fflush(stdout);
    if (dprintf(1, "dprintf %d\n", 7) != 10)
        return 41;
    return 0;
}

struct ck { char data[64]; size_t len, pos; int closed; };

static ssize_t ck_read(void *c, char *b, size_t n)
{
    struct ck *k = c;
    if (n > k->len - k->pos)
        n = k->len - k->pos;
    memcpy(b, k->data + k->pos, n);
    k->pos += n;
    return (ssize_t)n;
}

static ssize_t ck_write(void *c, const char *b, size_t n)
{
    struct ck *k = c;
    for (size_t i = 0; i < n; i++)
        k->data[k->len++] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]);
    return (ssize_t)n;
}

static int ck_close(void *c)
{
    ((struct ck *)c)->closed = 1;
    return 0;
}

static int cookies(void)
{
    struct ck k = { "cookie data\nsecond\n", 19, 0, 0 };
    cookie_io_functions_t io = { ck_read, ck_write, NULL, ck_close };
    FILE *f = fopencookie(&k, "r", io);
    char line[32];
    if (!f || !fgets(line, sizeof line, f) || strcmp(line, "cookie data\n"))
        return 50;
    errno = 0;
    if (fileno(f) != -1 || errno != EBADF || fseek(f, 0, SEEK_SET) != -1)
        return 51;
    fclose(f);
    if (!k.closed)
        return 52;
    struct ck w = { "", 0, 0, 0 };
    f = fopencookie(&w, "w", io);
    fprintf(f, "upper %d", 9);
    fclose(f);
    if (w.len != 7 || !same(w.data, "UPPER 9", 7))
        return 53;
    return 0;
}

static int locking(void)
{
    flockfile(stdout);
    flockfile(stdout);
    if (ftrylockfile(stdout) != 0)
        return 60;
    printf("nested ok\n");                 /* takes the lock a fourth time */
    funlockfile(stdout);
    funlockfile(stdout);
    funlockfile(stdout);
    putc_unlocked('\n', stdout);
    return 0;
}

static int many(void)
{
    /* more than FOPEN_MAX: the table is a minimum, not a limit */
    static char mb[40][4];
    FILE *s[40];
    for (int i = 0; i < 40; i++) {
        s[i] = fmemopen(mb[i], 4, "w");
        if (!s[i])
            return 70;
        fprintf(s[i], "%d", i);
    }
    for (int i = 0; i < 40; i++)
        fclose(s[i]);
    if (strcmp(mb[0], "0") != 0 || strcmp(mb[39], "39") != 0)
        return 71;
    return 0;
}

int main(void)
{
    int r;
    if ((r = fixed()) || (r = grows()) || (r = lines()) ||
        (r = formatted()) || (r = cookies()) || (r = locking()) ||
        (r = many()))
        return r;
    return 42;
}
