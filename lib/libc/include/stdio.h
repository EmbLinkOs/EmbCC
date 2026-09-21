/* <stdio.h> — C11 §7.21. */
#ifndef _STDIO_H
#define _STDIO_H

/* These declarations are reached from C++ too -- the C++ runtime in
 * lib/libcxx sits on this library -- so they carry C linkage explicitly.
 * Without it a C++ translation unit mangles every name in here and the
 * link fails on symbols that are right there in the archive. */

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)
#define BUFSIZ 4096
#define FOPEN_MAX 32
#define FILENAME_MAX 4096
#define L_tmpnam 32
#define TMP_MAX 26

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef struct _FILE FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *__restrict path, const char *__restrict mode);
FILE *freopen(const char *__restrict path, const char *__restrict mode,
              FILE *__restrict f);
int   fclose(FILE *f);
int   fflush(FILE *f);
int   setvbuf(FILE *__restrict f, char *__restrict buf, int mode, size_t size);
void  setbuf(FILE *__restrict f, char *__restrict buf);

size_t fread(void *__restrict p, size_t size, size_t n, FILE *__restrict f);
size_t fwrite(const void *__restrict p, size_t size, size_t n,
              FILE *__restrict f);

int  fgetc(FILE *f);
int  getc(FILE *f);
int  getchar(void);
int  ungetc(int c, FILE *f);
char *fgets(char *__restrict s, int n, FILE *__restrict f);
int  fputc(int c, FILE *f);
int  putc(int c, FILE *f);
int  putchar(int c);
int  fputs(const char *__restrict s, FILE *__restrict f);
int  puts(const char *s);

int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int  fgetpos(FILE *__restrict f, fpos_t *__restrict pos);
int  fsetpos(FILE *f, const fpos_t *pos);

void clearerr(FILE *f);
int  feof(FILE *f);
int  ferror(FILE *f);
void perror(const char *s);

int remove(const char *path);
int rename(const char *from, const char *to);

/* Which argument is the format, and where the tail it describes begins
 * -- both 1-based, as GCC has spelled this for thirty years. It is what
 * lets -Wformat check a call, and the knowledge belongs HERE rather
 * than in a list of names inside the compiler: a compiler that knew
 * "printf" would still know nothing about anyone's own log().
 *
 * The v-forms take a va_list, so there is no tail to compare against
 * and the second number is 0 -- the string is still checked for a
 * conversion that does not exist. */
#define __fmt(kind, m, n) __attribute__((format(kind, m, n)))

int printf(const char *__restrict fmt, ...)               __fmt(printf, 1, 2);
int fprintf(FILE *__restrict f, const char *__restrict fmt, ...)
                                                          __fmt(printf, 2, 3);
int sprintf(char *__restrict s, const char *__restrict fmt, ...)
                                                          __fmt(printf, 2, 3);
int snprintf(char *__restrict s, size_t n, const char *__restrict fmt, ...)
                                                          __fmt(printf, 3, 4);
int vprintf(const char *__restrict fmt, va_list ap)       __fmt(printf, 1, 0);
int vfprintf(FILE *__restrict f, const char *__restrict fmt, va_list ap)
                                                          __fmt(printf, 2, 0);
int vsprintf(char *__restrict s, const char *__restrict fmt, va_list ap)
                                                          __fmt(printf, 2, 0);
int vsnprintf(char *__restrict s, size_t n, const char *__restrict fmt,
              va_list ap)                                 __fmt(printf, 3, 0);

int scanf(const char *__restrict fmt, ...)                __fmt(scanf, 1, 2);
int fscanf(FILE *__restrict f, const char *__restrict fmt, ...)
                                                          __fmt(scanf, 2, 3);
int sscanf(const char *__restrict s, const char *__restrict fmt, ...)
                                                          __fmt(scanf, 2, 3);
int vscanf(const char *__restrict fmt, va_list ap)        __fmt(scanf, 1, 0);
int vfscanf(FILE *__restrict f, const char *__restrict fmt, va_list ap)
                                                          __fmt(scanf, 2, 0);
int vsscanf(const char *__restrict s, const char *__restrict fmt, va_list ap)
                                                          __fmt(scanf, 2, 0);

#ifdef __cplusplus
}
#endif

#endif
