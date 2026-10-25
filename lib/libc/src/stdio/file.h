/* The FILE object, shared by the stdio sources and by nothing else.
 *
 * Buffering lives here rather than in the OS backend on purpose: a backend
 * should move bytes and nothing more, so every target gets the same
 * buffering behaviour and the same bugs fixed once. */
#ifndef EMBLIBC_FILE_H
#define EMBLIBC_FILE_H

#include <stdio.h>

#include "../internal/lock.h"

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
};

#define F_READ   0x01
#define F_WRITE  0x02
#define F_EOF    0x04
#define F_ERR    0x08
#define F_APPEND 0x10
#define F_USED   0x20
#define F_WRITING 0x40   /* the buffer currently holds unwritten output */

void __stdio_flush_all(void);
int  __stdio_flush(FILE *f);

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
