/* <stdio.h> — C11 §7.21. */
#ifndef _STDIO_H
#define _STDIO_H

/* These declarations are reached from C++ too -- the C++ runtime in
 * lib/libcxx sits on this library -- so they carry C linkage explicitly.
 * Without it a C++ translation unit mangles every name in here and the
 * link fails on symbols that are right there in the archive. */

#include <stddef.h>
#include <stdarg.h>
#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)
#if defined(__APPLE__)
/* Apple's C library's, which a Darwin program links against: L_tmpnam
 * above all, since its tmpnam() writes up to that many bytes into the
 * caller's buffer -- 32 here was 992 short. */
#define BUFSIZ 1024
#define FOPEN_MAX 20
#define FILENAME_MAX 1024
#define L_tmpnam 1024
#define TMP_MAX 308915776
#else
#define BUFSIZ 4096
#define FOPEN_MAX 32
#define FILENAME_MAX 4096
#define L_tmpnam 32
#define TMP_MAX 26
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* <wchar.h> declares FILE too, for the wide stream functions. */
#ifndef __EMBLIBC_FILE
#define __EMBLIBC_FILE
typedef struct _FILE FILE;
#endif
typedef long fpos_t;

#if defined(__APPLE__)
/* Apple's C library names its streams __stdinp, __stdoutp and __stderrp;
 * `stderr` alone did not link there. */
extern FILE *__stdinp;
extern FILE *__stdoutp;
extern FILE *__stderrp;
#define stdin  __stdinp
#define stdout __stdoutp
#define stderr __stderrp
#else
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#endif

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
FILE *tmpfile(void);
char *tmpnam(char *s);

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

/* POSIX's, which ISO C leaves to the program (see <features.h>) */
#if defined(__EMB_POSIX)
#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef __PTRDIFF_TYPE__ ssize_t;
#endif
#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif
#define L_ctermid 9
#define P_tmpdir  "/tmp"

FILE   *fdopen(int fd, const char *mode);
int     fileno(FILE *f);
FILE   *fmemopen(void *__restrict buf, size_t size, const char *__restrict mode);
FILE   *open_memstream(char **bufp, size_t *sizep);
ssize_t getline(char **__restrict line, size_t *__restrict n,
                FILE *__restrict f);
ssize_t getdelim(char **__restrict line, size_t *__restrict n, int delim,
                 FILE *__restrict f);
int dprintf(int fd, const char *__restrict fmt, ...)      __fmt(printf, 2, 3);
int vdprintf(int fd, const char *__restrict fmt, va_list ap)
                                                          __fmt(printf, 2, 0);
int asprintf(char **__restrict s, const char *__restrict fmt, ...)
                                                          __fmt(printf, 2, 3);
int vasprintf(char **__restrict s, const char *__restrict fmt, va_list ap)
                                                          __fmt(printf, 2, 0);
int     fseeko(FILE *f, off_t off, int whence);
off_t   ftello(FILE *f);
char   *ctermid(char *s);
/* a command's output or input as a stream (src/posix/popen.c) */
FILE   *popen(const char *cmd, const char *mode);
int     pclose(FILE *f);
int     renameat(int fromfd, const char *from, int tofd, const char *to);

/* One lock around many calls: the stream's lock nests, so a thread that
 * holds it may still call printf on the stream. */
void flockfile(FILE *f);
int  ftrylockfile(FILE *f);
void funlockfile(FILE *f);
int  getc_unlocked(FILE *f);
int  getchar_unlocked(void);
int  putc_unlocked(int c, FILE *f);
int  putchar_unlocked(int c);
#endif

#if defined(__EMB_GNU)
#ifndef _OFF64_T_DECLARED
#define _OFF64_T_DECLARED
typedef long long off64_t;
#endif
/* A stream on the caller's four functions: glibc's fopencookie. */
typedef ssize_t cookie_read_function_t(void *cookie, char *buf, size_t n);
typedef ssize_t cookie_write_function_t(void *cookie, const char *buf,
                                        size_t n);
typedef int cookie_seek_function_t(void *cookie, off64_t *off, int whence);
typedef int cookie_close_function_t(void *cookie);
typedef struct {
    cookie_read_function_t  *read;
    cookie_write_function_t *write;
    cookie_seek_function_t  *seek;
    cookie_close_function_t *close;
} cookie_io_functions_t;
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io);

int    fgetc_unlocked(FILE *f);
int    fputc_unlocked(int c, FILE *f);
int    fflush_unlocked(FILE *f);
int    feof_unlocked(FILE *f);
int    ferror_unlocked(FILE *f);
void   clearerr_unlocked(FILE *f);
int    fileno_unlocked(FILE *f);
size_t fread_unlocked(void *__restrict p, size_t size, size_t n,
                      FILE *__restrict f);
size_t fwrite_unlocked(const void *__restrict p, size_t size, size_t n,
                       FILE *__restrict f);
int    fputs_unlocked(const char *__restrict s, FILE *__restrict f);
void   setbuffer(FILE *f, char *buf, size_t n);
void   setlinebuf(FILE *f);
char  *fgets_unlocked(char *__restrict s, int n, FILE *__restrict f);
int    fcloseall(void);
char  *tmpnam_r(char *s);
#endif

#if defined(__EMB_POSIX)
/* XSI's, withdrawn from POSIX.1-2008 and still everywhere */
int    getw(FILE *f);
int    putw(int w, FILE *f);
char  *tempnam(const char *dir, const char *prefix);
#define L_cuserid 9
char  *cuserid(char *s);
#endif

#if defined(__EMB_GNU) || defined(_LARGEFILE64_SOURCE)
/* off_t is 64 bits here: the large-file names are the same calls */
#define fopen64 fopen
#define freopen64 freopen
#define tmpfile64 tmpfile
#define fseeko64 fseeko
#define ftello64 ftello
#define fgetpos64 fgetpos
#define fsetpos64 fsetpos
#define fpos64_t fpos_t
#endif

#ifdef __cplusplus
}
#endif

#endif
