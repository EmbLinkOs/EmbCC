/* <glob.h> -- pathname patterns, as the shell expands them: POSIX's
 * glob and globfree, with glibc's numbers and GNU's extensions (braces,
 * tildes, GLOB_ONLYDIR, GLOB_PERIOD, the alternative directory functions). */
#ifndef _GLOB_H
#define _GLOB_H

#include <features.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat;
struct dirent;

typedef struct {
    size_t gl_pathc;           /* how many matched */
    char **gl_pathv;           /* gl_offs nulls, the matches, a null */
    size_t gl_offs;            /* with GLOB_DOOFFS, how many nulls first */
    int gl_flags;              /* the flags given, and GLOB_MAGCHAR */
    /* GLOB_ALTDIRFUNC: these instead of opendir, readdir, closedir,
     * lstat and stat */
    void (*gl_closedir)(void *);
    struct dirent *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *__restrict, struct stat *__restrict);
    int (*gl_stat)(const char *__restrict, struct stat *__restrict);
} glob_t;

#define GLOB_ERR         (1 << 0)
#define GLOB_MARK        (1 << 1)
#define GLOB_NOSORT      (1 << 2)
#define GLOB_DOOFFS      (1 << 3)
#define GLOB_NOCHECK     (1 << 4)
#define GLOB_APPEND      (1 << 5)
#define GLOB_NOESCAPE    (1 << 6)
#define GLOB_PERIOD      (1 << 7)
#define GLOB_MAGCHAR     (1 << 8)
#define GLOB_ALTDIRFUNC  (1 << 9)
#define GLOB_BRACE       (1 << 10)
#define GLOB_NOMAGIC     (1 << 11)
#define GLOB_TILDE       (1 << 12)
#define GLOB_ONLYDIR     (1 << 13)
#define GLOB_TILDE_CHECK (1 << 14)

#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
#define GLOB_NOSYS   4

int  glob(const char *__restrict pattern, int flags,
          int (*errfunc)(const char *path, int err), glob_t *__restrict g);
void globfree(glob_t *g);
#if defined(__EMB_GNU)
int  glob_pattern_p(const char *pattern, int quote);
#endif

#ifdef __cplusplus
}
#endif

#endif
