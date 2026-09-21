/* embidx — the cross-TU half of the project knowledge graph (vision §8.2).
 *
 * `embcc --emit-interfaces` already answers, for ONE unit: what it was
 * derived from, what it provides, what it observes of elsewhere. Nothing
 * stored those across units or across builds, so there was no persistent
 * index and no invalidation graph. This is that store, and the four
 * questions it exists to answer.
 *
 * ---- why -MD is not enough ----------------------------------------------
 *
 * `-MD` says a unit depends on a header. Add a comment to that header and
 * every unit that includes it rebuilds. On a tree the size of an operating
 * system that is most of the rebuilds, and it is why a one-character edit
 * to a widely included header costs minutes.
 *
 * Two different hashes are needed to do better, and the gap between them
 * is the entire benefit:
 *
 *   a FILE hash      says "the text changed" — decides whether a unit must
 *                    be RE-EXAMINED
 *   an INTERFACE hash says "what dependents observe changed" — decides
 *                    whether it must be REBUILT
 *
 * A comment moves the first and not the second. So `stale` re-examines
 * what changed on disk, and reports for rebuilding only those units whose
 * own source changed or whose observed interfaces actually differ.
 *
 * ---- what this tool is NOT ----------------------------------------------
 *
 * It is not a build system and does not run the compiler to produce
 * objects. It runs `embcc --emit-interfaces`, which is cheap (no codegen)
 * and is the same output a build system would consume directly. The index
 * is a cache of that, and §8.2 requires it be safely discardable: delete
 * it and the next `build` reconstructs it from the sources exactly.
 *
 * ---- the format ----------------------------------------------------------
 *
 * Line-oriented, sorted, versioned text, as build.ebm is. Deterministic
 * (R4): the same sources give the same bytes, so two builds' indexes can
 * be compared with diff. A binary format would be smaller and would make
 * every question about it need this program.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define IDX_MAGIC "; EmbCC index v1"

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("embidx: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---- the model ---------------------------------------------------------- */

struct fact {
    char *key;                   /* a file path, or a USR */
    char *hash;                  /* 16 hex digits, as text: never compared
                                  * as a number, only for equality */
};

struct unit {
    char *path;
    char *args;                  /* the flags it was indexed with */
    struct fact *files; int nfiles;
    struct fact *prov;  int nprov;
    struct fact *uses;  int nuses;
};

struct index {
    struct unit *u; int n, cap;
};

static void fact_add(struct fact **v, int *n, const char *k, const char *h)
{
    *v = xrealloc(*v, (size_t)(*n + 1) * sizeof **v);
    (*v)[*n].key = xstrdup(k);
    (*v)[*n].hash = xstrdup(h);
    (*n)++;
}

static const char *fact_find(const struct fact *v, int n, const char *k)
{
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].key, k) == 0)
            return v[i].hash;
    return NULL;
}

static int fact_cmp(const void *a, const void *b)
{
    return strcmp(((const struct fact *)a)->key,
                  ((const struct fact *)b)->key);
}

static int unit_cmp(const void *a, const void *b)
{
    return strcmp(((const struct unit *)a)->path,
                  ((const struct unit *)b)->path);
}

static struct unit *idx_add(struct index *ix, const char *path)
{
    if (ix->n == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 16;
        ix->u = xrealloc(ix->u, (size_t)ix->cap * sizeof *ix->u);
    }
    struct unit *u = &ix->u[ix->n++];
    memset(u, 0, sizeof *u);
    u->path = xstrdup(path);
    return u;
}

/* ---- running the compiler ----------------------------------------------- */

/* embcc's own output, read back. Running the real thing rather than
 * linking its front end keeps this program independent of the compiler's
 * internals: the text is the contract, and it is the same text a build
 * system consumes. */
static char *run_iface(const char *embcc, const char *args, const char *unit)
{
    struct { char *p; size_t n, cap; } b = { NULL, 0, 0 };
    char *cmd = xmalloc(strlen(embcc) + strlen(args) + strlen(unit) + 64);
    sprintf(cmd, "%s --emit-interfaces %s %s 2>/dev/null", embcc, args, unit);
    FILE *f = popen(cmd, "r");
    free(cmd);
    if (!f)
        return NULL;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (b.n + l + 1 > b.cap) {
            b.cap = (b.n + l + 1) * 2;
            b.p = xrealloc(b.p, b.cap);
        }
        memcpy(b.p + b.n, line, l + 1);
        b.n += l;
    }
    int rc = pclose(f);
    if (rc != 0) {
        free(b.p);
        return NULL;
    }
    return b.p ? b.p : xstrdup("");
}

/* Fill a unit from one --emit-interfaces output. */
static void unit_load(struct unit *u, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[8192];
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = e ? e + 1 : p + strlen(p);
        if (line[0] == ';' || !line[0])
            continue;
        char kind[16], hash[64], key[4096];
        if (sscanf(line, "%15s %63s %4095s", kind, hash, key) == 3 &&
            strcmp(kind, "file") == 0) {
            fact_add(&u->files, &u->nfiles, key, hash);
        } else if (sscanf(line, "%15s %4095s %63s", kind, key, hash) == 3) {
            if (strcmp(kind, "provides") == 0)
                fact_add(&u->prov, &u->nprov, key, hash);
            else if (strcmp(kind, "uses") == 0)
                fact_add(&u->uses, &u->nuses, key, hash);
        }
    }
    qsort(u->files, (size_t)u->nfiles, sizeof *u->files, fact_cmp);
    qsort(u->prov, (size_t)u->nprov, sizeof *u->prov, fact_cmp);
    qsort(u->uses, (size_t)u->nuses, sizeof *u->uses, fact_cmp);
}

/* ---- the file format ---------------------------------------------------- */

static void idx_write(const struct index *ix, const char *path)
{
    FILE *f = path ? fopen(path, "w") : stdout;
    if (!f)
        die("cannot write %s", path);
    fputs(IDX_MAGIC "\n", f);
    fputs("; a persistent cross-TU index (vision 8.2). Discard it freely:\n"
          "; everything here is rebuilt from the sources by 'embidx build'.\n",
          f);
    for (int i = 0; i < ix->n; i++) {
        const struct unit *u = &ix->u[i];
        fprintf(f, "unit %s\n", u->path);
        fprintf(f, "  args %s\n", u->args ? u->args : "");
        for (int k = 0; k < u->nfiles; k++)
            fprintf(f, "  file %s %s\n", u->files[k].hash, u->files[k].key);
        for (int k = 0; k < u->nprov; k++)
            fprintf(f, "  provides %s %s\n", u->prov[k].hash, u->prov[k].key);
        for (int k = 0; k < u->nuses; k++)
            fprintf(f, "  uses %s %s\n", u->uses[k].hash, u->uses[k].key);
    }
    if (path)
        fclose(f);
}

static void idx_read(struct index *ix, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot read %s (build it with 'embidx build')", path);
    char line[8192];
    if (!fgets(line, sizeof line, f) || strncmp(line, IDX_MAGIC, strlen(IDX_MAGIC)))
        die("%s is not an EmbCC index, or is a version this does not know",
            path);
    struct unit *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (line[0] == ';' || !line[0])
            continue;
        if (strncmp(line, "unit ", 5) == 0) {
            cur = idx_add(ix, line + 5);
            continue;
        }
        if (!cur)
            die("%s: a fact before any unit", path);
        char *q = line;
        while (*q == ' ')
            q++;
        if (strncmp(q, "args ", 5) == 0) {
            cur->args = xstrdup(q + 5);
        } else if (strcmp(q, "args") == 0) {
            cur->args = xstrdup("");
        } else {
            char kind[16], hash[64], key[4096];
            if (sscanf(q, "%15s %63s %4095s", kind, hash, key) != 3)
                continue;
            if (strcmp(kind, "file") == 0)
                fact_add(&cur->files, &cur->nfiles, key, hash);
            else if (strcmp(kind, "provides") == 0)
                fact_add(&cur->prov, &cur->nprov, key, hash);
            else if (strcmp(kind, "uses") == 0)
                fact_add(&cur->uses, &cur->nuses, key, hash);
        }
    }
    fclose(f);
}

/* ---- commands ----------------------------------------------------------- */

static const char *embcc_path(void)
{
    const char *e = getenv("EMBCC");
    return e && *e ? e : "./embcc";
}

static int cmd_build(int argc, char **argv)
{
    const char *outp = NULL;
    char args[8192] = "";
    char *units[4096];
    int nunits = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outp = argv[++i];
        } else if (argv[i][0] == '-') {
            if (strlen(args) + strlen(argv[i]) + 2 >= sizeof args)
                die("too many compiler flags");
            strcat(args, argv[i]);
            strcat(args, " ");
        } else {
            if (nunits == 4096)
                die("too many units");
            units[nunits++] = argv[i];
        }
    }
    if (!nunits)
        die("no units to index");
    const char *cc = embcc_path();
    struct index ix = { NULL, 0, 0 };
    int failed = 0;
    for (int i = 0; i < nunits; i++) {
        char *text = run_iface(cc, args, units[i]);
        if (!text) {
            fprintf(stderr, "embidx: %s: could not be read (skipped)\n",
                    units[i]);
            failed++;
            continue;
        }
        struct unit *u = idx_add(&ix, units[i]);
        u->args = xstrdup(args);
        unit_load(u, text);
        free(text);
    }
    qsort(ix.u, (size_t)ix.n, sizeof *ix.u, unit_cmp);
    idx_write(&ix, outp);
    if (outp)
        printf("indexed %d unit%s into %s%s\n", ix.n, ix.n == 1 ? "" : "s",
               outp, failed ? " (some could not be read)" : "");
    return failed ? 1 : 0;
}

/* Which units must be REBUILT, and why. */
static int cmd_stale(int argc, char **argv)
{
    if (argc < 1)
        die("usage: embidx stale <index>");
    struct index ix = { NULL, 0, 0 };
    idx_read(&ix, argv[0]);
    const char *cc = embcc_path();
    int n_re = 0, n_rebuild = 0;

    for (int i = 0; i < ix.n; i++) {
        struct unit *u = &ix.u[i];
        /* Step one: did anything it was derived from change on disk?
         * This is the cheap question and it is asked first, because a
         * unit whose inputs are all unchanged cannot need anything. */
        int touched = 0;
        const char *own = NULL;
        char *newtext = NULL;
        for (int k = 0; k < u->nfiles; k++) {
            if (strcmp(u->files[k].key, u->path) == 0)
                own = u->files[k].hash;
        }
        struct unit fresh;
        memset(&fresh, 0, sizeof fresh);
        newtext = run_iface(cc, u->args ? u->args : "", u->path);
        if (!newtext) {
            printf("%s: rebuild (it no longer compiles, or is gone)\n",
                   u->path);
            n_rebuild++;
            continue;
        }
        unit_load(&fresh, newtext);
        free(newtext);
        for (int k = 0; k < fresh.nfiles; k++) {
            const char *old = fact_find(u->files, u->nfiles, fresh.files[k].key);
            if (!old || strcmp(old, fresh.files[k].hash) != 0) {
                touched = 1;
                break;
            }
        }
        if (!touched && fresh.nfiles != u->nfiles)
            touched = 1;
        if (!touched)
            continue;
        n_re++;

        /* Step two: of the units whose text moved, which actually have to
         * be rebuilt? Its own source changing is enough -- a body edit
         * changes no hash and still must be compiled. Otherwise only a
         * DIFFERENT observed interface counts, and a header comment
         * produces none. */
        const char *nown = fact_find(fresh.files, fresh.nfiles, u->path);
        int own_changed = !own || !nown || strcmp(own, nown) != 0;
        int iface_changed = fresh.nuses != u->nuses || fresh.nprov != u->nprov;
        for (int k = 0; !iface_changed && k < fresh.nuses; k++) {
            const char *old = fact_find(u->uses, u->nuses, fresh.uses[k].key);
            if (!old || strcmp(old, fresh.uses[k].hash) != 0)
                iface_changed = 1;
        }
        for (int k = 0; !iface_changed && k < fresh.nprov; k++) {
            const char *old = fact_find(u->prov, u->nprov, fresh.prov[k].key);
            if (!old || strcmp(old, fresh.prov[k].hash) != 0)
                iface_changed = 1;
        }
        if (own_changed || iface_changed) {
            printf("%s: rebuild (%s)\n", u->path,
                   own_changed ? "its own source changed"
                               : "an interface it observes changed");
            n_rebuild++;
        }
    }
    fprintf(stderr, "%d unit%s re-examined, %d need rebuilding\n",
            n_re, n_re == 1 ? "" : "s", n_rebuild);
    return 0;
}

/* Cross-TU consistency: the questions only a project-wide index can ask. */
static int cmd_check(int argc, char **argv)
{
    if (argc < 1)
        die("usage: embidx check <index>");
    struct index ix = { NULL, 0, 0 };
    idx_read(&ix, argv[0]);
    int problems = 0;

    /* Two units that disagree about what a declaration IS. Each compiled
     * against a different view of the same entity -- a header included
     * under different -D flags, or two definitions that drifted -- and
     * the link will succeed and the program will not work. No single
     * translation unit can see this; that is the point of an index. */
    for (int i = 0; i < ix.n; i++) {
        for (int k = 0; k < ix.u[i].nuses; k++) {
            const char *usr = ix.u[i].uses[k].key;
            const char *h = ix.u[i].uses[k].hash;
            for (int j = i + 1; j < ix.n; j++) {
                const char *h2 = fact_find(ix.u[j].uses, ix.u[j].nuses, usr);
                if (h2 && strcmp(h, h2) != 0) {
                    printf("conflict %s\n  %s sees %s\n  %s sees %s\n",
                           usr, ix.u[i].path, h, ix.u[j].path, h2);
                    problems++;
                }
                const char *hp = fact_find(ix.u[j].prov, ix.u[j].nprov, usr);
                if (hp && strcmp(h, hp) != 0) {
                    printf("conflict %s\n  %s uses     %s\n"
                           "  %s provides %s\n",
                           usr, ix.u[i].path, h, ix.u[j].path, hp);
                    problems++;
                }
            }
        }
    }
    /* Defined in two places: the link fails, but it fails with a symbol
     * name and no source position. This says where both are. */
    for (int i = 0; i < ix.n; i++)
        for (int k = 0; k < ix.u[i].nprov; k++)
            for (int j = i + 1; j < ix.n; j++)
                if (fact_find(ix.u[j].prov, ix.u[j].nprov, ix.u[i].prov[k].key)) {
                    printf("defined twice %s\n  %s\n  %s\n",
                           ix.u[i].prov[k].key, ix.u[i].path, ix.u[j].path);
                    problems++;
                }
    /* Named by some unit and defined by none: either it comes from
     * outside (a library) or it is a link error waiting to happen. Said
     * as a note rather than a problem, because an index need not be
     * whole.
     *
     * Only functions and variables are counted. A type is never
     * "provided" by a unit -- it has no linkage and no definition to
     * link against -- so counting its uses here would report every
     * struct in every header as dangling, which is noise that makes the
     * real number unreadable. */
    int dangling = 0;
    for (int i = 0; i < ix.n; i++)
        for (int k = 0; k < ix.u[i].nuses; k++) {
            const char *usr = ix.u[i].uses[k].key;
            if (!strstr(usr, "@F@") && !strstr(usr, "@V@"))
                continue;
            int found = 0;
            for (int j = 0; j < ix.n && !found; j++)
                found = fact_find(ix.u[j].prov, ix.u[j].nprov, usr) != NULL;
            if (!found)
                dangling++;
        }
    printf("%d unit%s, %d problem%s, %d call%s or reference%s to outside "
           "the index\n",
           ix.n, ix.n == 1 ? "" : "s", problems, problems == 1 ? "" : "s",
           dangling, dangling == 1 ? "" : "s", dangling == 1 ? "" : "s");
    return problems ? 1 : 0;
}

/* Where a declaration is defined and who observes it. */
static int cmd_who(int argc, char **argv)
{
    if (argc < 2)
        die("usage: embidx who <usr> <index>");
    const char *usr = argv[0];
    struct index ix = { NULL, 0, 0 };
    idx_read(&ix, argv[1]);
    int n = 0;
    for (int i = 0; i < ix.n; i++) {
        const char *h = fact_find(ix.u[i].prov, ix.u[i].nprov, usr);
        if (h) { printf("provides %s %s\n", ix.u[i].path, h); n++; }
    }
    for (int i = 0; i < ix.n; i++) {
        const char *h = fact_find(ix.u[i].uses, ix.u[i].nuses, usr);
        if (h) { printf("uses     %s %s\n", ix.u[i].path, h); n++; }
    }
    if (!n) {
        printf("no unit in the index mentions %s\n", usr);
        return 1;
    }
    return 0;
}

static void usage(void)
{
    fputs("embidx — the project's cross-TU index (vision 8.2)\n"
          "\n"
          "  embidx build -o FILE [flags] unit.c...   build or rebuild it\n"
          "  embidx stale FILE                        what must be rebuilt\n"
          "  embidx check FILE                        cross-TU consistency\n"
          "  embidx who USR FILE                      who defines, who uses\n"
          "\n"
          "The compiler is $EMBCC, or ./embcc. The index is a cache: delete\n"
          "it and 'build' reconstructs it exactly from the sources.\n",
          stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "build") == 0)
        return cmd_build(argc - 2, argv + 2);
    if (strcmp(argv[1], "stale") == 0)
        return cmd_stale(argc - 2, argv + 2);
    if (strcmp(argv[1], "check") == 0)
        return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "who") == 0)
        return cmd_who(argc - 2, argv + 2);
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    usage();
    return 2;
}
