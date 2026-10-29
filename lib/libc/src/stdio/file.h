/* The FILE object, shared by the stdio sources and by nothing else.
 *
 * Buffering lives here rather than in the OS backend on purpose: a backend
 * should move bytes and nothing more, so every target gets the same
 * buffering behaviour and the same bugs fixed once. */
#ifndef EMBLIBC_FILE_H
#define EMBLIBC_FILE_H

#include <stdio.h>
#include <wchar.h>

#include "../internal/lock.h"

/* The four operations of a stream that is NOT a descriptor -- fmemopen's,
 * open_memstream's, fopencookie's. Laid out as glibc's
 * cookie_io_functions_t, which fopencookie copies in whole. A stream
 * carries them only with F_COOKIE set; every other stream reads and
 * writes its descriptor through the backend, as it always has. */
struct __stream_io {
    __PTRDIFF_TYPE__ (*read)(void *cookie, char *buf, size_t n);
    __PTRDIFF_TYPE__ (*write)(void *cookie, const char *buf, size_t n);
    int (*seek)(void *cookie, long long *off, int whence);
    int (*close)(void *cookie);
};

struct _FILE {
    int fd;
    int flags;
    unsigned char *buf;
    size_t bufsz;
    size_t pos;          /* next byte to read/write within buf */
    size_t len;          /* bytes of valid data in buf (reading) */
    int mode;            /* _IOFBF / _IOLBF / _IONBF */
    int ungot;           /* ungetc slot, or -1 */
    int own_buf;
    /* C11 7.21.2/7: each stream has a lock, and every function that
     * touches one holds it for the WHOLE call. Per stream rather than
     * one for all of stdio, because two threads writing to two different
     * files have nothing to say to each other. Last, so the positional
     * initialisers of the three standard streams stay readable. */
    __lock_t lock;
    /* The lock NESTS, for POSIX's flockfile: a thread that holds a
     * stream may call printf on it, which takes the lock again. owner is
     * meaningful only while depth > 0 -- the thread holding it, by
     * __os_thread_self, which is 0 for every caller where there are no
     * threads, and there the depth alone does the counting. */
    unsigned long owner;
    int depth;
    struct __stream_io io;         /* F_COOKIE streams only */
    void *cookie;
};

#define F_READ   0x01
#define F_WRITE  0x02
#define F_EOF    0x04
#define F_ERR    0x08
#define F_APPEND 0x10
#define F_USED   0x20
#define F_WRITING 0x40   /* the buffer currently holds unwritten output */
/* Orientation (C11 7.21.2p4): none until the first byte or wide operation
 * on the stream, and then that one until freopen. A wide stream's bytes
 * are UTF-8, read and written through the byte functions below -- which
 * is why those set F_BYTE only when neither is set yet. On a wide stream
 * `ungot` holds a wide character: there are no byte reads to see it. */
#define F_WIDE   0x80
#define F_BYTE   0x100
/* The stream is its io and cookie, not a descriptor (fd is -1). */
#define F_COOKIE 0x200
#define __byte_orient(f) \
    ((f)->flags & (F_WIDE | F_BYTE) ? (void)0 : (void)((f)->flags |= F_BYTE))

void __stdio_flush_all(void);
int  __stdio_flush(FILE *f);

/* Opening, in three steps that every way of opening shares (fopen,
 * fdopen, fopencookie, the memory streams): a free FILE, taken under the
 * table's lock and marked F_USED, or NULL with EMFILE; the mode string's
 * open flags and F_READ/F_WRITE/F_APPEND bits, or -1 with EINVAL; and the
 * buffer, once the stream has its fd or io. A FILE claimed and then not
 * wanted is given back with __stdio_unclaim. */
FILE *__stdio_claim(void);
void  __stdio_unclaim(FILE *f);
int   __stdio_mode(const char *mode, int *oflags, int *acc);
void  __stdio_setup(FILE *f, int acc);

/* The unlocked halves, for callers that ALREADY hold f's lock.
 *
 * They exist because the lock has to be taken at the outermost public
 * entry and nowhere else. `puts` is the argument for it: it writes a
 * string and then a newline, and if each took the lock separately
 * another thread could put a line between them -- which is exactly the
 * interleaving the per-stream lock is there to prevent. printf is the
 * same argument at greater length, and fwrite calling fputc per byte is
 * the same argument for efficiency. */
void   __flockfile(FILE *f);
void   __funlockfile(FILE *f);
int    __putc_unlocked(int c, FILE *f);
int    __getc_unlocked(FILE *f);
int    __ungetc_unlocked(int c, FILE *f);
size_t __fwrite_unlocked(const void *p, size_t size, size_t n, FILE *f);

/* The formatted-output engine, shared by every printf. `sink` takes one
 * chunk at a time so the same code serves a FILE and a fixed buffer. */
int __vformat(void (*sink)(void *, const char *, size_t), void *ctx,
              const char *fmt, va_list ap);

/* The formatted-INPUT engine, its mirror. get/unget rather than a source
 * callback because scanning needs one character of lookahead; see scan.c. */
int __vscan(int (*get)(void *), void (*unget)(void *, int), void *ctx,
            const char *fmt, va_list ap);

#endif
