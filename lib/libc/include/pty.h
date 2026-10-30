/* <pty.h> -- the BSD pseudo-terminal pair (src/posix/pty.c) */
#ifndef _PTY_H
#define _PTY_H

#include <termios.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

int openpty(int *master, int *slave, char *name, const struct termios *t,
            const struct winsize *ws);
pid_t forkpty(int *master, char *name, const struct termios *t,
              const struct winsize *ws);
int login_tty(int fd);

#ifdef __cplusplus
}
#endif

#endif
