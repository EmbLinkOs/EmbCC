/* <ftw.h> -- walking a file tree: POSIX's nftw and the older ftw, with
 * glibc's numbers and GNU's FTW_ACTIONRETVAL results. */
#ifndef _FTW_H
#define _FTW_H

#include <features.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* what the callback is told it was given */
#define FTW_F   0              /* a file */
#define FTW_D   1              /* a directory, before its entries */
#define FTW_DNR 2              /* a directory that could not be read */
#define FTW_NS  3              /* stat failed on it */
#define FTW_SL  4              /* a symbolic link (with FTW_PHYS) */
#define FTW_DP  5              /* a directory, after its entries (FTW_DEPTH) */
#define FTW_SLN 6              /* a link to nothing */

/* nftw's flags */
#define FTW_PHYS  1            /* do not follow links */
#define FTW_MOUNT 2            /* stay on one filesystem */
#define FTW_CHDIR 4            /* chdir into each directory first */
#define FTW_DEPTH 8            /* a directory after its entries */
#if defined(__EMB_GNU)
#define FTW_ACTIONRETVAL 16    /* the callback's result steers the walk */
#define FTW_CONTINUE      0
#define FTW_STOP          1
#define FTW_SKIP_SUBTREE  2
#define FTW_SKIP_SIBLINGS 3
#endif

struct FTW {
    int base;                  /* where the name starts in the path */
    int level;                 /* depth below the starting point */
};

int ftw(const char *path,
        int (*fn)(const char *path, const struct stat *st, int type),
        int fds);
int nftw(const char *path,
         int (*fn)(const char *path, const struct stat *st, int type,
                   struct FTW *ftw),
         int fds, int flags);

#ifdef __cplusplus
}
#endif

#endif
