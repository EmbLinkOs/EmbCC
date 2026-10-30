/* <sys/file.h> -- flock(2)'s whole-file advisory locks */
#ifndef _SYS_FILE_H
#define _SYS_FILE_H

#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int op);

#ifdef __cplusplus
}
#endif

#endif
