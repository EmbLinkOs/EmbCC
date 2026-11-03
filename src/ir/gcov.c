/* --coverage, -fprofile-arcs, -ftest-coverage, -fprofile-generate and
 * -fprofile-use: GCC's gcov, in GCC 16's files, so that gcov itself,
 * llvm-cov gcov, lcov and gcovr read what EmbCC writes.
 *
 * The model is GCC's. Each function becomes a graph of gcov blocks:
 * block 0 is the entry, block 1 the exit, and the code's basic blocks
 * follow -- split after every call as well as at every label and
 * branch, because a call need not return (exit, longjmp, a throw), and
 * a block is the unit a line count is told in. Every edge is an arc;
 * a call's block has a FAKE arc to the exit besides its fall-through,
 * which stands for leaving the function from the call.
 *
 * Not every arc needs a counter. Flow is conserved at every block --
 * what enters leaves -- and gcov closes the graph with an arc from the
 * exit back to the entry, so the arcs of a spanning tree can be solved
 * from the others. The tree is chosen here and its arcs are marked
 * ON_TREE in the .gcno; gcov solves them, and so does -fprofile-use
 * (solve_tree below). The fake arcs are always on it: they are the one
 * kind of arc no code runs along, so they could not be counted. Where
 * there is a choice the tree takes the arcs that would be dear to count
 * -- an edge from a branch to a block with other predecessors, which
 * needs a block of its own -- and then back edges, so a loop's counter
 * is on its exit rather than on every trip.
 *
 * A counted arc is a 64-bit increment, placed where only that arc
 * runs: at the end of its source when the source has no other
 * successor, at the start of its destination when that has no other
 * predecessor, after a conditional branch for its fall-through, and
 * otherwise in a block of its own at the function's end that the
 * branch (or the switch's table) is pointed at. Two kinds of entry
 * run through no arc of the code, and are modelled as arcs from the
 * entry: a landing pad, entered by the unwinder (counted after its
 * IR_LANDING), and -- in a function with a computed goto -- a label,
 * which `goto *p` could reach from anywhere. There, every jump to a
 * label is an arc into a block of its own in front of the label (the
 * label's "head"), the computed gotos all arc to one dispatch block,
 * the dispatch block arcs to every head on the tree, and each head's
 * arc into its label is counted at the label.
 *
 * The counters of a unit are one array, __embcc_gcov_ctrs, and what
 * the runtime needs to write the .gcda -- its path, the stamp, each
 * function's ident, checksums and counters -- one record beside it,
 * __embcc_gcov_info. Both are declared by the few lines of C the driver
 * appends to an instrumented unit (gcov_ctor_source), with a
 * constructor that hands the record to lib/gcov's __embcc_gcov_init;
 * their sizes and contents are only known after every function has
 * been instrumented, so gcov_unit_end gives them those. */

#include "ir.h"
#include "irgen_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../driver/util.h"
#include "../sema/sema.h"
#include "../sema/type.h"
#include "../arch/target.h"
#include "../platform/platform.h"

#define GCOV_NOTE_MAGIC 0x67636e6fu      /* "gcno" */
#define GCOV_DATA_MAGIC 0x67636461u      /* "gcda" */
#define GCOV_VERSION 0x4236322au         /* "B62*": GCC 16.2's */
/* The version written, and so the format: from GCC 12 on ("B2..*"), a
 * record's length and a string's are counted in bytes, a string is not
 * padded, and the header has a checksum word; before, in words, padded,
 * and none. gcov reads only its own generation's, and llvm-cov (as of
 * LLVM 23) only the older -- clang writes "B11*" -- so
 * --coverage-version= says which: gcc (the default), llvm, or a version
 * as GCC spells it. */
static unsigned g_version = GCOV_VERSION;
static int g_bytes = 1;                  /* lengths in bytes (GCC 12 on) */

static int version_number(unsigned v)
{
    int c0 = (int)(v >> 24) & 0xff, c1 = (int)(v >> 16) & 0xff,
        c2 = (int)(v >> 8) & 0xff;
    return c0 >= 'A' ? (c0 - 'A') * 100 + (c1 - '0') * 10 + (c2 - '0')
                     : (c0 - '0') * 10 + (c2 - '0');
}

int gcov_set_version(const char *v)
{
    if (!strcmp(v, "gcc"))
        v = "B62*";
    else if (!strcmp(v, "llvm") || !strcmp(v, "clang"))
        v = "B11*";
    if (strlen(v) != 4 || (!(v[0] >= '3' && v[0] <= '9') &&
                           !(v[0] >= 'A' && v[0] <= 'Z')))
        return -1;
    g_version = (unsigned)(unsigned char)v[0] << 24 |
                (unsigned)(unsigned char)v[1] << 16 |
                (unsigned)(unsigned char)v[2] << 8 | (unsigned char)v[3];
    int n = version_number(g_version);
    if (n < 90)
        return -1;                     /* before GCC 9: another layout */
    g_bytes = n >= 120;
    return 0;
}
#define TAG_FUNCTION 0x01000000u
#define TAG_BLOCKS 0x01410000u
#define TAG_ARCS 0x01430000u
#define TAG_LINES 0x01450000u
#define TAG_COUNTER_ARCS 0x01a10000u
#define TAG_OBJECT_SUMMARY 0xa1000000u
#define ARC_ON_TREE 1
#define ARC_FAKE 2
#define ARC_FALLTHROUGH 4
#define INFO_MAGIC 0x45474356u           /* "EGCV": lib/gcov's record */

static int g_arcs, g_notes, g_atomic;
static const char *g_filter, *g_exclude;
static const char *g_use;                /* -fprofile-use: the .gcda */

/* the unit being compiled */
static const char *g_src, *g_gcda;
static unsigned g_stamp;
static struct global *g_ctrs, *g_info;
static long g_nctrs;
static struct outbuf g_nb;               /* the .gcno's function records */
struct gfn { unsigned ident, lineno, cfg; long base, n; };
static struct gfn *g_fns;
static int g_nfns, g_capfns;

/* -fprofile-use: the profile read for this unit */
struct pfn { unsigned ident, lineno, cfg; long n; unsigned long long *c; };
static struct pfn *g_prof;
static int g_nprof, g_have_prof;
static unsigned long long g_prof_max;    /* the summary's sum_max */

void gcov_set(int arcs, int notes, int atomic, const char *filter,
              const char *exclude)
{
    g_arcs = arcs;
    g_notes = notes;
    g_atomic = atomic;
    g_filter = filter;
    g_exclude = exclude;
}

void gcov_set_use(const char *gcda) { g_use = gcda; }

/* -fprofile-partial-training: a function the profile never ran is
 * optimized as without one, not as cold; -fprofile-abs-path: the notes
 * name each source by its absolute path */
static int g_partial, g_abs;
void gcov_set_flags(int partial, int abs)
{
    g_partial = partial;
    g_abs = abs;
}

static const char *note_file(const char *file)
{
    if (!g_abs || !file || file[0] == '/' || file[0] == '<' || !plat_cwd())
        return file;
    size_t n = strlen(plat_cwd()) + strlen(file) + 2;
    char *r = xmalloc(n);
    snprintf(r, n, "%s/%s", plat_cwd(), file);
    return r;                    /* lives as long as the notes */
}
int gcov_arcs(void) { return g_arcs; }
int gcov_active(void) { return g_arcs || g_notes || g_use; }

/* ---- small things -------------------------------------------------- */

static unsigned crc32_add(unsigned c, const void *p, size_t n)
{
    const unsigned char *b = p;
    c = ~c;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static unsigned crc32_u32(unsigned c, unsigned v)
{
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16),
                           (unsigned char)(v >> 24) };
    return crc32_add(c, b, 4);
}

unsigned gcov_crc32(unsigned c, const void *p, unsigned long n)
{
    return crc32_add(c, p, n);
}

static void put32(struct outbuf *b, unsigned v)
{
    char w[4] = { (char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) };
    ob_add(b, w, 4);
}

static void put_str(struct outbuf *b, const char *s)
{
    size_t n = s ? strlen(s) + 1 : 0;
    if (!g_bytes) {
        /* in words, padded with NULs; an empty string is no words */
        size_t w = s ? (strlen(s) + 4) / 4 : 0;
        put32(b, (unsigned)w);
        if (w) {
            ob_add(b, s, strlen(s));
            for (size_t k = strlen(s); k < 4 * w; k++)
                ob_ch(b, 0);
        }
        return;
    }
    put32(b, (unsigned)n);
    if (n)
        ob_add(b, s, n);
}

static void patch32(struct outbuf *b, size_t at, unsigned v)
{
    b->p[at] = (char)v;
    b->p[at + 1] = (char)(v >> 8);
    b->p[at + 2] = (char)(v >> 16);
    b->p[at + 3] = (char)(v >> 24);
}

/* A record: its tag and a length patched once its payload is written
 * (GCC 12 on count it in bytes). Returns where the length is. */
static size_t rec_open(struct outbuf *b, unsigned tag)
{
    put32(b, tag);
    size_t at = b->n;
    put32(b, 0);
    return at;
}

static void rec_close(struct outbuf *b, size_t at)
{
    size_t n = b->n - at - 4;
    patch32(b, at, (unsigned)(g_bytes ? n : n / 4));
}

/* GCC's -fprofile-filter-files / -fprofile-exclude-files: `;`-separated
 * regular expressions, searched for in the file name. This is the
 * subset they are written in -- literals, `.`, `*`, `+`, `?`, `^`, `$`,
 * brackets and backslash escapes. */
static int re_here(const char *re, const char *re_end, const char *s);

static int re_class(const char **rp, const char *re_end, char c, int *ok)
{
    const char *r = *rp + 1;
    int neg = 0, hit = 0;
    if (r < re_end && *r == '^') { neg = 1; r++; }
    int first = 1;
    while (r < re_end && (*r != ']' || first)) {
        first = 0;
        char lo = *r, hi = lo;
        if (r + 2 < re_end && r[1] == '-' && r[2] != ']') { hi = r[2]; r += 2; }
        if (c >= lo && c <= hi) hit = 1;
        r++;
    }
    *ok = r < re_end;
    *rp = r < re_end ? r + 1 : r;
    return hit != neg;
}

/* one atom of re at s: the atom's length in *alen, matches or not */
static int re_atom(const char *re, const char *re_end, const char *s,
                   int *alen)
{
    const char *r = re;
    int ok = 1, m;
    if (*r == '[') {
        m = *s && re_class(&r, re_end, *s, &ok);
        *alen = (int)(r - re);
        return m && ok;
    }
    if (*r == '\\' && r + 1 < re_end) {
        *alen = 2;
        return *s && *s == r[1];
    }
    *alen = 1;
    return *s && (*r == '.' || *r == *s);
}

static int re_here(const char *re, const char *re_end, const char *s)
{
    if (re == re_end)
        return 1;
    if (*re == '$' && re + 1 == re_end)
        return *s == 0;
    int alen;
    (void)re_atom(re, re_end, "x", &alen);
    const char *next = re + alen;
    char q = next < re_end ? *next : 0;
    if (q == '*' || q == '+' || q == '?') {
        const char *t = s;
        int n = 0, max = q == '?' ? 1 : 1 << 30;
        while (n < max && re_atom(re, re_end, t, &alen)) { t++; n++; }
        for (; n >= (q == '+' ? 1 : 0); n--, t--)
            if (re_here(next + 1, re_end, t))
                return 1;
        return 0;
    }
    return re_atom(re, re_end, s, &alen) && re_here(next, re_end, s + 1);
}

static int re_search(const char *re, const char *re_end, const char *s)
{
    if (re < re_end && *re == '^')
        return re_here(re + 1, re_end, s);
    for (;; s++) {
        if (re_here(re, re_end, s))
            return 1;
        if (!*s)
            return 0;
    }
}

static int re_list_hit(const char *list, const char *s)
{
    for (const char *p = list; p && *p; ) {
        const char *e = strchr(p, ';');
        if (!e) e = p + strlen(p);
        if (e > p && re_search(p, e, s))
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int file_wanted(const char *file)
{
    if (!file)
        return 1;
    if (g_filter && !re_list_hit(g_filter, file))
        return 0;
    return !(g_exclude && re_list_hit(g_exclude, file));
}

/* ---- the unit ------------------------------------------------------ */

const char *gcov_ctor_source(void)
{
    return "\n# 1 \"<coverage>\"\n"
           "static unsigned char __embcc_gcov_ctrs[8] "
           "__attribute__((used));\n"
           "static unsigned char __embcc_gcov_info[8] "
           "__attribute__((used)) = { 0 };\n"
           "void __embcc_gcov_init(void *);\n"
           "static void __attribute__((constructor, used, "
           "no_instrument_function, no_profile_instrument_function)) "
           "__embcc_gcov_ctor(void) "
           "{ __embcc_gcov_init(__embcc_gcov_info); }\n";
}

static void load_profile(void);

void gcov_unit_begin(struct unit *u, const char *src, const char *gcda,
                     unsigned stamp)
{
    g_src = src;
    g_gcda = gcda;
    g_stamp = stamp;
    g_ctrs = g_info = NULL;
    g_nctrs = 0;
    g_nb.n = 0;
    g_nfns = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed)
            continue;
        if (!strcmp(g->name, "__embcc_gcov_ctrs")) g_ctrs = g;
        if (!strcmp(g->name, "__embcc_gcov_info")) g_info = g;
    }
    if (g_use)
        load_profile();
}

/* The counters' array, now that its length is known, and the record
 * lib/gcov reads, in the target's byte order and pointer width:
 *
 *   ptr  counters              (the array above)
 *   ptr  next                  (the runtime's list of units)
 *   u32  magic, version, stamp, checksum, nfuncs, name_off, funcs_off,
 *        nctrs
 *   per function, at funcs_off: u32 ident, lineno_checksum,
 *        cfg_checksum, ncounters, first counter
 *   the .gcda's path, at name_off */
void gcov_unit_end(struct unit *u)
{
    (void)u;
    if (!g_arcs || !g_ctrs || !g_info)
        return;
    int P = target_ptr_size(), be = target_big_endian();
    long n = g_nctrs ? g_nctrs : 1;
    g_ctrs->ty = ty_array(ty_base(TY_CHAR, 1), (int)(n * 8));
    g_ctrs->user_align = 8;
    size_t head = (size_t)(2 * P + 32);
    size_t fo = (head + 3) & ~(size_t)3;
    size_t no = fo + (size_t)g_nfns * 20;
    size_t len = no + strlen(g_gcda) + 1;
    unsigned char *b = xcalloc(len, 1);
#define W32(at, v) do { unsigned v_ = (unsigned)(v); size_t a_ = (at);      \
        for (int k_ = 0; k_ < 4; k_++)                                    \
            b[a_ + (size_t)k_] = (unsigned char)(v_ >> (be ? 24 - 8 * k_  \
                                                           : 8 * k_));    \
    } while (0)
    size_t h = (size_t)(2 * P);
    W32(h, INFO_MAGIC);
    W32(h + 4, g_version);
    W32(h + 8, g_stamp);
    W32(h + 12, 0);
    W32(h + 16, g_nfns);
    W32(h + 20, no);
    W32(h + 24, fo);
    W32(h + 28, g_nctrs);
    for (int i = 0; i < g_nfns; i++) {
        size_t a = fo + (size_t)i * 20;
        W32(a, g_fns[i].ident);
        W32(a + 4, g_fns[i].lineno);
        W32(a + 8, g_fns[i].cfg);
        W32(a + 12, g_fns[i].n);
        W32(a + 16, g_fns[i].base);
    }
#undef W32
    memcpy(b + no, g_gcda, strlen(g_gcda) + 1);
    g_info->ty = ty_array(ty_base(TY_CHAR, 1), (int)len);
    g_info->user_align = P;
    g_info->has_init = 1;
    g_info->init_bytes = (const char *)b;
    g_info->init_len = (int)len;
    g_info->init_expr = NULL;
    g_info->relocs = xcalloc(1, sizeof *g_info->relocs);
    g_info->relocs[0].gtarget = g_ctrs;
    g_info->nrelocs = 1;
}

int gcov_write_notes(const char *gcno)
{
    if (!g_notes)
        return 0;
    struct outbuf b = { NULL, 0, 0 };
    put32(&b, GCOV_NOTE_MAGIC);
    put32(&b, g_version);
    put32(&b, g_stamp);
    if (g_bytes)
        put32(&b, 0);                    /* checksum: GCC writes 0 too */
    const char *cwd = plat_cwd();
    put_str(&b, cwd ? cwd : ".");
    put32(&b, 1);                        /* it says which blocks never ran */
    ob_add(&b, g_nb.p, g_nb.n);
    put32(&b, 0);
    int rc = plat_write_file(gcno, b.p, b.n);
    free(b.p);
    return rc;
}

/* ---- -fprofile-use: reading the .gcda ------------------------------ */

static unsigned rd32(const unsigned char *p, int swap)
{
    return swap ? (unsigned)p[3] | (unsigned)p[2] << 8 |
                  (unsigned)p[1] << 16 | (unsigned)p[0] << 24
                : (unsigned)p[0] | (unsigned)p[1] << 8 |
                  (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static void load_profile(void)
{
    g_nprof = 0;
    g_have_prof = 0;
    g_prof_max = 0;
    long len;
    unsigned char *d = (unsigned char *)plat_read_file(g_use, &len);
    if (!d) {
        diag_warn_opt(g_src, 0, 0, "missing-profile",
                      "'%s' profile count data file not found: the "
                      "program has not been run since -fprofile-generate "
                      "built it", g_use);
        return;
    }
    int swap;
    if (len >= 16 && rd32(d, 0) == GCOV_DATA_MAGIC) swap = 0;
    else if (len >= 16 && rd32(d, 1) == GCOV_DATA_MAGIC) swap = 1;
    else {
        diag_warn_opt(g_src, 0, 0, "coverage-mismatch",
                      "'%s' is not a .gcda file", g_use);
        free(d);
        return;
    }
    if (rd32(d + 4, swap) != g_version) {
        diag_warn_opt(g_src, 0, 0, "coverage-mismatch",
                      "'%s' is a profile of another gcov version; "
                      "it is ignored", g_use);
        free(d);
        return;
    }
    long at = g_bytes ? 16 : 12;
    int cap = 0;
    struct pfn *cur = NULL;
    while (at + 8 <= len) {
        unsigned tag = rd32(d + at, swap), n = rd32(d + at + 4, swap);
        if (!g_bytes)
            n *= 4;
        at += 8;
        if (!tag || at + (long)n > len)
            break;
        if (tag == TAG_FUNCTION && n >= 12) {
            if (g_nprof == cap) {
                cap = cap ? cap * 2 : 64;
                g_prof = xrealloc(g_prof, (size_t)cap * sizeof *g_prof);
            }
            cur = &g_prof[g_nprof++];
            cur->ident = rd32(d + at, swap);
            cur->lineno = rd32(d + at + 4, swap);
            cur->cfg = rd32(d + at + 8, swap);
            cur->n = 0;
            cur->c = NULL;
        } else if (tag == TAG_COUNTER_ARCS && cur) {
            cur->n = n / 8;
            cur->c = xmalloc((size_t)(cur->n ? cur->n : 1) * sizeof *cur->c);
            for (long k = 0; k < cur->n; k++)
                cur->c[k] = rd32(d + at + 8 * k, swap) |
                            (unsigned long long)rd32(d + at + 8 * k + 4,
                                                     swap) << 32;
        } else if (tag == TAG_OBJECT_SUMMARY && n >= 8) {
            g_prof_max = rd32(d + at + 4, swap);
        }
        at += n;
    }
    free(d);
    g_have_prof = 1;
}

/* ---- one function -------------------------------------------------- */

struct gblk {
    int lo, hi;            /* IR [lo, hi) of a code block; 0, 0 otherwise */
    int body;              /* where its code starts: after its label and
                            * a landing pad's IR_LANDING */
    int landing;
    int head;              /* a labelled block's head, or -1 */
    int nsucc, npred;      /* arcs that are code, not FAKE */
};

struct garc {
    int src, dst, flags;
    int place;             /* where its counter goes (P_*) */
    int at;                /* ...the IR position, for P_AT */
    int br;                /* ...the branch or switch, for P_SPLIT */
    int order;             /* the tree's preference: lower goes first */
    int ctr;               /* its counter, or -1 */
    unsigned long long count;
    int known;
};

enum { P_NONE, P_AT, P_SPLIT };

static int uf_find(int *p, int x)
{
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}

static int is_term(enum ir_op op)
{
    return op == IR_JMP || op == IR_BRZ || op == IR_BRNZ || op == IR_RET ||
           op == IR_UD2 || op == IR_IGOTO || op == IR_SWITCH;
}

/* The flow solution: every arc's count from the counted ones. Tree arcs
 * are found by peeling -- a block with all of its arcs on one side
 * known and one unknown on the other gives that one -- from a worklist,
 * since a function may have thousands of blocks. Arc na is the exit to
 * entry one that closes the graph. */
static void solve_tree(struct garc *A, int na, int nb)
{
    int *nin = xcalloc((size_t)nb, sizeof *nin), *nout = xcalloc((size_t)nb, sizeof *nout);
    int *ui = xcalloc((size_t)nb, sizeof *ui), *uo = xcalloc((size_t)nb, sizeof *uo);
    unsigned long long *sin = xcalloc((size_t)nb, sizeof *sin);
    unsigned long long *sout = xcalloc((size_t)nb, sizeof *sout);
    int *work = xmalloc((size_t)(2 * (na + 1) + nb + 1) * sizeof *work);
    int nw = 0;
    /* per block: how many arcs are unknown on each side, the sum of the
     * known ones, and the XOR of the unknown ones' indexes -- which is
     * the index itself once a single one is left */
    for (int k = 0; k <= na; k++) {
        if (A[k].known) {
            sout[A[k].src] += A[k].count;
            sin[A[k].dst] += A[k].count;
        } else {
            nout[A[k].src]++; uo[A[k].src] ^= k;
            nin[A[k].dst]++; ui[A[k].dst] ^= k;
        }
    }
    for (int b = 0; b < nb; b++) work[nw++] = b;
    while (nw) {
        int b = work[--nw];
        int k = -1;
        unsigned long long v = 0;
        if (!nin[b] && nout[b] == 1) { k = uo[b]; v = sin[b] - sout[b]; }
        else if (!nout[b] && nin[b] == 1) { k = ui[b]; v = sout[b] - sin[b]; }
        if (k < 0)
            continue;
        A[k].known = 1;
        A[k].count = v;
        int s = A[k].src, d = A[k].dst;
        nout[s]--; uo[s] ^= k; sout[s] += v;
        nin[d]--; ui[d] ^= k; sin[d] += v;
        work[nw++] = s;
        work[nw++] = d;
    }
    free(nin); free(nout); free(ui); free(uo); free(sin); free(sout);
    free(work);
}

static struct pfn *find_prof(unsigned ident)
{
    for (int i = 0; i < g_nprof; i++)
        if (g_prof[i].ident == ident)
            return &g_prof[i];
    return NULL;
}

static unsigned fn_ident(const char *name)
{
    unsigned id = crc32_add(0, name, strlen(name)) & 0x7fffffffu;
    for (int again = 1; again; ) {
        again = 0;
        for (int i = 0; i < g_nfns; i++)
            if (g_fns[i].ident == id) { id = (id + 1) & 0x7fffffffu; again = 1; }
    }
    return id;
}

/* What the profile says, on the IR: each instruction's block heat, and
 * each conditional branch's probability. */
static void annotate(struct ir_func *fn, struct func *f, struct gblk *B,
                     int nb, struct garc *A, int na, const int *byblk,
                     const int *first)
{
    unsigned long long entry = 0, hot = g_prof_max / 16;
    for (int o = first[0]; o < first[1]; o++)
        entry += A[byblk[o]].count;
    if (hot < 1000)
        hot = 1000;
    /* what enters each block (a labelled block's head counts as it) */
    unsigned long long *cin = xcalloc((size_t)nb, sizeof *cin);
    for (int k = 0; k < na; k++)
        cin[A[k].dst] += A[k].count;
    for (int b = 2; b < nb; b++)
        if (B[b].head >= 0)
            cin[b] += cin[B[b].head];
    for (int b = 2; b < nb; b++) {
        if (B[b].hi <= B[b].lo)
            continue;
        /* a block that leaves only by its fake arc (a call that never
         * came back) ran as often as it was entered */
        unsigned long long c = cin[b];
        int heat = c == 0 ? IR_HEAT_COLD : c >= hot ? IR_HEAT_HOT
                                                    : IR_HEAT_NORMAL;
        for (int n = B[b].lo; n < B[b].hi; n++)
            fn->ins[n].heat = (unsigned char)heat;
        struct ir_ins *t = &fn->ins[B[b].hi - 1];
        if ((t->op == IR_BRZ || t->op == IR_BRNZ) && c) {
            unsigned long long taken = 0;
            for (int o = first[b]; o < first[b + 1]; o++)
                if (!(A[byblk[o]].flags & (ARC_FALLTHROUGH | ARC_FAKE)))
                    taken = A[byblk[o]].count;
            if (taken > c)
                taken = c;
            t->prob = (short)(1 + (int)(taken * 1000 / c));
        }
    }
    free(cin);
    f->prof_heat = entry == 0 ? IR_HEAT_COLD : entry >= hot ? IR_HEAT_HOT
                                                           : IR_HEAT_NORMAL;
    if (entry == 0 && g_partial) {
        f->prof_heat = 0;
        for (int n = 0; n < fn->nins; n++)
            fn->ins[n].heat = 0;
    }
}

void irgen_counter_incr(struct ir_func *fn, struct global *ctrs, long off,
                        int atomic);

/* The function's code with the counters in (ir_splice, ir.h). */
#define gins ir_slice

static int gins_cmp(const void *a, const void *b)
{
    const struct gins *x = a, *y = b;
    return x->at != y->at ? (x->at < y->at ? -1 : 1)
                          : (x->lo < y->lo ? -1 : x->lo > y->lo);
}

void ir_splice(struct ir_func *fn, int n0, int slices_end,
               struct ir_slice *gi, int ng)
{
    qsort(gi, (size_t)ng, sizeof *gi, gins_cmp);
    int total = fn->nins;           /* n0 code + slices + trampolines */
    struct ir_ins *out = xmalloc((size_t)(total ? total : 1) * sizeof *out);
    int *newpos = xmalloc((size_t)(n0 + 1) * sizeof *newpos);
    int o = 0, g = 0;
    for (int n = 0; n <= n0; n++) {
        while (g < ng && gi[g].at == n) {
            int line = n < n0 ? fn->ins[n].line
                              : n0 ? fn->ins[n0 - 1].line : 0;
            for (int s = gi[g].lo; s < gi[g].hi; s++) {
                out[o] = fn->ins[s];
                out[o].line = line;
                out[o].synth = 1;
                o++;
            }
            g++;
        }
        newpos[n] = o;
        if (n < n0)
            out[o++] = fn->ins[n];
    }
    /* the trampolines, last */
    for (int s = slices_end; s < total; s++)
        out[o++] = fn->ins[s];
    free(fn->ins);
    fn->ins = out;
    fn->nins = o;
    fn->cap = total ? total : 1;
    for (int r = 0; r < fn->neh; r++) {
        if (fn->eh[r].lo >= 0 && fn->eh[r].lo <= n0)
            fn->eh[r].lo = newpos[fn->eh[r].lo];
        if (fn->eh[r].hi >= 0 && fn->eh[r].hi <= n0)
            fn->eh[r].hi = newpos[fn->eh[r].hi];
    }
    for (int v = 0; fn->var_scope_lo && v < fn->nvars; v++) {
        int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
        if (lo >= 0 && lo <= n0) fn->var_scope_lo[v] = newpos[lo];
        if (hi >= 0 && hi <= n0)
            fn->var_scope_hi[v] = hi == n0 ? fn->nins : newpos[hi];
    }
    free(newpos);
}

static int arc_order_cmp(const void *a, const void *b)
{
    const struct garc *x = *(const struct garc *const *)a;
    const struct garc *y = *(const struct garc *const *)b;
    return x->order != y->order ? (x->order < y->order ? -1 : 1)
                                : (x < y ? -1 : x > y);
}

void gcov_function(struct ir_unit *iu, struct ir_func *fn, struct func *f)
{
    (void)iu;
    if (!gcov_active() || f->attr_no_profile || f->is_naked ||
        f->inline_only || !strncmp(f->name, "__embcc_gcov", 12) ||
        !file_wanted(fn->file) || fn->nins == 0)
        return;
    int n0 = fn->nins;

    /* the code's blocks */
    char *lead = xcalloc((size_t)n0 + 1, 1);
    lead[0] = 1;
    int igoto = 0;
    for (int n = 0; n < n0; n++) {
        enum ir_op op = fn->ins[n].op;
        if (op == IR_LABEL) lead[n] = 1;
        if ((is_term(op) || op == IR_CALL) && n + 1 < n0) lead[n + 1] = 1;
        if (op == IR_IGOTO) igoto = 1;
    }
    int ncode = 0;
    for (int n = 0; n < n0; n++) ncode += lead[n];
    int nb = 2 + ncode;
    int dstar = -1;
    int cap = nb + (igoto ? ncode + 1 : 0);
    struct gblk *B = xcalloc((size_t)cap, sizeof *B);
    int *l2b = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *l2b);
    for (int l = 0; l < fn->nlabels; l++) l2b[l] = -1;
    {
        int b = 1;
        for (int n = 0; n < n0; n++) {
            if (lead[n]) { b++; B[b].lo = n; }
            B[b].hi = n + 1;
        }
    }
    for (int b = 0; b < nb; b++) {
        B[b].head = -1;
        if (b < 2) continue;
        int k = B[b].lo;
        while (k < B[b].hi && fn->ins[k].op == IR_LABEL) {
            l2b[fn->ins[k].label] = b;
            k++;
        }
        if (k < B[b].hi && fn->ins[k].op == IR_LANDING) {
            B[b].landing = 1;
            k++;
        }
        B[b].body = k;
    }
    if (igoto) {
        dstar = nb++;
        B[dstar].head = -1;
        for (int b = 2; b < 2 + ncode; b++)
            if (fn->ins[B[b].lo].op == IR_LABEL && !B[b].landing) {
                B[b].head = nb;
                B[nb].head = -1;
                nb++;
            }
    }
    free(lead);

    /* the arcs */
    int na = 0, acap = 2 * ncode + 8;
    struct garc *A = xcalloc((size_t)acap + 1, sizeof *A);
#define ARC(s_, d_, fl_, br_) do {                                         \
        if (na == acap) {                                                  \
            acap *= 2;                                                     \
            A = xrealloc(A, (size_t)(acap + 1) * sizeof *A);               \
        }                                                                  \
        memset(&A[na], 0, sizeof A[na]);                                   \
        A[na].src = (s_); A[na].dst = (d_); A[na].flags = (fl_);           \
        A[na].br = (br_); A[na].ctr = -1;                                  \
        na++;                                                              \
    } while (0)
#define TGT(b_) (B[b_].head >= 0 ? B[b_].head : (b_))
    if (ncode)
        ARC(0, TGT(2), ARC_FALLTHROUGH, -1);
    for (int b = 2; b < 2 + ncode; b++) {
        if (B[b].landing)
            ARC(0, b, 0, -1);
        int last = B[b].hi - 1;
        struct ir_ins *t = &fn->ins[last];
        int next = b + 1 < 2 + ncode ? b + 1 : 1;
        int nextt = next == 1 ? 1 : TGT(next);
        switch (t->op) {
        case IR_JMP:
            if (t->label >= 0 && l2b[t->label] >= 0)
                ARC(b, TGT(l2b[t->label]), 0, last);
            break;
        case IR_BRZ: case IR_BRNZ:
            if (t->label >= 0 && l2b[t->label] >= 0)
                ARC(b, TGT(l2b[t->label]), 0, last);
            ARC(b, nextt, ARC_FALLTHROUGH, -1);
            break;
        case IR_SWITCH: {
            /* one arc per distinct target */
            int seen0 = na;
            const struct ir_jt *jt = &fn->jt[t->jt];
            for (int k = -1; k < jt->n; k++) {
                int L = k < 0 ? t->label : jt->labels[k];
                if (L < 0 || l2b[L] < 0)
                    continue;
                int d = TGT(l2b[L]), dup = 0;
                for (int q = seen0; q < na; q++)
                    if (A[q].dst == d) dup = 1;
                if (!dup)
                    ARC(b, d, 0, last);
            }
            break;
        }
        case IR_RET:
            ARC(b, 1, 0, -1);
            break;
        case IR_IGOTO:
            ARC(b, dstar, 0, -1);
            break;
        case IR_UD2:
            break;
        case IR_CALL:
            ARC(b, nextt, ARC_FALLTHROUGH, -1);
            ARC(b, 1, ARC_FAKE, -1);
            break;
        default:
            ARC(b, nextt, ARC_FALLTHROUGH, -1);
            break;
        }
    }
    if (igoto)
        for (int b = 2; b < 2 + ncode; b++)
            if (B[b].head >= 0) {
                ARC(dstar, B[b].head, 0, -1);
                ARC(B[b].head, b, ARC_FALLTHROUGH, -1);
            }
#undef ARC

    /* Blocks nothing reaches -- irgen's return after a body that always
     * returned, code after a noreturn call -- leave the graph: GCC's has
     * no code for them, and gcov would report them as never run. The
     * entry and the exit stay, and the code blocks keep their order, so
     * the numbering is the same in every build of the same source. */
    {
        int *cnt = xcalloc((size_t)nb + 1, sizeof *cnt);
        int *ord = xmalloc((size_t)(na ? na : 1) * sizeof *ord);
        for (int k = 0; k < na; k++) cnt[A[k].src + 1]++;
        for (int b = 0; b < nb; b++) cnt[b + 1] += cnt[b];
        int *fill = xmalloc((size_t)nb * sizeof *fill);
        memcpy(fill, cnt, (size_t)nb * sizeof *fill);
        for (int k = 0; k < na; k++) ord[fill[A[k].src]++] = k;
        free(fill);
        char *reach = xcalloc((size_t)nb, 1);
        int *wl = xmalloc((size_t)nb * sizeof *wl), nw = 0;
        reach[0] = reach[1] = 1;
        wl[nw++] = 0;
        while (nw) {
            int b = wl[--nw];
            for (int o = cnt[b]; o < cnt[b + 1]; o++) {
                int d = A[ord[o]].dst;
                if (!reach[d]) { reach[d] = 1; wl[nw++] = d; }
            }
        }
        int *nid = xmalloc((size_t)nb * sizeof *nid), m = 0, mcode = 0;
        for (int b = 0; b < nb; b++) {
            nid[b] = reach[b] ? m++ : -1;
            if (reach[b] && b >= 2 && b < 2 + ncode)
                mcode++;
        }
        for (int b = 0; b < nb; b++)
            if (nid[b] >= 0) {
                int h = B[b].head;
                B[nid[b]] = B[b];
                B[nid[b]].head = h >= 0 ? nid[h] : -1;
            }
        int ka = 0;
        for (int k = 0; k < na; k++)
            if (reach[A[k].src]) {
                A[ka] = A[k];
                A[ka].src = nid[A[k].src];
                A[ka].dst = nid[A[k].dst];
                ka++;
            }
        for (int l = 0; l < fn->nlabels; l++)
            if (l2b[l] >= 0)
                l2b[l] = nid[l2b[l]];
        if (dstar >= 0)
            dstar = nid[dstar];
        na = ka;
        nb = m;
        ncode = mcode;
        free(nid);
        free(reach);
        free(wl);
        free(ord);
        free(cnt);
    }

    for (int k = 0; k < na; k++)
        if (!(A[k].flags & ARC_FAKE)) {
            B[A[k].src].nsucc++;
            B[A[k].dst].npred++;
        }

    /* where each arc's counter would go, and the tree's preferences */
    for (int k = 0; k < na; k++) {
        struct garc *a = &A[k];
        int s = a->src, d = a->dst;
        if (a->flags & ARC_FAKE) {
            a->order = 1;
            continue;
        }
        if (s == dstar) { a->order = 0; continue; }
        if (s == 0) {
            a->place = P_AT;
            a->at = B[d].landing ? B[d].body : 0;
            a->order = 4;
            continue;
        }
        if (s >= 2 + ncode) {               /* a head into its label */
            a->place = P_AT;
            a->at = B[d].body;
            a->order = 4;
            continue;
        }
        struct ir_ins *t = &fn->ins[B[s].hi - 1];
        int d_code = d >= 2 && d < 2 + ncode;
        if (B[s].nsucc == 1) {
            a->place = P_AT;
            a->at = (t->op == IR_JMP || t->op == IR_RET ||
                     t->op == IR_IGOTO || t->op == IR_SWITCH)
                    ? B[s].hi - 1 : B[s].hi;
        } else if (d_code && B[d].npred == 1 && !B[d].landing) {
            a->place = P_AT;
            a->at = B[d].body;
        } else if (a->flags & ARC_FALLTHROUGH) {
            a->place = P_AT;
            a->at = B[s].hi;                /* after the branch */
        } else {
            a->place = P_SPLIT;
        }
        int back = d_code && d <= s;
        a->order = a->place == P_SPLIT ? 2 : back ? 3 : 4;
    }

    /* the spanning tree */
    int *uf = xmalloc((size_t)nb * sizeof *uf);
    for (int b = 0; b < nb; b++) uf[b] = b;
    uf[1] = 0;                              /* exit -> entry */
    struct garc **ord = xmalloc((size_t)(na ? na : 1) * sizeof *ord);
    for (int k = 0; k < na; k++) ord[k] = &A[k];
    qsort(ord, (size_t)na, sizeof *ord, arc_order_cmp);
    for (int k = 0; k < na; k++) {
        int x = uf_find(uf, ord[k]->src), y = uf_find(uf, ord[k]->dst);
        if (x != y) {
            uf[x] = y;
            ord[k]->flags |= ARC_ON_TREE;
        }
    }
    free(ord);
    free(uf);

    /* the counters, in the .gcno's order: by source block, then as the
     * arcs were made (which is how gcov numbers them) */
    int *byblk = xmalloc((size_t)(na ? na : 1) * sizeof *byblk);
    int *first = xcalloc((size_t)nb + 1, sizeof *first);  /* byblk's start */
    {
        for (int k = 0; k < na; k++)
            first[A[k].src + 1]++;
        for (int b = 0; b < nb; b++)
            first[b + 1] += first[b];
        int *fill = xmalloc((size_t)nb * sizeof *fill);
        memcpy(fill, first, (size_t)nb * sizeof *fill);
        for (int k = 0; k < na; k++)
            byblk[fill[A[k].src]++] = k;
        free(fill);
    }
    long nct = 0;
    for (int o = 0; o < na; o++) {
        struct garc *a = &A[byblk[o]];
        if (!(a->flags & ARC_ON_TREE))
            a->ctr = (int)nct++;
    }

    unsigned ident = fn_ident(f->name);
    unsigned lineno = crc32_u32(crc32_add(0, fn->file ? fn->file : "",
                                          fn->file ? strlen(fn->file) : 0),
                                (unsigned)f->line);
    unsigned cfg = crc32_u32(0, (unsigned)nb);
    for (int o = 0; o < na; o++) {
        struct garc *a = &A[byblk[o]];
        cfg = crc32_u32(cfg, (unsigned)a->src);
        cfg = crc32_u32(cfg, (unsigned)a->dst);
        cfg = crc32_u32(cfg, (unsigned)a->flags);
    }

    /* -fprofile-use: the counts, and what they say */
    if (g_use && g_have_prof) {
        struct pfn *p = find_prof(ident);
        if (p && (p->cfg != cfg || p->n != nct)) {
            diag_warn_opt(fn->file, f->line, 0, "coverage-mismatch",
                          "the profile of '%s' does not match its code "
                          "(its source or the options changed): it is "
                          "ignored", f->name);
            p = NULL;
        }
        if (p) {
            /* arc na: the exit -> entry one gcov adds */
            A[na].src = 1;
            A[na].dst = 0;
            A[na].known = 0;
            for (int k = 0; k < na; k++) {
                A[k].known = A[k].ctr >= 0;
                A[k].count = A[k].ctr >= 0 ? p->c[A[k].ctr] : 0;
            }
            solve_tree(A, na, nb);
            annotate(fn, f, B, nb, A, na, byblk, first);
        }
    }

    /* the notes */
    if (g_notes) {
        struct outbuf *b = &g_nb;
        int endl = f->line;
        for (int n = 0; n < n0; n++)
            if (fn->ins[n].line > endl) endl = fn->ins[n].line;
        size_t at = rec_open(b, TAG_FUNCTION);
        put32(b, ident);
        put32(b, lineno);
        put32(b, cfg);
        put_str(b, f->name);
        put32(b, 0);                        /* artificial */
        put_str(b, fn->file ? note_file(fn->file) : g_src);
        put32(b, (unsigned)f->line);
        put32(b, f->name_col > 0 ? (unsigned)f->name_col : 1u);
        put32(b, (unsigned)endl);
        put32(b, 1);
        rec_close(b, at);
        at = rec_open(b, TAG_BLOCKS);
        put32(b, (unsigned)nb);
        rec_close(b, at);
        for (int s = 0; s < nb; s++) {
            size_t ra = 0;
            int any = 0;
            for (int o = first[s]; o < first[s + 1]; o++) {
                struct garc *a = &A[byblk[o]];
                if (!any) {
                    ra = rec_open(b, TAG_ARCS);
                    put32(b, (unsigned)s);
                    any = 1;
                }
                put32(b, (unsigned)a->dst);
                put32(b, (unsigned)a->flags);
            }
            if (any)
                rec_close(b, ra);
        }
        for (int s = 2; s < 2 + ncode; s++) {
            int last = -1, any = 0;
            size_t ra = 0;
            for (int n = B[s].lo; n < B[s].hi; n++) {
                int L = fn->ins[n].line;
                if (s == 2 && !any && f->line > 0 && L != f->line) {
                    ra = rec_open(b, TAG_LINES);
                    put32(b, (unsigned)s);
                    put32(b, 0);
                    put_str(b, fn->file ? note_file(fn->file) : g_src);
                    put32(b, (unsigned)f->line);
                    last = f->line;
                    any = 1;
                }
                if (L <= 0 || L == last)
                    continue;
                if (!any) {
                    ra = rec_open(b, TAG_LINES);
                    put32(b, (unsigned)s);
                    put32(b, 0);
                    put_str(b, fn->file ? note_file(fn->file) : g_src);
                    any = 1;
                }
                put32(b, (unsigned)L);
                last = L;
            }
            if (any) {
                put32(b, 0);
                put32(b, 0);                /* no more files */
                rec_close(b, ra);
            }
        }
    }

    /* every function's ident is kept, so that the next one's is unique
     * the same way in every build of this unit */
    if (g_nfns == g_capfns) {
        g_capfns = g_capfns ? g_capfns * 2 : 32;
        g_fns = xrealloc(g_fns, (size_t)g_capfns * sizeof *g_fns);
    }
    g_fns[g_nfns].ident = ident;
    g_fns[g_nfns].lineno = lineno;
    g_fns[g_nfns].cfg = cfg;
    g_fns[g_nfns].base = g_nctrs;
    g_fns[g_nfns].n = nct;
    g_nfns++;

    /* the counters' code */
    if (g_arcs && g_ctrs) {
        struct gins *gi = xmalloc((size_t)(na ? na : 1) * sizeof *gi);
        int ng = 0;
        for (int k = 0; k < na; k++) {
            struct garc *a = &A[k];
            if (a->ctr < 0 || a->place != P_AT)
                continue;
            gi[ng].at = a->at;
            gi[ng].lo = fn->nins;
            irgen_counter_incr(fn, g_ctrs, (g_nctrs + a->ctr) * 8, g_atomic);
            gi[ng].hi = fn->nins;
            ng++;
        }
        /* the arcs that need a block of their own: at the end, and the
         * branch pointed at it */
        int trampolines = fn->nins;
        for (int k = 0; k < na; k++) {
            struct garc *a = &A[k];
            if (a->ctr < 0 || a->place != P_SPLIT)
                continue;
            int dcode = a->dst;
            if (dcode >= 2 + ncode) {         /* a head: its label */
                for (int b = 2; b < 2 + ncode; b++)
                    if (B[b].head == dcode) dcode = b;
            }
            int L = fn->ins[B[dcode].lo].label;
            int T = fn->nlabels++;
            struct ir_ins *br = &fn->ins[a->br];
            if (br->op == IR_SWITCH) {
                struct ir_jt *jt = &fn->jt[br->jt];
                for (int q = 0; q < jt->n; q++)
                    if (jt->labels[q] >= 0 && l2b[jt->labels[q]] == dcode)
                        jt->labels[q] = T;
                if (br->label >= 0 && l2b[br->label] == dcode)
                    br->label = T;
            } else {
                br->label = T;
            }
            int line = br->line;
            struct ir_ins *li = emit(fn);
            li->op = IR_LABEL;
            li->label = T;
            li->line = line;
            li->synth = 1;
            int lo = fn->nins;
            irgen_counter_incr(fn, g_ctrs, (g_nctrs + a->ctr) * 8, g_atomic);
            for (int s = lo; s < fn->nins; s++) {
                fn->ins[s].line = line;
                fn->ins[s].synth = 1;
            }
            struct ir_ins *j = emit(fn);
            j->op = IR_JMP;
            j->label = L;
            j->line = line;
            j->synth = 1;
        }
        ir_splice(fn, n0, trampolines, gi, ng);
        free(gi);
        g_nctrs += nct;
    }
    free(first);
    free(byblk);
    free(A);
    free(B);
    free(l2b);
}

/* ---- embcc --gcov-extract LOG... -----------------------------------
 *
 * A board has no file system, so its libgcov printed each .gcda on the
 * console (lib/gcov/gcov.c): `@gcda PATH N`, N bytes as hex, `@end`.
 * Each is written here -- merged with the file already there when that
 * is the same program's, as the runtime merges -- at PATH, or where
 * GCOV_PREFIX and GCOV_PREFIX_STRIP move it. Other text on the console
 * is passed over, so the log can be the board's whole output. */
#include "gcda.h"

static char *read_stdin(long *len)
{
    size_t cap = 65536, n = 0;
    char *b = xmalloc(cap);
    for (;;) {
        size_t got = fread(b + n, 1, cap - n, stdin);
        n += got;
        if (n < cap)
            break;
        cap *= 2;
        b = xrealloc(b, cap);
    }
    *len = (long)n;
    return b;
}

static char *gcov_place(const char *path)
{
    const char *pre = plat_getenv("GCOV_PREFIX");
    const char *st = plat_getenv("GCOV_PREFIX_STRIP");
    int strip = st ? atoi(st) : 0;
    const char *p = path;
    for (int k = 0; k < strip && *p; k++) {
        const char *q = p + (*p == '/');
        while (*q && *q != '/')
            q++;
        if (!*q)
            break;
        p = q;
    }
    size_t n = (pre ? strlen(pre) : 0) + strlen(p) + 2;
    char *r = xmalloc(n);
    snprintf(r, n, "%s%s%s", pre ? pre : "",
             pre && *pre && *p != '/' ? "/" : "", p);
    return r;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f'
           ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int gcov_extract(int nlogs, char **logs)
{
    int files = 0, bad = 0;
    for (int L = 0; L < (nlogs ? nlogs : 1); L++) {
        const char *name = nlogs ? logs[L] : "-";
        long len;
        char *t = strcmp(name, "-") ? plat_read_file(name, &len)
                                    : read_stdin(&len);
        if (!t) {
            fprintf(stderr, "embcc: error: --gcov-extract: cannot read '%s'\n",
                    name);
            return 1;
        }
        char *end = t + len;
        for (char *p = t; p < end; ) {
            char *m = NULL;
            for (char *q = p; q + 6 <= end; q++)
                if (!memcmp(q, "@gcda ", 6)) { m = q; break; }
            if (!m)
                break;
            char *path = m + 6, *sp = path;
            while (sp < end && *sp != ' ' && *sp != '\n' && *sp != '\r')
                sp++;
            unsigned long want = strtoul(sp, NULL, 10);
            char *nl = sp;
            while (nl < end && *nl != '\n')
                nl++;
            char *pth = xstrndup(path, (size_t)(sp - path));
            unsigned char *img = xmalloc(want ? want : 1);
            unsigned long got = 0;
            char *q = nl;
            int hi = -1;
            for (; q < end && got < want; q++) {
                if (*q == '@')
                    break;
                int v = hexval((unsigned char)*q);
                if (v < 0)
                    continue;
                if (hi < 0) hi = v;
                else { img[got++] = (unsigned char)(hi << 4 | v); hi = -1; }
            }
            p = q;
            if (got != want || gcda_order(img, want) < 0) {
                fprintf(stderr, "embcc: warning: --gcov-extract: '%s' in "
                                "'%s' is cut short (%lu of %lu bytes); not "
                                "written\n", pth, name, got, want);
                bad = 1;
            } else {
                char *at = gcov_place(pth);
                long olen;
                unsigned char *old = (unsigned char *)plat_read_file(at, &olen);
                int merged = old && gcda_merge(img, want, old,
                                               (unsigned long)olen);
                free(old);
                if (plat_write_file(at, img, want) != 0) {
                    fprintf(stderr, "embcc: error: --gcov-extract: cannot "
                                    "write '%s'\n", at);
                    bad = 1;
                } else {
                    printf("%s%s\n", at, merged ? " (merged)" : "");
                    files++;
                }
                free(at);
            }
            free(pth);
            free(img);
        }
        free(t);
    }
    if (!files && !bad)
        fprintf(stderr, "embcc: warning: --gcov-extract: no .gcda in the "
                        "log\n");
    return bad;
}
