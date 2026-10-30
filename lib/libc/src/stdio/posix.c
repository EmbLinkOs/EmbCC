/* <stdio.h>'s POSIX functions beyond C's, and the GNU ones Unix programs
 * reach for: streams on a descriptor (fdopen, fileno) or on a caller's
 * functions (fopencookie), line reading of any length (getline,
 * getdelim), printf to a descriptor or to new memory (dprintf, asprintf),
 * the 64-bit offsets' names, and the unlocked forms.
 *
 * The memory streams, fmemopen and open_memstream, are memstream.c. */
#include "file.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../../os/backend.h"

/* A stream on a descriptor already open. The mode must not ask for more
 * than the descriptor allows, but nothing here can ask the OS what that
 * is, so a mismatch surfaces at the first read or write, as EBADF --
 * where glibc would have refused at fdopen. "w" does NOT truncate: the
 * file is already open and fdopen does not reopen it. */
__attribute__((weak)) FILE *fdopen(int fd, const char *mode)
{
    int fl, acc;
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    if (__stdio_mode(mode, &fl, &acc) < 0)
        return NULL;
    FILE *f = __stdio_claim();
    if (!f)
        return NULL;
    f->fd = fd;
    __stdio_setup(f, acc);
    return f;
}

__attribute__((weak)) int fileno(FILE *f)
{
    if (!f || !(f->flags & F_USED) || (f->flags & F_COOKIE)) {
        errno = EBADF;
        return -1;
    }
    return f->fd;
}

/* A stream whose reads, writes, seeks and close are the caller's. */
__attribute__((weak)) FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io)
{
    int fl, acc;
    if (__stdio_mode(mode, &fl, &acc) < 0)
        return NULL;
    FILE *f = __stdio_claim();
    if (!f)
        return NULL;
    f->flags |= F_COOKIE;
    f->cookie = cookie;
    f->io.read = io.read;
    f->io.write = io.write;
    f->io.seek = io.seek;
    f->io.close = io.close;
    __stdio_setup(f, acc);
    return f;
}

/* ---- lines of any length --------------------------------------------- */

/* Up to and including delim, NUL-terminated, in *line -- which is grown
 * with realloc as the line needs, and allocated when it is NULL. The
 * length, which may count NULs in the line; -1 at end of file with
 * nothing read, or on error. The buffer exists afterwards even then, as
 * glibc leaves it, so a caller's free(line) is always right. */
__attribute__((weak)) ssize_t getdelim(char **restrict line, size_t *restrict n, int delim,
                 FILE *restrict f)
{
    if (!line || !n || !f) {
        errno = EINVAL;
        return -1;
    }
    if (!*line || *n == 0) {
        char *p = realloc(*line, 128);
        if (!p) {
            errno = ENOMEM;
            return -1;
        }
        *line = p;
        *n = 128;
    }
    size_t len = 0;
    __flockfile(f);
    for (;;) {
        int c = __getc_unlocked(f);
        if (c == EOF)
            break;
        if (len + 2 > *n) {
            size_t want = *n * 2;
            if (want < *n || want > (size_t)PTRDIFF_MAX) {
                __funlockfile(f);
                errno = EOVERFLOW;
                return -1;
            }
            char *p = realloc(*line, want);
            if (!p) {
                f->flags |= F_ERR;
                __funlockfile(f);
                errno = ENOMEM;
                return -1;
            }
            *line = p;
            *n = want;
        }
        (*line)[len++] = (char)c;
        if (c == (unsigned char)delim)
            break;
    }
    __funlockfile(f);
    (*line)[len] = 0;
    return len ? (ssize_t)len : -1;
}

__attribute__((weak)) ssize_t getline(char **restrict line, size_t *restrict n, FILE *restrict f)
{
    return getdelim(line, n, '\n', f);
}

/* ---- printf somewhere else ------------------------------------------- */

/* To a descriptor, through a small buffer: one write per 256 bytes,
 * not one per conversion, and no FILE. */
struct fd_sink {
    int fd, err;
    size_t n;
    char buf[256];
};

static void fd_flush(struct fd_sink *s)
{
    for (size_t off = 0; off < s->n && !s->err; ) {
        long w = __os_write(s->fd, s->buf + off, s->n - off);
        if (w <= 0)
            s->err = 1;
        else
            off += (size_t)w;
    }
    s->n = 0;
}

static void sink_fd(void *ctx, const char *p, size_t n)
{
    struct fd_sink *s = ctx;
    while (n) {
        if (s->n == sizeof s->buf)
            fd_flush(s);
        size_t k = sizeof s->buf - s->n;
        if (k > n)
            k = n;
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        n -= k;
    }
}

__attribute__((weak)) int vdprintf(int fd, const char *restrict fmt, va_list ap)
{
    struct fd_sink s = { fd, 0, 0, { 0 } };
    int r = __vformat(sink_fd, &s, fmt, ap);
    fd_flush(&s);
    return s.err ? -1 : r;
}

__attribute__((weak)) int dprintf(int fd, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

/* To memory it allocates: vsnprintf twice, once to learn the length. On
 * failure *s is NULL, which POSIX.1-2024 made the rule. */
__attribute__((weak)) int vasprintf(char **restrict s, const char *restrict fmt, va_list ap)
{
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    char *p = n < 0 ? NULL : malloc((size_t)n + 1);
    if (p)
        vsnprintf(p, (size_t)n + 1, fmt, again);
    va_end(again);
    *s = p;
    return p ? n : -1;
}

__attribute__((weak)) int asprintf(char **restrict s, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- offsets, buffering, the terminal --------------------------------- */

__attribute__((weak)) int   fseeko(FILE *f, off_t off, int whence) { return fseek(f, off, whence); }
__attribute__((weak)) off_t ftello(FILE *f)                        { return ftell(f); }

__attribute__((weak)) void setbuffer(FILE *f, char *buf, size_t n)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, n);
}

__attribute__((weak)) void setlinebuf(FILE *f) { setvbuf(f, NULL, _IOLBF, 0); }

/* The name of the controlling terminal, which on every Unix is this. */
__attribute__((weak)) char *ctermid(char *s)
{
    static const char tty[] = "/dev/tty";
    return s ? strcpy(s, tty) : (char *)tty;
}

/* ---- without the lock ------------------------------------------------
 *
 * For a caller that has taken it with flockfile: one lock around a loop
 * of getc_unlocked is the point of having both. */
__attribute__((weak)) int getc_unlocked(FILE *f)      { return __getc_unlocked(f); }
__attribute__((weak)) int getchar_unlocked(void)      { return __getc_unlocked(stdin); }
__attribute__((weak)) int putc_unlocked(int c, FILE *f) { return __putc_unlocked(c, f); }
__attribute__((weak)) int putchar_unlocked(int c)     { return __putc_unlocked(c, stdout); }
__attribute__((weak)) int fgetc_unlocked(FILE *f)     { return __getc_unlocked(f); }
__attribute__((weak)) int fputc_unlocked(int c, FILE *f) { return __putc_unlocked(c, f); }
__attribute__((weak)) int fflush_unlocked(FILE *f)    { return f ? __stdio_flush(f) : fflush(NULL); }
__attribute__((weak)) int feof_unlocked(FILE *f)      { return (f->flags & F_EOF) != 0; }
__attribute__((weak)) int ferror_unlocked(FILE *f)    { return (f->flags & F_ERR) != 0; }
__attribute__((weak)) void clearerr_unlocked(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
__attribute__((weak)) int fileno_unlocked(FILE *f)    { return fileno(f); }

__attribute__((weak)) size_t fwrite_unlocked(const void *restrict p, size_t size, size_t n,
                       FILE *restrict f)
{
    return __fwrite_unlocked(p, size, n, f);
}

__attribute__((weak)) size_t fread_unlocked(void *restrict p, size_t size, size_t n,
                      FILE *restrict f)
{
    if (size == 0 || n == 0)
        return 0;
    unsigned char *q = p;
    size_t total = size * n, got = 0;
    while (got < total) {
        int c = __getc_unlocked(f);
        if (c == EOF)
            break;
        q[got++] = (unsigned char)c;
    }
    return got / size;
}

__attribute__((weak)) int fputs_unlocked(const char *restrict s, FILE *restrict f)
{
    size_t n = strlen(s);
    return __fwrite_unlocked(s, 1, n, f) == n ? 0 : EOF;
}

/* ---- XSI's and GNU's leftovers ------------------------------------------------ */

/* an int's bytes, as they are in memory */
__attribute__((weak)) int getw(FILE *f)
{
    int w;
    return fread(&w, sizeof w, 1, f) == 1 ? w : EOF;
}

__attribute__((weak)) int putw(int w, FILE *f)
{
    return fwrite(&w, sizeof w, 1, f) == 1 ? 0 : EOF;
}

/* A name for a new file: in $TMPDIR, else dir, else P_tmpdir, else
 * /tmp -- the first that is a directory one can write to -- beginning
 * with up to five bytes of prefix. The caller frees it. */
__attribute__((weak)) char *tempnam(const char *dir, const char *prefix)
{
    const char *cand[4] = { getenv("TMPDIR"), dir, P_tmpdir, "/tmp" };
    const char *d = 0;
    for (int i = 0; i < 4 && !d; i++) {
        struct stat st;
        if (cand[i] && *cand[i] && stat(cand[i], &st) == 0 &&
            S_ISDIR(st.st_mode) && access(cand[i], W_OK | X_OK) == 0)
            d = cand[i];
    }
    if (!d) {
        errno = ENOENT;
        return NULL;
    }
    size_t pl = prefix ? strnlen(prefix, 5) : 0;
    size_t dl = strlen(d);
    while (dl > 1 && d[dl - 1] == '/')
        dl--;
    char *name = malloc(dl + 1 + pl + 6 + 1);
    if (!name)
        return NULL;
    static const char abc[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int tries = 0; tries < 100; tries++) {
        unsigned long long r;
        if (getentropy(&r, sizeof r))
            r = (unsigned long long)(uintptr_t)name * 2654435761u + (unsigned)tries;
        memcpy(name, d, dl);
        name[dl] = '/';
        if (pl)
            memcpy(name + dl + 1, prefix, pl);
        for (int i = 0; i < 6; i++, r /= 62)
            name[dl + 1 + pl + i] = abc[r % 62];
        name[dl + 1 + pl + 6] = 0;
        struct stat st;
        if (lstat(name, &st) != 0 && errno == ENOENT)
            return name;
    }
    free(name);
    errno = EEXIST;
    return NULL;
}

/* the unlocked forms: each call here takes its stream's lock anyway, and
 * a program that already holds it (flockfile) may take it again */
__attribute__((weak)) char *fgets_unlocked(char *restrict s, int n, FILE *restrict f)
{
    return fgets(s, n, f);
}
