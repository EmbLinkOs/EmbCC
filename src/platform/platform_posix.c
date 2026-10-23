/* The console and the program's own path, for POSIX-shaped hosts: macOS,
 * Linux, and EmbLinkOS itself, whose newlib provides <unistd.h>. Files,
 * the environment and the source provider are platform_common.c's, which
 * every host shares. A host without <unistd.h> builds platform_iso.c
 * instead (`make PLATFORM=iso`); a host with more to say gets its own file
 * beside these -- chosen by the build, not an #ifdef in one of them. */
/* POSIX.1-2008's names, asked for: under -std=c99 glibc declares only
 * ISO C's, and readlink was an implicit declaration -- an error to GCC
 * 14 -- on every Linux build. macOS declares them unasked. */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

/* The two hosts answer this differently and neither is portable, which
 * is exactly why it lives down here: Darwin has a libc call, Linux has
 * a symlink in /proc. A host with neither gets NULL, and the driver
 * then relies on what it was told on the command line.
 *
 * __APPLE__ / __linux__ inside THIS file is what the seam is for --
 * the rule is that no file above it may ask. */
/* opendir, for plat_list_dir: macOS's and Linux's. EmbLinkOS builds this
 * file against newlib, whose <dirent.h> is an #error; there a directory
 * cannot be listed, and a PIC32 pack is named with --dfp=. */
#if defined(__APPLE__) || defined(__linux__)
#define PLAT_HAS_DIRENT 1
#include <dirent.h>
#endif
#include <unistd.h>            /* isatty, readlink */
/* threads, for plat_run_on_stack's big stack: macOS's and glibc's (in
 * libc from 2.34, -lpthread before). EmbLinkOS's newlib has none. */
#if defined(__APPLE__) || defined(__linux__)
#define PLAT_HAS_THREADS 1
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

int plat_stderr_is_terminal(void)
{
    return isatty(2);
}

/* the main thread's default on macOS and Linux (ulimit -s), until
 * plat_run_on_stack moves the compile onto a bigger one */
static unsigned long g_stack = 8ul << 20;

unsigned long plat_stack_size(void)
{
    return g_stack;
}

#if PLAT_HAS_THREADS
struct onstack {
    int (*fn)(int, char **);
    int argc;
    char **argv;
    int rc;
};

static void *onstack_run(void *p)
{
    struct onstack *o = p;
    o->rc = o->fn(o->argc, o->argv);
    return NULL;
}
#endif

int plat_run_on_stack(int (*fn)(int, char **), int argc, char **argv)
{
#if PLAT_HAS_THREADS
    /* reserved, not committed: the pages are touched only as deep as the
     * recursion goes, so an ordinary compile costs what it did */
    const unsigned long want = 256ul << 20;
    struct onstack o = { fn, argc, argv, 0 };
    pthread_attr_t a;
    pthread_t t;
    if (pthread_attr_init(&a) == 0) {
        int ok = pthread_attr_setstacksize(&a, (size_t)want) == 0;
        if (ok) {
            g_stack = want;            /* before the thread reads it */
            ok = pthread_create(&t, &a, onstack_run, &o) == 0;
        }
        pthread_attr_destroy(&a);
        if (ok) {
            pthread_join(t, NULL);
            return o.rc;
        }
        g_stack = 8ul << 20;           /* no thread: the main one's */
    }
#endif
    return fn(argc, argv);
}

const char *plat_self_path(void)
{
    static char buf[4096];
    static int tried;
    if (tried)
        return buf[0] ? buf : NULL;
    tried = 1;
#if defined(__APPLE__)
    unsigned size = (unsigned)sizeof buf;
    char raw[4096];
    if (_NSGetExecutablePath(raw, &size) == 0) {
        /* _NSGetExecutablePath may hand back a path with symlinks and
         * `..` still in it; realpath is what makes "the directory this
         * binary is in" a stable answer. */
        if (!realpath(raw, buf))
            snprintf(buf, sizeof buf, "%s", raw);
    }
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0)
        buf[n] = 0;
    else
        buf[0] = 0;
#else
    buf[0] = 0;
#endif
    if (!buf[0]) {
        /* nothing the OS can say: argv[0], when it is a path */
        const char *a = plat_argv0_path();
        if (a)
            snprintf(buf, sizeof buf, "%s", a);
    }
    return buf[0] ? buf : NULL;
}

#ifndef PLAT_HAS_DIRENT
char **plat_list_dir(const char *path)
{
    (void)path;
    return NULL;
}
#else
char **plat_list_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return NULL;
    size_t n = 0, cap = 16;
    char **v = malloc(cap * sizeof *v);
    struct dirent *e;
    while (v && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n + 1 >= cap) {
            char **w = realloc(v, (cap *= 2) * sizeof *v);
            if (!w)
                break;
            v = w;
        }
        v[n] = malloc(strlen(e->d_name) + 1);
        if (!v[n])
            break;
        strcpy(v[n++], e->d_name);
    }
    closedir(d);
    if (v)
        v[n] = NULL;
    return v;
}
#endif
