/* <sys/wait.h> -- waiting for a child (src/posix/proc.c). The status
 * is encoded as Linux encodes it: an exit is (code << 8), a signal its
 * number, a stop 0x7f with the signal above it. */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   WUNTRACED
#define WCONTINUED 8
#define WEXITED    4
#define WNOWAIT    0x01000000

typedef enum { P_ALL = 0, P_PID = 1, P_PGID = 2, P_PIDFD = 3 } idtype_t;

#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WSTOPSIG(s)    WEXITSTATUS(s)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WCOREDUMP(s)   ((s) & 0x80)

struct rusage;

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait3(int *status, int options, struct rusage *ru);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru);
int   waitid(idtype_t idtype, id_t id, siginfo_t *info, int options);

#ifdef __cplusplus
}
#endif

#endif
