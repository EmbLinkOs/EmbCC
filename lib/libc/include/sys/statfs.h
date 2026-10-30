/* <sys/statfs.h> -- Linux's statfs (src/posix/linuxapi.c); elsewhere
 * filled in from statvfs. */
#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int __val[2]; } fsid_t;

struct statfs {
    long f_type;
    long f_bsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsid_t f_fsid;
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
#if defined(_LARGEFILE64_SOURCE)
#define statfs64 statfs
#define fstatfs64 fstatfs
#endif

#ifdef __cplusplus
}
#endif

#endif
