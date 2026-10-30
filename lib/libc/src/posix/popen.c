/* popen and pclose: a command's standard output or input as a stream,
 * on a pipe and posix_spawn -- so they work where fork does not.
 *
 * The child must not inherit the pipes of the OTHER popen streams still
 * open, or a reader waiting for one command's end of file waits for
 * every later command too; POSIX says so, and the list of open ones is
 * kept for exactly that. */
#include <stdio.h>
#include <stdlib.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <paths.h>

#include "posix.h"
#include "../internal/lock.h"

struct child { FILE *f; int fd; pid_t pid; struct child *next; };

static struct child *open_children;
static __lock_t children_lock = LOCK_INIT;

/* "r" or "w", and 'e' for close-on-exec as glibc takes it */
WEAK FILE *popen(const char *cmd, const char *mode)
{
    int reading = mode[0] == 'r';
    if ((mode[0] != 'r' && mode[0] != 'w') ||
        (mode[1] && mode[1] != 'e')) {
        errno = EINVAL;
        return NULL;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return NULL;
    int mine = reading ? p[0] : p[1], theirs = reading ? p[1] : p[0];
    struct child *c = malloc(sizeof *c);
    if (!c)
        goto fail;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    int e = posix_spawn_file_actions_adddup2(&fa, theirs, reading ? 1 : 0);
    __lock(&children_lock);
    for (struct child *o = open_children; o && !e; o = o->next)
        if (o->fd != (reading ? 1 : 0))
            e = posix_spawn_file_actions_addclose(&fa, o->fd);
    char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
    extern char **environ;
    pid_t pid = -1;
    if (!e)
        e = posix_spawn(&pid, _PATH_BSHELL, &fa, NULL, argv, environ);
    __unlock(&children_lock);
    posix_spawn_file_actions_destroy(&fa);
    if (e) {
        free(c);
        errno = e;
        goto fail;
    }
    close(theirs);
    if (!(mode[1] == 'e'))
        fcntl(mine, F_SETFD, 0);
    FILE *f = fdopen(mine, reading ? "r" : "w");
    if (!f) {
        close(mine);
        waitpid(pid, NULL, 0);
        free(c);
        return NULL;
    }
    c->f = f;
    c->fd = mine;
    c->pid = pid;
    __lock(&children_lock);
    c->next = open_children;
    open_children = c;
    __unlock(&children_lock);
    return f;
fail:
    close(p[0]);
    close(p[1]);
    return NULL;
}

/* the command's status, as waitpid reports it; -1 for a stream popen
 * did not open */
WEAK int pclose(FILE *f)
{
    __lock(&children_lock);
    struct child **pp = &open_children, *c = NULL;
    while (*pp && (*pp)->f != f)
        pp = &(*pp)->next;
    if (*pp) {
        c = *pp;
        *pp = c->next;
    }
    __unlock(&children_lock);
    if (!c) {
        errno = ECHILD;
        return -1;
    }
    fclose(f);
    int st;
    pid_t r;
    while ((r = waitpid(c->pid, &st, 0)) < 0 && errno == EINTR)
        ;
    free(c);
    return r < 0 ? -1 : st;
}
