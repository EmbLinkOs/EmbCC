#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "../platform/platform.h"

/* Kept in step with EMBCC_VERSION in main.c by the golden test rather
 * than by hoping: tests/golden/install.sh compares the directory this
 * builds against the version --version prints. */
#include "version.h"

#define MAXP 1024

static char g_lib[MAXP];      /* the support directory, or "" */
static int  g_kind;           /* 0 unresolved, 1 installed, 2 build tree */
static int  g_done;

/* dir of a path, into `out`. "/a/b/c" -> "/a/b". A path with no slash
 * has no directory to speak of and yields ".". */
static void dirname_of(const char *p, char *out, size_t cap)
{
    const char *slash = strrchr(p, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - p);
    if (n == 0)
        n = 1;                       /* "/x" -> "/" */
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static int has(const char *dir, const char *rel)
{
    char p[MAXP];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    return plat_file_exists(p);
}

static void resolve(void)
{
    if (g_done)
        return;
    g_done = 1;

    /* An explicit override wins, and is how a test can exercise the
     * installed layout without installing. */
    const char *env = plat_getenv("EMBCC_PREFIX");
    if (env && *env) {
        char cand[MAXP];
        snprintf(cand, sizeof cand, "%s/lib/embcc/%s", env, EMBCC_VERSION);
        if (has(cand, "include/stdio.h")) {
            snprintf(g_lib, sizeof g_lib, "%s", cand);
            g_kind = 1;
            return;
        }
        /* Named a prefix that has no EmbCC in it: say so now rather
         * than letting every #include fail one at a time. */
        fprintf(stderr, "embcc: EMBCC_PREFIX=%s has no "
                        "lib/embcc/%s/include\n", env, EMBCC_VERSION);
    }

    const char *self = plat_self_path();
    if (!self)
        return;                      /* host cannot say; no defaults */

    char bin[MAXP], up[MAXP], cand[MAXP];
    dirname_of(self, bin, sizeof bin);

    /* A build tree: the binary sits at the root, beside lib/. Checked
     * FIRST so that working on EmbCC uses the tree being worked on,
     * never a copy installed earlier -- which is the kind of confusion
     * that costs an afternoon. */
    if (has(bin, "lib/libc/include/stdio.h")) {
        snprintf(g_lib, sizeof g_lib, "%s", bin);
        g_kind = 2;
        return;
    }

    /* Installed: <prefix>/bin/embcc, so the prefix is one level up. */
    dirname_of(bin, up, sizeof up);
    snprintf(cand, sizeof cand, "%s/lib/embcc/%s", up, EMBCC_VERSION);
    if (has(cand, "include/stdio.h")) {
        snprintf(g_lib, sizeof g_lib, "%s", cand);
        g_kind = 1;
    }
}

const char *paths_lib_dir(void)
{
    resolve();
    return g_lib[0] ? g_lib : NULL;
}

const char *const *paths_default_includes(int *n)
{
    static char inc[3][MAXP];
    static const char *v[3];
    static int cached = -1;

    if (cached >= 0) {
        *n = cached;
        return cached ? v : NULL;
    }
    resolve();
    cached = 0;
    if (!g_lib[0]) {
        *n = 0;
        return NULL;
    }
    /* Three directories, and the order is load-bearing.
     *
     *   c++          <cstdio> must be found before the C directory is
     *                reached, because it includes <stdio.h> by name.
     *   libc         the HOSTED headers -- <string.h> here declares a
     *                library that exists.
     *   freestanding the headers a compiler must supply itself:
     *                <stddef.h>, <stdarg.h>, <float.h>, <limits.h>.
     *                The libc headers include them, so they have to be
     *                reachable -- and they come LAST because this
     *                directory also holds a declarations-only
     *                <string.h> for freestanding use, which must not
     *                shadow the hosted one. */
    if (g_kind == 1) {
        snprintf(inc[0], sizeof inc[0], "%s/include/c++", g_lib);
        snprintf(inc[1], sizeof inc[1], "%s/include", g_lib);
        snprintf(inc[2], sizeof inc[2], "%s/freestanding", g_lib);
    } else {
        snprintf(inc[0], sizeof inc[0], "%s/lib/libcxx/include", g_lib);
        snprintf(inc[1], sizeof inc[1], "%s/lib/libc/include", g_lib);
        snprintf(inc[2], sizeof inc[2], "%s/include", g_lib);
    }
    for (int i = 0; i < 3; i++)
        v[cached++] = inc[i];
    *n = cached;
    return v;
}

int paths_target_file(const char *triple, const char *name,
                      char *out, size_t cap)
{
    char p[MAXP];
    resolve();
    if (!g_lib[0] || !triple || !name)
        return 0;
    if (g_kind == 1)
        snprintf(p, sizeof p, "%s/%s/%s", g_lib, triple, name);
    else
        /* The build tree keeps these where make put them, under names
         * that predate any install layout. Translating here is what
         * lets the same driver code serve both. */
        snprintf(p, sizeof p, "%s/build/libc/%s/%s", g_lib,
                 strncmp(triple, "x86_64-linux", 12) == 0 ? "linux-x86_64"
                 : strncmp(triple, "aarch64-linux", 13) == 0 ? "linux-aarch64"
                 : strncmp(triple, "aarch64", 7) == 0 ? "aarch64"
                 : "x86_64",
                 name);
    if (!plat_file_exists(p))
        return 0;
    snprintf(out, cap, "%s", p);
    return 1;
}

void paths_print_search_dirs(void)
{
    resolve();
    printf("layout: %s\n", g_kind == 1 ? "installed"
                         : g_kind == 2 ? "build tree"
                         : "none found");
    printf("self: %s\n", plat_self_path() ? plat_self_path() : "(unknown)");
    printf("lib: %s\n", g_lib[0] ? g_lib : "(none)");
    int n = 0;
    const char *const *inc = paths_default_includes(&n);
    for (int i = 0; i < n; i++)
        printf("include: %s\n", inc[i]);
    if (!n)
        printf("include: (none -- every header needs an explicit -I)\n");
}
