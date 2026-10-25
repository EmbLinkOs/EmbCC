#include "paths.h"

#include <stdarg.h>
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
    /* either separator: argv[0] on a DOS-shaped host says `bin\embcc` */
    const char *slash = strrchr(p, '/'), *bs = strrchr(p, '\\');
    if (bs && (!slash || bs > slash))
        slash = bs;
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

/* A path made from fmt into out: 1 when it fits, 0 (and out empty) when
 * it does not. A path cut short names some other file, so a caller treats
 * one that does not fit as not found rather than looking there. */
static int pjoin(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

static int has(const char *dir, const char *rel)
{
    char p[MAXP];
    return pjoin(p, sizeof p, "%s/%s", dir, rel) && plat_file_exists(p);
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
        if (pjoin(cand, sizeof cand, "%s/lib/embcc/%s", env, EMBCC_VERSION) &&
            has(cand, "include/stdio.h") &&
            pjoin(g_lib, sizeof g_lib, "%s", cand)) {
            g_kind = 1;
            return;
        }
        /* Named a prefix that has no EmbCC in it: say so now rather
         * than letting every #include fail one at a time. */
        fprintf(stderr, "embcc: EMBCC_PREFIX=%s has no "
                        "lib/embcc/%s/include\n", env, EMBCC_VERSION);
    }

    const char *self = plat_self_path();
    if (!self) {
        /* The host cannot say where this program is. A compiler built
         * for such a host names its prefix at build time (make
         * DEFAULT_PREFIX=...); without one there are no defaults, and
         * the driver relies on what it was told on the command line. */
#ifdef EMBCC_DEFAULT_PREFIX
        char cand[MAXP];
        if (pjoin(cand, sizeof cand, "%s/lib/embcc/%s", EMBCC_DEFAULT_PREFIX,
                  EMBCC_VERSION) &&
            has(cand, "include/stdio.h") &&
            pjoin(g_lib, sizeof g_lib, "%s", cand))
            g_kind = 1;
#endif
        return;
    }

    char bin[MAXP], up[MAXP], cand[MAXP];
    dirname_of(self, bin, sizeof bin);

    /* A build tree: the binary sits at the root, beside lib/. Checked
     * FIRST so that working on EmbCC uses the tree being worked on,
     * never a copy installed earlier -- which is the kind of confusion
     * that costs an afternoon. */
    if (has(bin, "lib/libc/include/stdio.h") &&
        pjoin(g_lib, sizeof g_lib, "%s", bin)) {
        g_kind = 2;
        return;
    }

    /* Installed: <prefix>/bin/embcc, so the prefix is one level up. */
    dirname_of(bin, up, sizeof up);
    if (pjoin(cand, sizeof cand, "%s/lib/embcc/%s", up, EMBCC_VERSION) &&
        has(cand, "include/stdio.h") &&
        pjoin(g_lib, sizeof g_lib, "%s", cand))
        g_kind = 1;
}

const char *paths_lib_dir(void)
{
    resolve();
    return g_lib[0] ? g_lib : NULL;
}

int paths_is_build_tree(void)
{
    resolve();
    return g_lib[0] && g_kind == 2;
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
    static const char *const sub[2][3] = {
        { "lib/libcxx/include", "lib/libc/include", "include" },   /* tree */
        { "include/c++", "include", "freestanding" },             /* installed */
    };
    for (int i = 0; i < 3; i++)
        if (pjoin(inc[i], sizeof inc[i], "%s/%s", g_lib,
                  sub[g_kind == 1][i]))
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
    if (g_kind == 1) {
        if (!pjoin(p, sizeof p, "%s/%s/%s", g_lib, triple, name))
            return 0;
    } else {
        /* The build tree keeps these where make put them, under names
         * that predate any install layout. Translating here is what
         * lets the same driver code serve both.
         *
         * The C++ runtime lives under build/libcxx rather than
         * build/libc, because it is built by a different rule for a
         * different set of targets. An install puts them side by side,
         * so only this branch has to know. */
        /* Anything this does not recognise is looked for under its OWN
         * name, never under "x86_64". The last arm used to be that
         * fallback, so asking the build tree for AVR's librt.a found
         * x86-64's -- a runtime for the wrong machine, which the linker
         * would reject at best. The embedded runtimes are built into
         * build/libc/<triple>/ by `make rt-embedded` for exactly this. */
        const char *dir =
            strncmp(triple, "x86_64-linux", 12) == 0 ? "linux-x86_64"
            : strncmp(triple, "aarch64-linux", 13) == 0 ? "linux-aarch64"
            : strncmp(triple, "x86_64-apple", 12) == 0 ? "darwin-x86_64"
            : strncmp(triple, "aarch64-apple", 13) == 0 ? "darwin-aarch64"
            : strncmp(triple, "aarch64", 7) == 0 ? "aarch64"
            : strncmp(triple, "x86_64", 6) == 0 ? "x86_64"
            : triple;
        if (!pjoin(p, sizeof p, "%s/build/%s/%s/%s", g_lib,
                   strcmp(name, "libcxx.a") == 0 ? "libcxx" : "libc",
                   dir, name))
            return 0;
    }
    return plat_file_exists(p) && pjoin(out, cap, "%s", p);
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

/* ---- a PIC32's device pack ------------------------------------------- */

/* "1.4.168" against "1.10.2", number by number */
static int pack_version_cmp(const char *a, const char *b)
{
    long x[4] = { 0 }, y[4] = { 0 };
    sscanf(a, "%ld.%ld.%ld.%ld", &x[0], &x[1], &x[2], &x[3]);
    sscanf(b, "%ld.%ld.%ld.%ld", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

/* In a family's directory of versions (.../PIC32MZ-EF_DFP), the newest
 * that has the part's header: into best (and its version into bestv) */
static void pack_versions(const char *fam, const char *hdr, char *best,
                          char *bestv, size_t cap)
{
    char **vs = plat_list_dir(fam);
    for (char **v = vs; v && *v; v++) {
        char dir[MAXP];
        if (!pjoin(dir, sizeof dir, "%s/%s", fam, *v) || !has(dir, hdr))
            continue;
        if (!best[0] || pack_version_cmp(*v, bestv) > 0) {
            snprintf(best, cap, "%s", dir);
            snprintf(bestv, 64, "%s", *v);
        }
    }
    plat_free_list(vs);
}

/* A packs root (~/.mchp_packs, an MPLAB X packs folder): every vendor's
 * *_DFP families */
static void pack_root(const char *root, const char *hdr, char *best,
                      char *bestv, size_t cap)
{
    char **vendors = plat_list_dir(root);
    for (char **vd = vendors; vd && *vd; vd++) {
        char vdir[MAXP];
        if (!pjoin(vdir, sizeof vdir, "%s/%s", root, *vd))
            continue;
        char **fams = plat_list_dir(vdir);
        for (char **f = fams; f && *f; f++) {
            size_t n = strlen(*f);
            char fdir[MAXP];
            if (n > 4 && strcmp(*f + n - 4, "_DFP") == 0 &&
                pjoin(fdir, sizeof fdir, "%s/%s", vdir, *f))
                pack_versions(fdir, hdr, best, bestv, cap);
        }
        plat_free_list(fams);
    }
    plat_free_list(vendors);
}

/* Each MPLAB X installed under base (base/<version>/packs) */
static void pack_mplabx(const char *base, const char *hdr, char *best,
                        char *bestv, size_t cap)
{
    char **vs = plat_list_dir(base);
    for (char **v = vs; v && *v; v++) {
        char packs[MAXP];
        if (pjoin(packs, sizeof packs, "%s/%s/packs", base, *v))
            pack_root(packs, hdr, best, bestv, cap);
    }
    plat_free_list(vs);
}

int paths_pic32_dfp(const char *part, const char *named, char *out,
                    size_t cap)
{
    char hdr[96], bestv[64] = "";
    size_t k = 0;
    int n = snprintf(hdr, sizeof hdr, "include/proc/p");
    for (; part[k] && n + 3 < (int)sizeof hdr; k++)
        hdr[n++] = (char)(part[k] >= 'A' && part[k] <= 'Z' ? part[k] + 32
                                                           : part[k]);
    snprintf(hdr + n, sizeof hdr - (size_t)n, ".h");
    out[0] = 0;
    if (named) {
        if (has(named, hdr)) {
            snprintf(out, cap, "%s", named);
            return 1;
        }
        pack_versions(named, hdr, out, bestv, cap);
        return out[0] != 0;
    }
    const char *home = plat_getenv("HOME");
    char root[MAXP];
    if (home && pjoin(root, sizeof root, "%s/.mchp_packs", home))
        pack_root(root, hdr, out, bestv, cap);
    pack_mplabx("/Applications/microchip/mplabx", hdr, out, bestv, cap);
    pack_mplabx("/opt/microchip/mplabx", hdr, out, bestv, cap);
    pack_mplabx("C:/Program Files/Microchip/MPLABX", hdr, out, bestv, cap);
    return out[0] != 0;
}
