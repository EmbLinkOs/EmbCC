/* Running a program, for hosts with POSIX's posix_spawn: macOS and Linux.
 * The driver uses it to compile several sources in one command
 * (platform.h). EmbLinkOS has no fork/exec and builds process_none.c
 * instead (`make PROCESS=none`, and tools/gen-embbuild-manifest.sh). */
#include "platform.h"

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* The children still running, by handle; a handle is a slot here. */
#define MAXRUN 256
static pid_t run_pid[MAXRUN];

int plat_can_run(void)
{
    return 1;
}

int plat_run_start(const char *const argv[])
{
    int h = 0;
    while (h < MAXRUN && run_pid[h])
        h++;
    if (h == MAXRUN)
        return -1;
    pid_t pid;
    /* posix_spawn takes `char *const []`: the strings are not written */
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv,
                    environ) != 0)
        return -1;
    run_pid[h] = pid;
    return h;
}

int plat_run_wait(int *which)
{
    int any = 0;
    for (int h = 0; h < MAXRUN; h++)
        any |= run_pid[h] != 0;
    if (!any)
        return -1;
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0)
            return -1;
        for (int h = 0; h < MAXRUN; h++)
            if (run_pid[h] == pid) {
                run_pid[h] = 0;
                *which = h;
                if (WIFEXITED(st))
                    return WEXITSTATUS(st);
                return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
            }
        /* a child this table never started: not ours to report */
    }
}

int plat_ncpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 && n < 1024 ? (int)n : 1;
}
