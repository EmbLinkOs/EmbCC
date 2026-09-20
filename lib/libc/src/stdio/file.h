/* The FILE object, shared by the stdio sources and by nothing else.
 *
 * Buffering lives here rather than in the OS backend on purpose: a backend
 * should move bytes and nothing more, so every target gets the same
 * buffering behaviour and the same bugs fixed once. */
#ifndef EMBLIBC_FILE_H
#define EMBLIBC_FILE_H

#include <stdio.h>

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

/* The formatted-output engine, shared by every printf. `sink` takes one
 * chunk at a time so the same code serves a FILE and a fixed buffer. */
int __vformat(void (*sink)(void *, const char *, size_t), void *ctx,
              const char *fmt, va_list ap);

#endif
