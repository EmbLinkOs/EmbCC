/* The memory streams: fmemopen, a stream over a buffer of a fixed size,
 * and open_memstream, a write stream into memory that grows.
 *
 * Both are ordinary buffered streams whose io is here (file.h's
 * __stream_io): what a program wrote is in its memory once the stream is
 * flushed or closed, as POSIX says, and not necessarily before -- so a
 * program must fflush before reading the buffer, as it must with glibc.
 */
#include "file.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

static FILE *open_io(void *cookie, const struct __stream_io *io, int acc)
{
    FILE *f = __stdio_claim();
    if (!f)
        return NULL;
    f->flags |= F_COOKIE;
    f->cookie = cookie;
    f->io = *io;
    __stdio_setup(f, acc);
    return f;
}

/* ---- fmemopen ----------------------------------------------------------
 *
 * The rules are POSIX.1-2008's as glibc implements them since 2.22:
 *
 *   size   the buffer's; nothing is ever written past it
 *   end    how much of it holds data: all of it for "r", none for "w"
 *          (which also writes a NUL at the start), up to the first NUL
 *          for "a" -- and every append goes there
 *   NUL    a write that moves `end` puts a NUL after the data when there
 *          is room, so the buffer is a string; a write that fills the
 *          buffer puts one in its last byte instead, unless the data
 *          itself ended with one or the stream appends
 *
 * A seek may go anywhere from 0 to size. Reading stops at `end`. */
struct mem {
    char *buf;
    size_t size, pos, end;
    int append, own;
};

static ssize_t mem_read(void *c, char *b, size_t n)
{
    struct mem *m = c;
    size_t have = m->pos < m->end ? m->end - m->pos : 0;
    if (n > have)
        n = have;
    memcpy(b, m->buf + m->pos, n);
    m->pos += n;
    return (ssize_t)n;
}

static ssize_t mem_write(void *c, const char *b, size_t n)
{
    struct mem *m = c;
    int nul = n == 0 || b[n - 1] != 0;
    if (m->append)
        m->pos = m->end;
    if (m->pos + n > m->size) {
        if (m->pos + nul >= m->size) {
            errno = ENOSPC;
            return 0;
        }
        n = m->size - m->pos;
    }
    memcpy(m->buf + m->pos, b, n);
    m->pos += n;
    if (m->pos > m->end) {
        m->end = m->pos;
        if (m->end < m->size && nul)
            m->buf[m->end] = 0;
        else if (!m->append && nul)
            m->buf[m->size - 1] = 0;
    }
    return (ssize_t)n;
}

static int mem_seek(void *c, long long *off, int whence)
{
    struct mem *m = c;
    long long base = whence == SEEK_SET ? 0
                   : whence == SEEK_CUR ? (long long)m->pos
                   : whence == SEEK_END ? (long long)m->end : -1;
    if (base < 0 || *off < -base || base + *off > (long long)m->size) {
        errno = EINVAL;
        return -1;
    }
    m->pos = (size_t)(base + *off);
    *off = (long long)m->pos;
    return 0;
}

static int mem_close(void *c)
{
    struct mem *m = c;
    if (m->own)
        free(m->buf);
    free(m);
    return 0;
}

static const struct __stream_io mem_io = {
    mem_read, mem_write, mem_seek, mem_close
};

/* A NULL buf asks for one of size bytes, freed at close: useful only
 * with "+", since nothing else could read what was written there. */
FILE *fmemopen(void *restrict buf, size_t size, const char *restrict mode)
{
    int fl, acc;
    if (size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (__stdio_mode(mode, &fl, &acc) < 0)
        return NULL;
    struct mem *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->buf = buf;
    if (!buf) {
        m->buf = calloc(1, size);
        m->own = 1;
        if (!m->buf) {
            free(m);
            return NULL;
        }
    }
    m->size = size;
    switch (mode[0]) {
    case 'r': m->end = buf ? size : 0; break;
    case 'w': m->end = 0; m->buf[0] = 0; break;
    default:  m->append = 1;
              m->pos = m->end = strnlen(m->buf, size); break;
    }
    FILE *f = open_io(m, &mem_io, acc);
    if (!f)
        mem_close(m);
    return f;
}

/* ---- open_memstream ----------------------------------------------------
 *
 * Write-only, into memory that grows. After every flush *bufp is the
 * memory and *sizep the position, with a NUL there; a seek past the end
 * and a write make a gap, which reads as zeros. The memory is the
 * caller's to free, after fclose. */
struct dyn {
    char **bufp;
    size_t *sizep;
    char *buf;
    size_t cap, pos, len;
};

static void publish(struct dyn *d)
{
    *d->bufp = d->buf;
    *d->sizep = d->pos < d->len ? d->pos : d->len;
}

static ssize_t dyn_write(void *c, const char *b, size_t n)
{
    struct dyn *d = c;
    if (d->pos + n + 1 > d->cap) {
        size_t cap = d->cap * 2;
        if (cap < d->pos + n + 1)
            cap = d->pos + n + 1;
        char *p = realloc(d->buf, cap);
        if (!p) {
            errno = ENOMEM;
            return 0;
        }
        memset(p + d->cap, 0, cap - d->cap);
        d->buf = p;
        d->cap = cap;
    }
    memcpy(d->buf + d->pos, b, n);
    d->pos += n;
    if (d->pos > d->len)
        d->len = d->pos;
    d->buf[d->len] = 0;
    publish(d);
    return (ssize_t)n;
}

static int dyn_seek(void *c, long long *off, int whence)
{
    struct dyn *d = c;
    long long base = whence == SEEK_SET ? 0
                   : whence == SEEK_CUR ? (long long)d->pos
                   : whence == SEEK_END ? (long long)d->len : -1;
    if (base < 0 || *off < -base) {
        errno = EINVAL;
        return -1;
    }
    d->pos = (size_t)(base + *off);
    *off = (long long)d->pos;
    publish(d);
    return 0;
}

static int dyn_close(void *c)
{
    publish(c);
    free(c);
    return 0;
}

static const struct __stream_io dyn_io = { NULL, dyn_write, dyn_seek,
                                           dyn_close };

FILE *open_memstream(char **bufp, size_t *sizep)
{
    if (!bufp || !sizep) {
        errno = EINVAL;
        return NULL;
    }
    struct dyn *d = calloc(1, sizeof *d);
    char *buf = calloc(1, 64);
    if (!d || !buf) {
        free(d);
        free(buf);
        return NULL;
    }
    *d = (struct dyn){ bufp, sizep, buf, 64, 0, 0 };
    publish(d);
    FILE *f = open_io(d, &dyn_io, F_WRITE);
    if (!f) {
        free(buf);
        free(d);
    }
    return f;
}
