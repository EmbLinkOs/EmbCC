/* <dirent.h> -- reading a directory (src/posix/dir.c). `.` and `..`
 * are returned where the OS has them, as POSIX says. */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <features.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    ino_t d_ino;
    off_t d_off;                /* this entry's place, for telldir */
    unsigned short d_reclen;
    unsigned char d_type;       /* DT_* */
    char d_name[256];
};

typedef struct __dirstream DIR;

DIR           *opendir(const char *path);
DIR           *fdopendir(int fd);
struct dirent *readdir(DIR *d);
int            readdir_r(DIR *__restrict d, struct dirent *__restrict e,
                         struct dirent **__restrict result);
void           rewinddir(DIR *d);
long           telldir(DIR *d);
void           seekdir(DIR *d, long at);
int            closedir(DIR *d);
int            dirfd(DIR *d);
int            alphasort(const struct dirent **a, const struct dirent **b);
int            scandir(const char *path, struct dirent ***list,
                       int (*keep)(const struct dirent *),
                       int (*cmp)(const struct dirent **,
                                  const struct dirent **));
#if defined(__EMB_GNU)
int            versionsort(const struct dirent **a, const struct dirent **b);
#endif

#ifdef __cplusplus
}
#endif

#endif
