/* <stdio.h> — C11 §7.21. */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

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

FILE *fopen(const char *restrict path, const char *restrict mode);
FILE *freopen(const char *restrict path, const char *restrict mode,
              FILE *restrict f);
int   fclose(FILE *f);
int   fflush(FILE *f);
int   setvbuf(FILE *restrict f, char *restrict buf, int mode, size_t size);
void  setbuf(FILE *restrict f, char *restrict buf);

size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f);
size_t fwrite(const void *restrict p, size_t size, size_t n,
              FILE *restrict f);

int  fgetc(FILE *f);
int  getc(FILE *f);
int  getchar(void);
int  ungetc(int c, FILE *f);
char *fgets(char *restrict s, int n, FILE *restrict f);
int  fputc(int c, FILE *f);
int  putc(int c, FILE *f);
int  putchar(int c);
int  fputs(const char *restrict s, FILE *restrict f);
int  puts(const char *s);

int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int  fgetpos(FILE *restrict f, fpos_t *restrict pos);
int  fsetpos(FILE *f, const fpos_t *pos);

void clearerr(FILE *f);
int  feof(FILE *f);
int  ferror(FILE *f);
void perror(const char *s);

int remove(const char *path);
int rename(const char *from, const char *to);

int printf(const char *restrict fmt, ...);
int fprintf(FILE *restrict f, const char *restrict fmt, ...);
int sprintf(char *restrict s, const char *restrict fmt, ...);
int snprintf(char *restrict s, size_t n, const char *restrict fmt, ...);
int vprintf(const char *restrict fmt, va_list ap);
int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap);
int vsprintf(char *restrict s, const char *restrict fmt, va_list ap);
int vsnprintf(char *restrict s, size_t n, const char *restrict fmt,
              va_list ap);

int scanf(const char *restrict fmt, ...);
int fscanf(FILE *restrict f, const char *restrict fmt, ...);
int sscanf(const char *restrict s, const char *restrict fmt, ...);
int vscanf(const char *restrict fmt, va_list ap);
int vfscanf(FILE *restrict f, const char *restrict fmt, va_list ap);
int vsscanf(const char *restrict s, const char *restrict fmt, va_list ap);

#endif
