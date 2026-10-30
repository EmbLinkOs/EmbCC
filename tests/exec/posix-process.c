/* POSIX's processes: fork and the wait family, the exec family's PATH
 * search, posix_spawn with file actions and posix_spawnp's script rule,
 * system and popen, and the ids.
 *
 * The program is its own child and its own shell: run as "sh -c CMD" it
 * does what the few commands below ask, so tests/golden/linux-libc-exec.sh
 * need only copy it to /bin/sh in the guest. glibc 2.36 gave the same
 * results on the same kernel. Where there is no kernel to start a
 * process (the boards), each call fails with ENOSYS. */
// expect-exit: 42
// needs-libc: POSIX's process calls, on lib/libc's backends
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* "exit N", "echo WORDS", "cat" -- the shell this test needs */
static int as_shell(const char *cmd)
{
    if (!strncmp(cmd, "exit ", 5))
        return atoi(cmd + 5);
    if (!strncmp(cmd, "echo ", 5)) {
        printf("%s\n", cmd + 5);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(cmd, "cat")) {
        char b[64];
        long n;
        while ((n = read(0, b, sizeof b)) > 0)
            write(1, b, (size_t)n);
        return 0;
    }
    return 127;
}

#ifdef EMBCC_TEST_HAS_FS
static int run(void)
{
    int st;
    fflush(stdout);
    pid_t c = fork();
    if (c == 0)
        _exit(23);
    if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 23)
        return 1;
    c = fork();
    if (c == 0) {
        pause();
        _exit(0);
    }
    kill(c, SIGTERM);
    if (waitpid(c, &st, 0) != c || !WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM)
        return 2;
    errno = 0;
    if (wait(&st) != -1 || errno != ECHILD)
        return 3;
    c = fork();
    if (c == 0) {
        usleep(200000);
        _exit(4);
    }
    if (waitpid(c, &st, WNOHANG) != 0 || waitpid(c, &st, 0) != c ||
        WEXITSTATUS(st) != 4)
        return 4;

    char *av[] = { "sh", "-c", "exit 9", NULL };
    pid_t sp;
    if (posix_spawn(&sp, "/bin/sh", NULL, NULL, av, environ) ||
        waitpid(sp, &st, 0) != sp || WEXITSTATUS(st) != 9)
        return 5;
    /* a program that is not there is posix_spawn's error, not a child's
     * exit status of 127 */
    if (posix_spawn(&sp, "/nonexistent", NULL, NULL, av, environ) != ENOENT)
        return 6;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/tmp/out",
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *av2[] = { "sh", "-c", "echo spawned output", NULL };
    setenv("PATH", "/bin", 1);
    if (posix_spawnp(&sp, "sh", &fa, NULL, av2, environ) ||
        waitpid(sp, &st, 0) != sp)
        return 7;
    posix_spawn_file_actions_destroy(&fa);
    char b[64] = { 0 };
    FILE *f = fopen("/tmp/out", "r");
    if (!f || !fgets(b, sizeof b, f) || strcmp(b, "spawned output\n"))
        return 8;
    fclose(f);

    /* a script with no binary format: posix_spawnp runs /bin/sh on it */
    f = fopen("/bin/script7", "w");
    fputs("exit 7\n", f);
    fclose(f);
    chmod("/bin/script7", 0755);
    char *av3[] = { "script7", NULL };
    if (posix_spawnp(&sp, "script7", NULL, NULL, av3, environ) ||
        waitpid(sp, &st, 0) != sp || WEXITSTATUS(st) != 7)
        return 9;

    if (WEXITSTATUS(system("exit 3")) != 3 || system(NULL) != 1)
        return 10;
    f = popen("echo from popen", "r");
    memset(b, 0, sizeof b);
    if (!f || !fgets(b, sizeof b, f) || strcmp(b, "from popen\n") ||
        WEXITSTATUS(pclose(f)) != 0)
        return 11;
    f = popen("cat", "w");
    if (!f || fputs("to cat\n", f) < 0 || pclose(f) != 0)
        return 12;

    fflush(stdout);
    c = fork();
    if (c == 0) {
        char *ev[] = { "sh", "-c", "exit 11", NULL };
        execvp("sh", ev);
        _exit(99);
    }
    if (waitpid(c, &st, 0) != c || WEXITSTATUS(st) != 11)
        return 13;
    errno = 0;
    char *none[] = { "nosuchprogram", NULL };
    if (execvp("nosuchprogram", none) != -1 || errno != ENOENT)
        return 14;
    if (getpid() <= 0 || getppid() < 0)
        return 15;
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-c"))
        return as_shell(argv[2]);
    /* argv[0] is the script's path when run as a script's interpreter */
    if (argc == 2 && strstr(argv[1], "script")) {
        char line[64] = "";
        FILE *f = fopen(argv[1], "r");
        if (f && fgets(line, sizeof line, f))
            line[strcspn(line, "\n")] = 0;
        return as_shell(line);
    }
#ifdef EMBCC_TEST_HAS_FS
    int r = run();
    if (r)
        return r;
#else
    char *av[] = { "x", NULL };
    pid_t pid;
    errno = 0;
    if (fork() != -1 || errno != ENOSYS)
        return 20;
    if (posix_spawn(&pid, "/bin/x", NULL, NULL, av, environ) != ENOSYS)
        return 21;
#endif
    return 42;
}
