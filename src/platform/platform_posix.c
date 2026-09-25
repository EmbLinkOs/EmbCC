/* The platform layer for hosts with a C standard library and a POSIX-shaped
 * filesystem: macOS, Linux, and EmbLinkOS itself, whose emlibc provides
 * stdio. This is the only file in `src/` that is allowed to care.
 *
 * A second host means a second file beside this one, chosen by the build —
 * not an #ifdef inside it (§16).
 */
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
    return buf[0] ? buf : NULL;
}

char *plat_read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = xmalloc((size_t)n + 1);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    if (len)
        *len = n;
    return buf;
}

int plat_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int plat_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

const char *plat_getenv(const char *name)
{
    return getenv(name);
}

/* ---- the source provider ---- */

static src_provider_fn g_src_fn;
static void *g_src_ctx;

void src_set_provider(src_provider_fn fn, void *ctx)
{
    g_src_fn = fn;
    g_src_ctx = ctx;
}

char *src_read(const char *path, long *len)
{
    long n = 0;
    char *b = g_src_fn ? g_src_fn(path, &n, g_src_ctx)
                       : plat_read_file(path, &n);
    if (b && len)
        *len = n;
    return b;
}

int src_exists(const char *path)
{
    if (!g_src_fn)
        return plat_file_exists(path);
    long n = 0;
    char *b = g_src_fn(path, &n, g_src_ctx);
    free(b);
    return b != NULL;
}
