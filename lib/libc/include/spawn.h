/* <spawn.h> -- posix_spawn: a new process running a program, set up by
 * a list of file actions and attributes (src/posix/spawn.c), which is how
 * a program starts another on a kernel that cannot fork. */
#ifndef _SPAWN_H
#define _SPAWN_H

#include <sys/types.h>
#include <bits/sigset.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER  0x20
#define POSIX_SPAWN_USEVFORK      0x40
#define POSIX_SPAWN_SETSID        0x80

typedef struct {
    int __n, __cap;
    struct __os_spawn_action *__a;
    char **__paths;             /* copies of the paths, freed with it */
} posix_spawn_file_actions_t;

typedef struct {
    short __flags;
    pid_t __pgroup;
    sigset_t __sigdefault, __sigmask;
    int __policy;
    struct sched_param __param;
} posix_spawnattr_t;

int posix_spawn(pid_t *__restrict pid, const char *__restrict path,
                const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *__restrict attr,
                char *const argv[__restrict], char *const envp[__restrict]);
int posix_spawnp(pid_t *__restrict pid, const char *__restrict file,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *__restrict attr,
                 char *const argv[__restrict], char *const envp[__restrict]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__restrict fa,
                                     int fd, const char *__restrict path,
                                     int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd,
                                     int newfd);
/* POSIX.1-2024's, and their glibc names before that */
int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *__restrict fa,
                                      const char *__restrict path);
int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *__restrict fa,
                                         const char *__restrict path);
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *fa,
                                          int fd);

int posix_spawnattr_init(posix_spawnattr_t *a);
int posix_spawnattr_destroy(posix_spawnattr_t *a);
int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict a,
                             short *__restrict flags);
int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *__restrict a,
                              pid_t *__restrict pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t pgroup);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *__restrict a,
                                  sigset_t *__restrict set);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *__restrict a,
                                  const sigset_t *__restrict set);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *__restrict a,
                               sigset_t *__restrict set);
int posix_spawnattr_setsigmask(posix_spawnattr_t *__restrict a,
                               const sigset_t *__restrict set);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *__restrict a,
                                   int *__restrict policy);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *a, int policy);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *__restrict a,
                                  struct sched_param *__restrict p);
int posix_spawnattr_setschedparam(posix_spawnattr_t *__restrict a,
                                  const struct sched_param *__restrict p);

#ifdef __cplusplus
}
#endif

#endif
