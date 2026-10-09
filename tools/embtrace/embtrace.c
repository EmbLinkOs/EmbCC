/* embtrace -- what a program traced with -finstrument-functions did:
 * calls, times and the call tree, from the ring lib/rt/embtrace.c keeps
 * on the target and embtrace_dump() writes to a console or a UART.
 *
 *   embtrace IMAGE.elf LOG [--tree] [--depth N] [--chrome OUT.json]
 *
 * IMAGE names the addresses (its ELF symbol table: 32 or 64 bits, either
 * byte order; the Thumb bit of an ARM function's address is not part of
 * it). LOG is any text holding a dump -- a QEMU console log, a terminal
 * capture -- and the LAST dump in it is read:
 *
 *   EMBTRACE 1 <pointer bytes> <capacity> <recorded> <clock Hz>
 *   <kind> <fn> <site> <time>          kind 1 enter, 2 exit; hex
 *   EMBTRACE END
 *
 * The events are replayed against a stack. An interrupt's calls nest
 * inside whatever it interrupted, so they replay like any call. The ring
 * keeps the newest events only, so the oldest frames' entries may be
 * gone: an exit with no entry on the stack is counted and dropped, and a
 * frame still open at the end is reported as such. Each entry's call
 * site (the hooks' second argument, the return address) is checked to be
 * in the function the stack says called it -- unless it is in a function
 * the trace never names, which was not instrumented.
 *
 * Without --tree: one line per function -- calls, total time with its
 * callees, self time, the longest single call -- largest total first.
 * Times are in the clock's units (cycles, for the ready-made clocks); with
 * no clock (every time 0) the counts and the tree are still exact.
 * --tree prints the calls aggregated by path. --chrome writes the trace
 * as Chrome's JSON trace events, which Perfetto and chrome://tracing
 * open, one begin/end pair per call (the event's index stands in for the
 * time when there is no clock).
 *
 * ISO C; docs/manual/tools/embtrace.md is the reference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef unsigned long long u64;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "embtrace: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    size_t cap = 1 << 16, n = 0;
    unsigned char *b = xmalloc(cap);
    for (;;) {
        if (n == cap) b = xrealloc(b, cap *= 2);
        size_t r = fread(b + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    fclose(f);
    b = xrealloc(b, n + 1);
    b[n] = 0;
    *len = n;
    return b;
}

/* ==== the image's functions ============================================= */

struct fsym { u64 addr, size; const char *name; };
static struct fsym *g_sym;
static int g_nsym;
static int g_thumb;                 /* EM_ARM: the low bit is the state */

static unsigned char *g_e;
static size_t g_elen;
static int g_be, g_64;

static u64 rd(size_t off, int n)
{
    u64 v = 0;
    if (off + (size_t)n > g_elen) die("the image is truncated");
    for (int k = 0; k < n; k++)
        v |= (u64)g_e[off + (size_t)(g_be ? n - 1 - k : k)] << (8 * k);
    return v;
}

static int sym_cmp(const void *a, const void *b)
{
    const struct fsym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void load_elf(const char *path)
{
    g_e = slurp(path, &g_elen);
    if (g_elen < 52 || memcmp(g_e, "\177ELF", 4))
        die("%s is not an ELF file", path);
    g_64 = g_e[4] == 2;
    g_be = g_e[5] == 2;
    unsigned mach = (unsigned)rd(18, 2);
    g_thumb = mach == 40;                       /* EM_ARM */
    u64 shoff = g_64 ? rd(0x28, 8) : rd(0x20, 4);
    int shentsize = (int)rd(g_64 ? 0x3a : 0x2e, 2);
    int shnum = (int)rd(g_64 ? 0x3c : 0x30, 2);
    for (int s = 0; s < shnum; s++) {
        size_t sh = (size_t)(shoff + (u64)s * (u64)shentsize);
        unsigned type = (unsigned)rd(sh + 4, 4);
        if (type != 2)                          /* SHT_SYMTAB */
            continue;
        u64 off = g_64 ? rd(sh + 0x18, 8) : rd(sh + 0x10, 4);
        u64 size = g_64 ? rd(sh + 0x20, 8) : rd(sh + 0x14, 4);
        unsigned link = (unsigned)rd(sh + (g_64 ? 0x28 : 0x18), 4);
        u64 entsz = g_64 ? rd(sh + 0x38, 8) : rd(sh + 0x24, 4);
        size_t strsh = (size_t)(shoff + (u64)link * (u64)shentsize);
        u64 stroff = g_64 ? rd(strsh + 0x18, 8) : rd(strsh + 0x10, 4);
        if (!entsz) continue;
        for (u64 k = 1; k < size / entsz; k++) {
            size_t e = (size_t)(off + k * entsz);
            unsigned name = (unsigned)rd(e, 4);
            unsigned info = g_64 ? (unsigned)rd(e + 4, 1) : (unsigned)rd(e + 12, 1);
            u64 val = g_64 ? rd(e + 8, 8) : rd(e + 4, 4);
            u64 sz = g_64 ? rd(e + 16, 8) : rd(e + 8, 4);
            if ((info & 15) != 2)               /* STT_FUNC */
                continue;
            if (g_thumb) val &= ~1ULL;
            g_sym = xrealloc(g_sym, (size_t)(g_nsym + 1) * sizeof *g_sym);
            g_sym[g_nsym].addr = val;
            g_sym[g_nsym].size = sz;
            g_sym[g_nsym].name = (const char *)g_e + stroff + name;
            g_nsym++;
        }
    }
    if (!g_nsym)
        die("%s has no function symbols (a stripped image?)", path);
    qsort(g_sym, (size_t)g_nsym, sizeof *g_sym, sym_cmp);
}

/* the function holding address a, or NULL */
static const struct fsym *sym_at(u64 a)
{
    if (g_thumb) a &= ~1ULL;
    int lo = 0, hi = g_nsym - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_sym[mid].addr <= a) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    const struct fsym *s = &g_sym[best];
    if (s->size ? a < s->addr + s->size : a == s->addr)
        return s;
    return NULL;
}

static const char *name_of(u64 a, char *buf, size_t n)
{
    const struct fsym *s = sym_at(a);
    if (s) return s->name;
    snprintf(buf, n, "0x%llx", a);
    return buf;
}

/* ==== the dump =========================================================== */

struct ev { int kind; u64 fn, site, t; };
static struct ev *g_ev;
static int g_nev;
static u64 g_cap, g_recorded, g_hz;

static void load_log(const char *path)
{
    size_t len;
    char *text = (char *)slurp(path, &len), *last = NULL;
    for (char *p = text; (p = strstr(p, "EMBTRACE 1 ")) != NULL; p++)
        last = p;
    if (!last)
        die("%s holds no EMBTRACE dump (did the program call "
            "embtrace_dump(), and does embtrace_putc reach the console?)",
            path);
    unsigned long long ps;
    if (sscanf(last, "EMBTRACE 1 %llx %llx %llx %llx", &ps, &g_cap,
               &g_recorded, &g_hz) != 4)
        die("%s: the dump's first line is not EmbTrace's", path);
    char *p = strchr(last, '\n');
    int ended = 0;
    while (p && *++p) {
        char *eol = strchr(p, '\n');
        if (!strncmp(p, "EMBTRACE END", 12)) { ended = 1; break; }
        unsigned k;
        unsigned long long fn, site, t;
        /* a console may add a carriage return, or interleave a line of
         * its own: only lines of the event form are events */
        if (sscanf(p, "%x %llx %llx %llx", &k, &fn, &site, &t) == 4 &&
            (k == 1 || k == 2)) {
            g_ev = xrealloc(g_ev, (size_t)(g_nev + 1) * sizeof *g_ev);
            g_ev[g_nev].kind = (int)k;
            g_ev[g_nev].fn = fn;
            g_ev[g_nev].site = site;
            g_ev[g_nev].t = t;
            g_nev++;
        }
        p = eol;
    }
    if (!ended)
        die("%s: the last dump has no EMBTRACE END (cut off?)", path);
}

/* ==== the replay ========================================================== */

struct fstat { u64 fn, calls, total, self, longest; };
static struct fstat *g_st;
static int g_nst;

static struct fstat *stat_of(u64 fn)
{
    for (int k = 0; k < g_nst; k++)
        if (g_st[k].fn == fn) return &g_st[k];
    g_st = xrealloc(g_st, (size_t)(g_nst + 1) * sizeof *g_st);
    memset(&g_st[g_nst], 0, sizeof *g_st);
    g_st[g_nst].fn = fn;
    return &g_st[g_nst++];
}

/* the tree: a node per distinct path */
struct node {
    u64 fn, calls, total;
    int parent, child, sibling;
};
static struct node *g_node;
static int g_nnode;

static int child_of(int parent, u64 fn)
{
    /* the roots are a chain of siblings from node 0, the first one made */
    int k = parent < 0 ? (g_nnode ? 0 : -1) : g_node[parent].child;
    for (; k >= 0; k = g_node[k].sibling)
        if (g_node[k].fn == fn) return k;
    g_node = xrealloc(g_node, (size_t)(g_nnode + 1) * sizeof *g_node);
    struct node *n = &g_node[g_nnode];
    memset(n, 0, sizeof *n);
    n->fn = fn;
    n->parent = parent;
    n->child = -1;
    n->sibling = -1;
    /* appended, so the tree prints in the order of the first calls */
    int *link = parent >= 0 ? &g_node[parent].child : NULL;
    if (link && *link < 0) {
        *link = g_nnode;
    } else if (link || g_nnode) {
        int r = link ? *link : 0;       /* the roots' chain starts at 0 */
        while (g_node[r].sibling >= 0) r = g_node[r].sibling;
        g_node[r].sibling = g_nnode;
    }
    return g_nnode++;
}

struct frame { u64 fn, t0, child_time; int node; };

static int g_lost_exits, g_open, g_maxdepth, g_noclock;
/* entries whose call site is not in the function the stack says called
 * them -- 0 unless the hooks' second argument is wrong (or an interrupt,
 * whose "caller" is whatever it interrupted) */
static int g_sites_checked, g_sites_wrong, g_sites_untraced;

/* does any event name the function holding address a? (one that does not
 * was not instrumented, and its calls' sites are in it, not in the caller
 * the stack shows) */
static int traced(const struct fsym *f)
{
    for (int k = 0; f && k < g_nev; k++)
        if (sym_at(g_ev[k].fn) == f)
            return 1;
    return 0;
}

static void replay(FILE *chrome)
{
    struct frame *st = NULL;
    int sp = 0, cap = 0, first = 1;
    double scale = g_hz ? 1e6 / (double)g_hz : 1.0;
    int noclock = 1;
    for (int k = 0; k < g_nev; k++)
        if (g_ev[k].t) noclock = 0;
    g_noclock = noclock && g_nev;
    for (int k = 0; k < g_nev; k++) {
        const struct ev *e = &g_ev[k];
        u64 t = noclock ? (u64)k : e->t;
        char b1[32];
        if (e->kind == 1) {
            if (sp) {
                const struct fsym *caller = sym_at(st[sp - 1].fn);
                const struct fsym *at = sym_at(e->site);
                if (at && at != caller && !traced(at)) {
                    g_sites_untraced++;
                } else {
                    g_sites_checked++;
                    if (!caller || at != caller)
                        g_sites_wrong++;
                }
            }
            if (sp == cap) st = xrealloc(st, (size_t)(cap = cap * 2 + 16) * sizeof *st);
            st[sp].fn = e->fn;
            st[sp].t0 = t;
            st[sp].child_time = 0;
            st[sp].node = child_of(sp ? st[sp - 1].node : -1, e->fn);
            sp++;
            if (sp > g_maxdepth) g_maxdepth = sp;
            if (chrome) {
                fprintf(chrome, "%s\n{\"name\":\"%s\",\"ph\":\"B\",\"ts\":%.3f,"
                                "\"pid\":1,\"tid\":1}", first ? "" : ",",
                        name_of(e->fn, b1, sizeof b1), (double)t * scale);
                first = 0;
            }
            continue;
        }
        /* an exit: the innermost frame of that function; the frames above
         * it lost their exits (the ring cannot lose an exit while keeping
         * a later one, but a longjmp leaves frames this way) */
        int at = sp - 1;
        while (at >= 0 && st[at].fn != e->fn) at--;
        if (at < 0) {
            g_lost_exits++;
            continue;
        }
        while (sp - 1 > at) sp--;
        struct frame *f = &st[--sp];
        u64 dur = t - f->t0;
        struct fstat *s = stat_of(f->fn);
        s->calls++;
        s->total += dur;
        s->self += dur - f->child_time;
        if (dur > s->longest) s->longest = dur;
        g_node[f->node].calls++;
        g_node[f->node].total += dur;
        if (sp) st[sp - 1].child_time += dur;
        if (chrome) {
            fprintf(chrome, "%s\n{\"name\":\"%s\",\"ph\":\"E\",\"ts\":%.3f,"
                            "\"pid\":1,\"tid\":1}", first ? "" : ",",
                    name_of(e->fn, b1, sizeof b1), (double)t * scale);
            first = 0;
        }
    }
    g_open = sp;
    free(st);
}

static int by_total(const void *a, const void *b)
{
    const struct fstat *x = a, *y = b;
    if (x->total != y->total) return x->total < y->total ? 1 : -1;
    if (x->calls != y->calls) return x->calls < y->calls ? 1 : -1;
    return x->fn < y->fn ? -1 : x->fn > y->fn;
}

static void print_tree(int k, int depth, int maxdepth)
{
    for (; k >= 0; k = g_node[k].sibling) {
        char b[32];
        if (g_node[k].calls || g_node[k].child >= 0) {
            printf("%*s%s  x%llu", 2 * depth, "",
                   name_of(g_node[k].fn, b, sizeof b), g_node[k].calls);
            if (g_hz || g_node[k].total)
                printf("  %llu", g_node[k].total);
            putchar('\n');
        }
        if (depth + 1 < maxdepth)
            print_tree(g_node[k].child, depth + 1, maxdepth);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: embtrace IMAGE.elf LOG [--tree] [--depth N] "
                    "[--chrome OUT.json]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *image = NULL, *log = NULL, *chrome = NULL;
    int tree = 0, depth = 64;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tree")) tree = 1;
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc)
            depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chrome") && i + 1 < argc)
            chrome = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) usage();
        else if (!image) image = argv[i];
        else if (!log) log = argv[i];
        else usage();
    }
    if (!image || !log)
        usage();
    load_elf(image);
    load_log(log);
    FILE *cf = NULL;
    if (chrome) {
        cf = fopen(chrome, "w");
        if (!cf) die("cannot write %s", chrome);
        fputs("{\"traceEvents\":[", cf);
    }
    replay(cf);
    if (cf) {
        fputs("\n]}\n", cf);
        fclose(cf);
    }
    u64 lost = g_recorded > g_cap ? g_recorded - g_cap : 0;
    printf("%d events (%llu recorded, %llu lost to the ring)", g_nev,
           g_recorded, lost);
    if (g_hz) printf(", clock %llu Hz", g_hz);
    if (g_noclock) printf(", no clock (times are counts of events)");
    printf("; deepest %d\n", g_maxdepth);
    if (g_lost_exits || g_open)
        printf("%d exits without their entry, %d calls still open\n",
               g_lost_exits, g_open);
    if (g_sites_checked || g_sites_untraced)
        printf("%d of %d call sites in their caller, %d in a function not "
               "traced\n", g_sites_checked - g_sites_wrong, g_sites_checked,
               g_sites_untraced);
    if (tree) {
        print_tree(g_nnode ? 0 : -1, 0, depth);
        return 0;
    }
    qsort(g_st, (size_t)g_nst, sizeof *g_st, by_total);
    printf("%-32s %10s %12s %12s %12s\n", "function", "calls", "total", "self",
           "longest");
    for (int k = 0; k < g_nst; k++) {
        char b[32];
        printf("%-32s %10llu %12llu %12llu %12llu\n",
               name_of(g_st[k].fn, b, sizeof b), g_st[k].calls,
               g_st[k].total, g_st[k].self, g_st[k].longest);
    }
    return 0;
}
