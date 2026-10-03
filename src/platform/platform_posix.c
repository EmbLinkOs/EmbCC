/* The console and the program's own path, for POSIX-shaped hosts: macOS,
 * Linux, and EmbLinkOS itself, whose newlib provides <unistd.h>. Files,
 * the environment and the source provider are platform_common.c's, which
 * every host shares. A host without <unistd.h> builds platform_iso.c
 * instead (`make PLATFORM=iso`); a host with more to say gets its own file
 * beside these -- chosen by the build, not an #ifdef in one of them. */
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
#include <unistd.h>            /* isatty, readlink */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

int plat_stderr_is_terminal(void)
{
    return isatty(2);
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
