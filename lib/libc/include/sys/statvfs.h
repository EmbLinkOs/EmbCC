/* <sys/statvfs.h> -- what a filesystem says about itself */
#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H

#include <features.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};

#define ST_RDONLY 1
#define ST_NOSUID 2

int statvfs(const char *__restrict path, struct statvfs *__restrict out);
int fstatvfs(int fd, struct statvfs *out);

#if defined(__EMB_GNU) || defined(_LARGEFILE64_SOURCE)
/* off_t is 64 bits here: the large-file names are the same calls */
#define statvfs64 statvfs
#define fstatvfs64 fstatvfs
#endif

#ifdef __cplusplus
}
#endif

#endif
