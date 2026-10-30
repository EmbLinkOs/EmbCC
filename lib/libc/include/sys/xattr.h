/* <sys/xattr.h> -- extended attributes, on Linux (src/posix/linuxapi.c). */
#ifndef _SYS_XATTR_H
#define _SYS_XATTR_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XATTR_CREATE  1
#define XATTR_REPLACE 2

ssize_t getxattr(const char *path, const char *name, void *value, size_t n);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t n);
ssize_t fgetxattr(int fd, const char *name, void *value, size_t n);
int     setxattr(const char *path, const char *name, const void *value,
                 size_t n, int flags);
int     lsetxattr(const char *path, const char *name, const void *value,
                  size_t n, int flags);
int     fsetxattr(int fd, const char *name, const void *value, size_t n,
                  int flags);
ssize_t listxattr(const char *path, char *list, size_t n);
ssize_t llistxattr(const char *path, char *list, size_t n);
ssize_t flistxattr(int fd, char *list, size_t n);
int     removexattr(const char *path, const char *name);
int     lremovexattr(const char *path, const char *name);
int     fremovexattr(int fd, const char *name);

#ifdef __cplusplus
}
#endif

#endif
