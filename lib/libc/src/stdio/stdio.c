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
#include "../internal/tmpname.h"

static FILE g_files[FOPEN_MAX];

/* More than FOPEN_MAX streams at once: FOPEN_MAX is C's promise of how
 * many a program may count on, not a limit a hosted program should meet
 * -- a server has a stream per connection. Beyond the table they come in
 * chunks from the heap, and a chunk is never freed: a FILE that has
 * been handed out stays valid memory for the life of the program, so
 * the flush at exit, walking every stream, can never land in a closed
 * one's freed storage. A closed FILE in a chunk is reused like a slot. */
#define CHUNK 16
struct chunk {
    struct chunk *next;
    FILE f[CHUNK];
};
static struct chunk *g_chunks;
static unsigned char g_inbuf[BUFSIZ], g_outbuf[BUFSIZ];

static FILE g_stdin  = { 0, F_READ  | F_USED, g_inbuf,  BUFSIZ, 0, 0,
                         _IOFBF, -1, 0, LOCK_INIT, 0, 0, { 0 }, 0 };
static FILE g_stdout = { 1, F_WRITE | F_USED, g_outbuf, BUFSIZ, 0, 0,
                         _IOFBF, -1, 0, LOCK_INIT, 0, 0, { 0 }, 0 };
static FILE g_stderr = { 2, F_WRITE | F_USED, NULL, 0, 0, 0, _IONBF, -1, 0,
                         LOCK_INIT, 0, 0, { 0 }, 0 };

/* The g_files TABLE has its own lock, which is a different question from
 * a stream's: it guards which slots are taken, so two threads in fopen
 * cannot pick the same one. A stream's lock guards its buffer. */
static __lock_t files_lock = LOCK_INIT;

void __flockfile(FILE *f)
{
    if (!f)
        return;
    unsigned long me = __os_thread_self();
    if (__atomic_load_n(&f->depth, __ATOMIC_RELAXED) > 0 &&
        __atomic_load_n(&f->owner, __ATOMIC_RELAXED) == me) {
        f->depth++;                /* this thread's already: nest */
        return;
    }
    __lock(&f->lock);
    __atomic_store_n(&f->owner, me, __ATOMIC_RELAXED);
    __atomic_store_n(&f->depth, 1, __ATOMIC_RELAXED);
}

void __funlockfile(FILE *f)
{
    if (!f || f->depth <= 0)
        return;
    if (--f->depth == 0)
        __unlock(&f->lock);
}

void flockfile(FILE *f)   { __flockfile(f); }
void funlockfile(FILE *f) { __funlockfile(f); }

/* 0 with the lock taken, nonzero when another thread has it. */
int ftrylockfile(FILE *f)
{
    unsigned long me = __os_thread_self();
    if (__atomic_load_n(&f->depth, __ATOMIC_RELAXED) > 0 &&
        __atomic_load_n(&f->owner, __ATOMIC_RELAXED) == me) {
        f->depth++;
        return 0;
    }
    if (__trylock(&f->lock) != 0)
        return -1;
    __atomic_store_n(&f->owner, me, __ATOMIC_RELAXED);
    __atomic_store_n(&f->depth, 1, __ATOMIC_RELAXED);
    return 0;
}

/* ---- the four operations, for a descriptor or for a cookie ----
 *
 * A cookie stream with no read function reads end-of-file, and one with
 * no write function swallows what is written: glibc's rules for
 * fopencookie, which is where a caller learned to leave one out. */
static long io_read(FILE *f, void *b, size_t n)
{
    if (!(f->flags & F_COOKIE))
        return __os_read(f->fd, b, n);
    return f->io.read ? (long)f->io.read(f->cookie, b, n) : 0;
}

static long io_write(FILE *f, const void *b, size_t n)
{
    if (!(f->flags & F_COOKIE))
        return __os_write(f->fd, b, n);
    return f->io.write ? (long)f->io.write(f->cookie, b, n) : (long)n;
}

static long io_seek(FILE *f, long off, int whence)
{
    if (!(f->flags & F_COOKIE))
        return __os_lseek(f->fd, off, whence);
    long long at = off;
    if (!f->io.seek) {
        errno = ESPIPE;
        return -1;
    }
    return f->io.seek(f->cookie, &at, whence) < 0 ? -1 : (long)at;
}

static int io_close(FILE *f)
{
    if (!(f->flags & F_COOKIE))
        return __os_close(f->fd);
    return f->io.close ? f->io.close(f->cookie) : 0;
}

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
        long w = io_write(f, f->buf + off, f->pos - off);
        if (w <= 0) { f->flags |= F_ERR; f->pos = 0; return EOF; }
        off += (size_t)w;
    }
    f->pos = 0;
    f->flags &= ~F_WRITING;
    return 0;
}

static void flush_if_used(FILE *f)
{
    __lock(&files_lock);
    int used = (f->flags & F_USED) != 0;
    __unlock(&files_lock);
    if (used) {
        __flockfile(f);
        __stdio_flush(f);
        __funlockfile(f);
    }
}

void __stdio_flush_all(void)
{
    /* One stream at a time, each under its own lock. Taking them all at
     * once would be a lock-ordering problem for no benefit: flushing is
     * per stream and they do not interact. */
    __flockfile(&g_stdout); __stdio_flush(&g_stdout); __funlockfile(&g_stdout);
    __flockfile(&g_stderr); __stdio_flush(&g_stderr); __funlockfile(&g_stderr);
    for (int i = 0; i < FOPEN_MAX; i++)
        flush_if_used(&g_files[i]);
    __lock(&files_lock);
    struct chunk *c = g_chunks;
    __unlock(&files_lock);
    for (; c; c = c->next)
        for (int i = 0; i < CHUNK; i++)
            flush_if_used(&c->f[i]);
}

int fflush(FILE *f)
{
    if (!f) { __stdio_flush_all(); return 0; }
    __flockfile(f);
    int r = __stdio_flush(f);
    __funlockfile(f);
    return r;
}

int __putc_unlocked(int c, FILE *f)
{
    if (!f || !(f->flags & F_WRITE)) { if (f) f->flags |= F_ERR; return EOF; }
    __byte_orient(f);
    settle_mode(f);
    unsigned char ch = (unsigned char)c;
    if (f->mode == _IONBF || !f->buf) {
        if (io_write(f, &ch, 1) != 1) { f->flags |= F_ERR; return EOF; }
        return (int)ch;
    }
    f->flags |= F_WRITING;
    f->buf[f->pos++] = ch;
    if (f->pos == f->bufsz || (f->mode == _IOLBF && ch == '\n'))
        if (__stdio_flush(f) == EOF)
            return EOF;
    return (int)ch;
}

int fputc(int c, FILE *f)
{
    if (!f) return EOF;
    __flockfile(f);
    int r = __putc_unlocked(c, f);
    __funlockfile(f);
    return r;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *restrict s, FILE *restrict f)
{
    size_t n = strlen(s);
    if (!f) return EOF;
    __flockfile(f);
    size_t w = __fwrite_unlocked(s, 1, n, f);
    __funlockfile(f);
    return w == n ? 0 : EOF;
}

int puts(const char *s)
{
    /* One lock for the string AND the newline. Two would let another
     * thread's line land between them, which is the whole point. */
    size_t n = strlen(s);
    __flockfile(stdout);
    int r = __fwrite_unlocked(s, 1, n, stdout) == n &&
            __putc_unlocked('\n', stdout) != EOF ? 0 : EOF;
    __funlockfile(stdout);
    return r;
}

size_t __fwrite_unlocked(const void *p, size_t size, size_t n, FILE *f)
{
    if (!f || !(f->flags & F_WRITE) || size == 0 || n == 0)
        return 0;
    __byte_orient(f);
    const unsigned char *q = p;
    size_t total = size * n;
    /* A large write goes straight out rather than through the buffer: the
     * copy would cost more than the syscall it saves. */
    if (f->buf && f->mode != _IONBF && total < f->bufsz) {
        for (size_t i = 0; i < total; i++)
            if (__putc_unlocked(q[i], f) == EOF)
                return i / size;
        return n;
    }
    if (__stdio_flush(f) == EOF)
        return 0;
    size_t off = 0;
    while (off < total) {
        long w = io_write(f, q + off, total - off);
        if (w <= 0) { f->flags |= F_ERR; return off / size; }
        off += (size_t)w;
    }
    return n;
}

size_t fwrite(const void *restrict p, size_t size, size_t n,
              FILE *restrict f)
{
    if (!f) return 0;
    __flockfile(f);
    size_t r = __fwrite_unlocked(p, size, n, f);
    __funlockfile(f);
    return r;
}

static int refill(FILE *f)
{
    if (!f->buf || !(f->flags & F_READ)) return EOF;
    long r = io_read(f, f->buf, f->bufsz);
    if (r <= 0) { f->flags |= (r == 0) ? F_EOF : F_ERR; return EOF; }
    f->pos = 0;
    f->len = (size_t)r;
    return 0;
}

int __getc_unlocked(FILE *f)
{
    if (!f || !(f->flags & F_READ)) return EOF;
    __byte_orient(f);
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->pos >= f->len && refill(f) == EOF) return EOF;
    return f->buf[f->pos++];
}

int fgetc(FILE *f)
{
    if (!f) return EOF;
    __flockfile(f);
    int c = __getc_unlocked(f);
    __funlockfile(f);
    return c;
}

int getc(FILE *f)  { return fgetc(f); }
int getchar(void)  { return fgetc(stdin); }

int __ungetc_unlocked(int c, FILE *f)
{
    if (!f || c == EOF || f->ungot >= 0) return EOF;
    __byte_orient(f);
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

int ungetc(int c, FILE *f)
{
    if (!f) return EOF;
    __flockfile(f);
    int r = __ungetc_unlocked(c, f);
    __funlockfile(f);
    return r;
}

char *fgets(char *restrict s, int n, FILE *restrict f)
{
    if (n <= 0 || !f) return NULL;
    int i = 0;
    __flockfile(f);
    while (i < n - 1) {
        int c = __getc_unlocked(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;     /* the newline is kept, unlike gets */
    }
    __funlockfile(f);
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}

size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f)
{
    if (!f || size == 0 || n == 0) return 0;
    unsigned char *q = p;
    size_t total = size * n, got = 0;
    __flockfile(f);
    while (got < total) {
        int c = __getc_unlocked(f);
        if (c == EOF) break;
        q[got++] = (unsigned char)c;
    }
    __funlockfile(f);
    return got / size;
}

/* The flag accessors take the lock too. A torn read of `flags` is not
 * the risk -- it is one word -- but ferror() after a failed write must
 * see the write's effect, and the lock is what orders the two. */
void clearerr(FILE *f)
{
    if (!f) return;
    __flockfile(f); f->flags &= ~(F_EOF | F_ERR); __funlockfile(f);
}

int feof(FILE *f)
{
    if (!f) return 0;
    __flockfile(f); int r = (f->flags & F_EOF) != 0; __funlockfile(f);
    return r;
}

int ferror(FILE *f)
{
    if (!f) return 0;
    __flockfile(f); int r = (f->flags & F_ERR) != 0; __funlockfile(f);
    return r;
}

void perror(const char *s)
{
    /* Four writes, one lock: a diagnostic split across threads is worse
     * than no diagnostic, because it reads as a different message. */
    const char *e = strerror(errno);
    __flockfile(stderr);
    if (s && *s) {
        __fwrite_unlocked(s, 1, strlen(s), stderr);
        __fwrite_unlocked(": ", 1, 2, stderr);
    }
    __fwrite_unlocked(e, 1, strlen(e), stderr);
    __putc_unlocked('\n', stderr);
    __funlockfile(stderr);
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
#define O_EXCL   __OS_O_EXCL

/* The mode string: its open flags and its F_* access bits. 'b' is
 * accepted and means nothing, as on every POSIX system; 'e' is glibc's
 * close-on-exec and 'x' C11's exclusive creation. */
int __stdio_mode(const char *mode, int *oflags, int *acc)
{
    int fl = 0, a = 0;
    switch (mode[0]) {
    case 'r': a = F_READ;  fl = O_RDONLY; break;
    case 'w': a = F_WRITE; fl = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': a = F_WRITE | F_APPEND; fl = O_WRONLY | O_CREAT | O_APPEND;
              break;
    default:  errno = EINVAL; return -1;
    }
    if (strchr(mode, '+')) { a |= F_READ | F_WRITE;
                             fl = (fl & ~(O_RDONLY | O_WRONLY)) | O_RDWR; }
    /* C11's exclusive mode, "wx" and its forms: the open fails if the
     * file exists, and that test and the creation are one step. */
    if (mode[0] == 'w' && strchr(mode, 'x'))
        fl |= O_EXCL;
    *oflags = fl;
    *acc = a;
    return 0;
}

/* Claiming a FILE and opening the file are two steps, and the claim has
 * to happen under the table's lock or two threads take the same one.
 * F_USED is set while still holding it, so the FILE is unavailable to
 * anyone else before the open is even attempted; __stdio_unclaim gives
 * it back if the open fails. */
FILE *__stdio_claim(void)
{
    FILE *f = NULL;
    __lock(&files_lock);
    for (int i = 0; i < FOPEN_MAX && !f; i++)
        if (!(g_files[i].flags & F_USED))
            f = &g_files[i];
    for (struct chunk *c = g_chunks; c && !f; c = c->next)
        for (int i = 0; i < CHUNK && !f; i++)
            if (!(c->f[i].flags & F_USED))
                f = &c->f[i];
    if (!f) {
        struct chunk *c = calloc(1, sizeof *c);
        if (c) {
            c->next = g_chunks;
            g_chunks = c;
            f = &c->f[0];
        }
    }
    if (f) {
        memset(f, 0, sizeof *f);
        f->fd = -1;
        f->ungot = -1;
        f->flags = F_USED;
    }
    __unlock(&files_lock);
    if (!f)
        errno = EMFILE;
    return f;
}

void __stdio_unclaim(FILE *f)
{
    __lock(&files_lock);
    f->flags = 0;
    __unlock(&files_lock);
}

/* Fully buffered, with a buffer of its own -- or unbuffered when there
 * is no memory for one, which is slower but still correct. */
void __stdio_setup(FILE *f, int acc)
{
    f->flags = (f->flags & F_COOKIE) | acc | F_USED;
    f->buf = malloc(BUFSIZ);
    f->bufsz = f->buf ? BUFSIZ : 0;
    f->own_buf = f->buf != NULL;
    f->mode = f->buf ? _IOFBF : _IONBF;
    f->pos = f->len = 0;
    f->ungot = -1;
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    int fl, acc;
    if (__stdio_mode(mode, &fl, &acc) < 0)
        return NULL;
    FILE *f = __stdio_claim();
    if (!f)
        return NULL;
    int fd = __os_open(path, fl, 0666);
    if (fd < 0) {                  /* the backend set errno */
        __stdio_unclaim(f);
        return NULL;
    }
    f->fd = fd;
    __stdio_setup(f, acc);
    return f;
}

/* Everything fclose does but give the FILE back: what freopen needs. */
static int close_unlocked(FILE *f)
{
    int r = __stdio_flush(f);
    if (io_close(f) < 0) r = EOF;
    if (f->own_buf) free(f->buf);
    f->buf = NULL;
    f->own_buf = 0;
    return r;
}

int fclose(FILE *f)
{
    if (!f) return EOF;
    __flockfile(f);
    if (!(f->flags & F_USED)) { __funlockfile(f); return EOF; }
    int r = close_unlocked(f);
    f->flags = 0;                  /* the slot is free from here */
    f->depth = 0;
    __unlock(&f->lock);
    return r;
}

/* The SAME stream, reopened: freopen("log", "w", stdout) must leave
 * stdout itself writing to log, so the FILE is kept and only what it is
 * a stream of changes. A NULL path asks to change the mode of the file
 * already open, which needs an fcntl this library's OS contract does not
 * have; it fails, as POSIX allows. */
FILE *freopen(const char *restrict path, const char *restrict mode,
              FILE *restrict f)
{
    int fl, acc;
    if (!f) return NULL;
    __flockfile(f);
    if (f->flags & F_USED)
        close_unlocked(f);
    f->flags = F_USED;             /* closed, but not given back */
    if (!path || __stdio_mode(mode, &fl, &acc) < 0) {
        if (!path) errno = EBADF;
        goto fail;
    }
    int fd = __os_open(path, fl, 0666);
    if (fd < 0)
        goto fail;
    f->fd = fd;
    __stdio_setup(f, acc);
    f->io = (struct __stream_io){ 0 };
    f->cookie = NULL;
    __funlockfile(f);
    return f;
fail:
    f->flags = 0;
    f->depth = 0;
    __unlock(&f->lock);
    return NULL;
}

int setvbuf(FILE *restrict f, char *restrict buf, int mode, size_t size)
{
    if (!f) return -1;
    __flockfile(f);
    __stdio_flush(f);
    if (f->own_buf) { free(f->buf); f->own_buf = 0; }
    if (mode == _IONBF) { f->buf = NULL; f->bufsz = 0; }
    else if (buf)       { f->buf = (unsigned char *)buf; f->bufsz = size; }
    else { f->buf = malloc(size ? size : BUFSIZ);
           f->bufsz = f->buf ? (size ? size : BUFSIZ) : 0;
           f->own_buf = f->buf != NULL; }
    f->mode = f->buf ? mode : _IONBF;
    f->pos = f->len = 0;
    __funlockfile(f);
    return 0;
}

void setbuf(FILE *restrict f, char *restrict buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

static int seek_unlocked(FILE *f, long off, int whence)
{
    __stdio_flush(f);
    f->pos = f->len = 0;
    f->ungot = -1;
    f->flags &= ~F_EOF;
    return io_seek(f, off, whence) < 0 ? -1 : 0;
}

int fseek(FILE *f, long off, int whence)
{
    if (!f) return -1;
    __flockfile(f);
    int r = seek_unlocked(f, off, whence);
    __funlockfile(f);
    return r;
}

static long tell_unlocked(FILE *f)
{
    long at = io_seek(f, 0, SEEK_CUR);
    if (at < 0) return -1;
    /* What the OS thinks minus what is still sitting in our buffer. */
    if (f->flags & F_WRITING) return at + (long)f->pos;
    return at - (long)(f->len - f->pos) - (f->ungot >= 0 ? 1 : 0);
}

long ftell(FILE *f)
{
    if (!f) return -1;
    __flockfile(f);
    long r = tell_unlocked(f);
    __funlockfile(f);
    return r;
}

void rewind(FILE *f)
{
    /* Seek and clear under ONE lock: a rewind that another thread can
     * see half of is a stream at position zero still claiming EOF. */
    if (!f) return;
    __flockfile(f);
    seek_unlocked(f, 0, SEEK_SET);
    f->flags &= ~(F_EOF | F_ERR);
    __funlockfile(f);
}

int fgetpos(FILE *restrict f, fpos_t *restrict pos)
{
    if (!f) return -1;
    __flockfile(f);
    long at = tell_unlocked(f);
    __funlockfile(f);
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

/* ---- temporary files, C11 §7.21.4.3 and §7.21.4.4 ----
 *
 * Their names are under /tmp, the directory the rest of this library
 * calls temporary (lib/libcxx's temp_directory_path), and end in twelve
 * letters from __tmp_bits (internal/tmpname.h says why those).
 *
 * tmpnam's name is one no file had when it was made. Between that and
 * the caller's open is a race C cannot close, so tmpfile does not use
 * it: it opens exclusively, which fails if the name exists by then, and
 * tries another name; with read and write for the owner only; and
 * removes the name at once. The open stream keeps the file, which goes
 * when the stream closes or the program ends -- or, on a filesystem
 * that cannot remove an open file, is left behind. A target with no
 * filesystem has no tmpfile: NULL, with the backend's errno. */
static void tmp_name(char *buf)
{
    static const char abc[] = "abcdefghijklmnopqrstuvwxyz012345";
    unsigned long long r = __tmp_bits();
    memcpy(buf, "/tmp/", 5);
    for (int i = 0; i < 12; i++, r >>= 5)
        buf[5 + i] = abc[r & 31];
    buf[17] = 0;
}

char *tmpnam(char *s)
{
    static char internal[L_tmpnam];
    char name[L_tmpnam];
    int saved = errno;
    for (int tries = 0; tries < 100; tries++) {
        struct __os_fileinfo fi;
        tmp_name(name);
        /* Free if nothing is there -- or if there is nowhere for anything
         * to be. */
        if (__os_lstat(name, &fi) != 0 &&
            (errno == ENOENT || errno == ENOSYS)) {
            errno = saved;
            return strcpy(s ? s : internal, name);
        }
    }
    return NULL;
}

FILE *tmpfile(void)
{
    char name[L_tmpnam];
    for (int tries = 0; tries < 100; tries++) {
        tmp_name(name);
        FILE *f = __stdio_claim();
        if (!f)
            return NULL;
        int fd = __os_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            f->fd = fd;
            __stdio_setup(f, F_READ | F_WRITE);
            int saved = errno;
            __os_remove(name);
            errno = saved;
            return f;
        }
        __stdio_unclaim(f);
        if (errno != EEXIST)
            return NULL;
    }
    return NULL;
}
