/* FILE objects, buffering, and the byte-level calls, C11 §7.21.
 *
 * Buffering rules follow C: stderr unbuffered, a terminal line-buffered,
 * everything else fully buffered. They matter more than they look -- a
 * program that crashes with its output still in a buffer looks like it
 * stopped somewhere it did not.
 */
#include "file.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "../../os/backend.h"

static FILE g_files[FOPEN_MAX];
static unsigned char g_inbuf[BUFSIZ], g_outbuf[BUFSIZ];

static FILE g_stdin  = { 0, F_READ  | F_USED, g_inbuf,  BUFSIZ, 0, 0,
                         _IOFBF, -1, 0 };
static FILE g_stdout = { 1, F_WRITE | F_USED, g_outbuf, BUFSIZ, 0, 0,
                         _IOFBF, -1, 0 };
static FILE g_stderr = { 2, F_WRITE | F_USED, NULL, 0, 0, 0, _IONBF, -1, 0 };

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

/* Decided once, on first use: isatty is a syscall and stdout's buffering
 * must not cost one per write. */
static void settle_mode(FILE *f)
{
    if (f == &g_stdout && f->mode == _IOFBF && f->pos == 0 && f->len == 0) {
        static int done;
        if (!done) { done = 1; if (__os_isatty(1)) f->mode = _IOLBF; }
    }
}

int __stdio_flush(FILE *f)
{
    if (!f || !(f->flags & F_WRITING) || f->pos == 0)
        return 0;
    size_t off = 0;
    while (off < f->pos) {
        long w = __os_write(f->fd, f->buf + off, f->pos - off);
        if (w <= 0) { f->flags |= F_ERR; f->pos = 0; return EOF; }
        off += (size_t)w;
    }
    f->pos = 0;
    f->flags &= ~F_WRITING;
    return 0;
}

void __stdio_flush_all(void)
{
    __stdio_flush(&g_stdout);
    __stdio_flush(&g_stderr);
    for (int i = 0; i < FOPEN_MAX; i++)
        if (g_files[i].flags & F_USED)
            __stdio_flush(&g_files[i]);
}

int fflush(FILE *f)
{
    if (!f) { __stdio_flush_all(); return 0; }
    return __stdio_flush(f);
}

int fputc(int c, FILE *f)
{
    if (!f || !(f->flags & F_WRITE)) { if (f) f->flags |= F_ERR; return EOF; }
    settle_mode(f);
    unsigned char ch = (unsigned char)c;
    if (f->mode == _IONBF || !f->buf) {
        if (__os_write(f->fd, &ch, 1) != 1) { f->flags |= F_ERR; return EOF; }
        return (int)ch;
    }
    f->flags |= F_WRITING;
    f->buf[f->pos++] = ch;
    if (f->pos == f->bufsz || (f->mode == _IOLBF && ch == '\n'))
        if (__stdio_flush(f) == EOF)
            return EOF;
    return (int)ch;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *restrict s, FILE *restrict f)
{
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void *restrict p, size_t size, size_t n,
              FILE *restrict f)
{
    if (!f || !(f->flags & F_WRITE) || size == 0 || n == 0)
        return 0;
    const unsigned char *q = p;
    size_t total = size * n;
    /* A large write goes straight out rather than through the buffer: the
     * copy would cost more than the syscall it saves. */
    if (f->buf && f->mode != _IONBF && total < f->bufsz) {
        for (size_t i = 0; i < total; i++)
            if (fputc(q[i], f) == EOF)
                return i / size;
        return n;
    }
    if (__stdio_flush(f) == EOF)
        return 0;
    size_t off = 0;
    while (off < total) {
        long w = __os_write(f->fd, q + off, total - off);
        if (w <= 0) { f->flags |= F_ERR; return off / size; }
        off += (size_t)w;
    }
    return n;
}

static int refill(FILE *f)
{
    if (!f->buf || !(f->flags & F_READ)) return EOF;
    long r = __os_read(f->fd, f->buf, f->bufsz);
    if (r <= 0) { f->flags |= (r == 0) ? F_EOF : F_ERR; return EOF; }
    f->pos = 0;
    f->len = (size_t)r;
    return 0;
}

int fgetc(FILE *f)
{
    if (!f || !(f->flags & F_READ)) return EOF;
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->pos >= f->len && refill(f) == EOF) return EOF;
    return f->buf[f->pos++];
}

int getc(FILE *f)  { return fgetc(f); }
int getchar(void)  { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (!f || c == EOF || f->ungot >= 0) return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

char *fgets(char *restrict s, int n, FILE *restrict f)
{
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;     /* the newline is kept, unlike gets */
    }
    s[i] = 0;
    return s;
}

size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f)
{
    if (!f || size == 0 || n == 0) return 0;
    unsigned char *q = p;
    size_t total = size * n, got = 0;
    while (got < total) {
        int c = fgetc(f);
        if (c == EOF) break;
        q[got++] = (unsigned char)c;
    }
    return got / size;
}

void clearerr(FILE *f) { if (f) f->flags &= ~(F_EOF | F_ERR); }
int  feof(FILE *f)     { return f && (f->flags & F_EOF) ? 1 : 0; }
int  ferror(FILE *f)   { return f && (f->flags & F_ERR) ? 1 : 0; }

void perror(const char *s)
{
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* ---- files by name ---- */

/* The open flags belong to the OS contract (os/backend.h), not to stdio:
 * a backend has to be able to see the same numbers this file asks with. */
#define O_RDONLY __OS_O_RDONLY
#define O_WRONLY __OS_O_WRONLY
#define O_RDWR   __OS_O_RDWR
#define O_CREAT  __OS_O_CREAT
#define O_TRUNC  __OS_O_TRUNC
#define O_APPEND __OS_O_APPEND

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    int fl = 0, acc = 0;
    switch (mode[0]) {
    case 'r': acc = F_READ;  fl = O_RDONLY; break;
    case 'w': acc = F_WRITE; fl = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': acc = F_WRITE | F_APPEND; fl = O_WRONLY | O_CREAT | O_APPEND;
              break;
    default:  errno = EINVAL; return NULL;
    }
    if (strchr(mode, '+')) { acc = F_READ | F_WRITE;
                             fl = (fl & ~(O_RDONLY | O_WRONLY)) | O_RDWR; }

    FILE *f = NULL;
    for (int i = 0; i < FOPEN_MAX; i++)
        if (!(g_files[i].flags & F_USED)) { f = &g_files[i]; break; }
    if (!f) { errno = EMFILE; return NULL; }

    int fd = __os_open(path, fl, 0666);
    if (fd < 0) return NULL;       /* the backend set errno */

    memset(f, 0, sizeof *f);
    f->fd = fd;
    f->flags = acc | F_USED;
    f->buf = malloc(BUFSIZ);
    f->bufsz = f->buf ? BUFSIZ : 0;
    f->own_buf = f->buf != NULL;
    f->mode = f->buf ? _IOFBF : _IONBF;
    f->ungot = -1;
    return f;
}

int fclose(FILE *f)
{
    if (!f || !(f->flags & F_USED)) return EOF;
    int r = __stdio_flush(f);
    if (__os_close(f->fd) < 0) r = EOF;
    if (f->own_buf) free(f->buf);
    f->flags = 0;
    return r;
}

FILE *freopen(const char *restrict path, const char *restrict mode,
              FILE *restrict f)
{
    if (f) { __stdio_flush(f); __os_close(f->fd); f->flags = 0; }
    return fopen(path, mode);
}

int setvbuf(FILE *restrict f, char *restrict buf, int mode, size_t size)
{
    if (!f) return -1;
    __stdio_flush(f);
    if (f->own_buf) { free(f->buf); f->own_buf = 0; }
    if (mode == _IONBF) { f->buf = NULL; f->bufsz = 0; }
    else if (buf)       { f->buf = (unsigned char *)buf; f->bufsz = size; }
    else { f->buf = malloc(size ? size : BUFSIZ);
           f->bufsz = f->buf ? (size ? size : BUFSIZ) : 0;
           f->own_buf = f->buf != NULL; }
    f->mode = f->buf ? mode : _IONBF;
    f->pos = f->len = 0;
    return 0;
}

void setbuf(FILE *restrict f, char *restrict buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int fseek(FILE *f, long off, int whence)
{
    if (!f) return -1;
    __stdio_flush(f);
    f->pos = f->len = 0;
    f->ungot = -1;
    f->flags &= ~F_EOF;
    return __os_lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
    if (!f) return -1;
    long at = __os_lseek(f->fd, 0, SEEK_CUR);
    if (at < 0) return -1;
    /* What the OS thinks minus what is still sitting in our buffer. */
    if (f->flags & F_WRITING) return at + (long)f->pos;
    return at - (long)(f->len - f->pos) - (f->ungot >= 0 ? 1 : 0);
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); clearerr(f); }

int fgetpos(FILE *restrict f, fpos_t *restrict pos)
{
    long at = ftell(f);
    if (at < 0) return -1;
    *pos = at;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, *pos, SEEK_SET); }

/* remove and rename — C11 §7.21.4. Both are the backend's, because what
 * a name refers to and whether it can be unlinked is the OS's business
 * (os/backend.h). */
int remove(const char *path) { return __os_remove(path); }

int rename(const char *from, const char *to)
{
    return __os_rename(from, to);
}
