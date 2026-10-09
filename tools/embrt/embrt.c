/* embrt -- how much stack a firmware can need, proved from what the
 * compiler and the objects say rather than measured on a board.
 *
 *   embrt OBJ.o... [--entry NAME]... [--isr NAME]... [--isr-frame BYTES]
 *                  [--nested] [--max-stack SIZE] [--json]
 *
 * For each object it reads three things:
 *
 *   OBJ.su  the frame of every function the compiler emitted
 *           (-fstack-usage; EmbCC's and GCC's format alike),
 *   OBJ.ci  the calls the compiler left after optimisation, and the calls
 *           through a pointer (-fcallgraph-info=su; GCC's format, which
 *           EmbCC writes too) -- optional, but without it a call through
 *           a pointer is invisible,
 *   OBJ.o   its relocations: every call the object makes to a function in
 *           another section is one, including the calls a backend makes to
 *           a run-time helper for an operation the target lacks (a
 *           soft-float add, a 64-bit divide), which no call graph from the
 *           source shows.
 *
 * From the graph it computes each entry point's worst case -- its frame
 * plus the deepest of its callees', all the way down -- and the path that
 * reaches it. What makes a bound unknowable is reported by name rather
 * than guessed: a recursive cycle, a frame that grows at run time (a
 * variable-length array: "dynamic"), a callee no object provides a frame
 * for. A call through a pointer is assumed to reach any function whose
 * address the program takes -- sound, and named in the report.
 *
 * Interrupts (--isr) add to the main entry's worst case: the deepest
 * handler, or every handler at once with --nested (each preempting the
 * last), plus --isr-frame bytes the hardware pushes for each (32 on a
 * Cortex-M without an FPU, 104 with lazy FPU stacking).
 *
 * ISO C and standalone. docs/manual/tools/embrt.md is the reference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "embrt: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    exit(2);
}

static void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s);
    if (!p)
        die("%s", "out of memory");
    return p;
}

static char *dupstr(const char *s)
{
    char *d = xcalloc(strlen(s) + 1, 1);
    memcpy(d, s, strlen(s));
    return d;
}

/* ---- the graph -------------------------------------------------------- */

enum { UNKNOWN = -1 };

struct fn {
    char *name;
    int obj;                    /* the object defining it; -1 for none */
    int local;                  /* file-scope static: matched within obj */
    int frame;                  /* bytes, or UNKNOWN */
    int dynamic;                /* the frame grows at run time */
    int indirect;               /* makes a call through a pointer */
    int addr_taken;             /* its address is stored somewhere */
    int *callee, ncallee, capcallee;
    /* the walk */
    int state;                  /* 0 new, 1 on the stack, 2 done */
    long long worst;            /* -1: unbounded */
    int next;                   /* the callee on the worst path, or -1 */
    const char *why;            /* why unbounded */
};

static struct fn *g_fn;
static int g_nfn, g_capfn;

static int fn_find(const char *name, int obj, int want_local)
{
    for (int k = 0; k < g_nfn; k++)
        if (!strcmp(g_fn[k].name, name) &&
            (want_local ? g_fn[k].local && g_fn[k].obj == obj
                        : !g_fn[k].local))
            return k;
    return -1;
}

static int fn_add(const char *name, int obj, int local)
{
    if (g_nfn == g_capfn) {
        g_capfn = g_capfn ? g_capfn * 2 : 256;
        g_fn = realloc(g_fn, (size_t)g_capfn * sizeof *g_fn);
        if (!g_fn)
            die("%s", "out of memory");
    }
    struct fn *f = &g_fn[g_nfn];
    memset(f, 0, sizeof *f);
    f->name = dupstr(name);
    f->obj = obj;
    f->local = local;
    f->frame = UNKNOWN;
    f->worst = -1;
    f->next = -1;
    return g_nfn++;
}

/* The function a name means from inside object obj: its own static of
 * that name first, then the global one (made if nobody defines it yet). */
static int fn_ref(const char *name, int obj)
{
    int k = fn_find(name, obj, 1);
    if (k >= 0)
        return k;
    k = fn_find(name, -1, 0);
    return k >= 0 ? k : fn_add(name, -1, 0);
}

static void edge(int from, int to)
{
    struct fn *f = &g_fn[from];
    for (int k = 0; k < f->ncallee; k++)
        if (f->callee[k] == to)
            return;
    if (f->ncallee == f->capcallee) {
        f->capcallee = f->capcallee ? f->capcallee * 2 : 8;
        f->callee = realloc(f->callee, (size_t)f->capcallee * sizeof(int));
        if (!f->callee)
            die("%s", "out of memory");
    }
    f->callee[f->ncallee++] = to;
}

/* ---- reading an object ------------------------------------------------ */

static unsigned char *g_buf;
static size_t g_len;
static int g_be, g_is64;

static u64 rd(size_t off, int n)
{
    u64 v = 0;
    if (off + (size_t)n > g_len || off + (size_t)n < off)
        die("%s: truncated ELF file", "object");
    for (int k = 0; k < n; k++)
        v = (v << 8) | g_buf[off + (size_t)(g_be ? k : n - 1 - k)];
    return v;
}

static unsigned char *slurp(const char *path, size_t *len, int must)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (must)
            die("cannot open %s", path);
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    unsigned char *p = xcalloc(cap + 1, 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            p = realloc(p, cap + 1);
            if (!p)
                die("%s", "out of memory");
        }
        size_t got = fread(p + n, 1, cap - n, f);
        if (got == 0)
            break;
        n += got;
    }
    fclose(f);
    p[n] = 0;
    *len = n;
    return p;
}

/* Is relocation type t of machine m a call (or a tail-call jump)? The
 * psABI numbers, per machine. A jump to another function counts as a call:
 * that it reuses the caller's frame only makes the bound looser, never
 * wrong. */
static int is_call_reloc(unsigned m, unsigned t)
{
    switch (m) {
    case 40:    /* EM_ARM: CALL, JUMP24, THM_CALL, THM_JUMP24, THM_JUMP19 */
        return t == 28 || t == 29 || t == 10 || t == 30 || t == 51;
    case 183:   /* EM_AARCH64: JUMP26, CALL26 */
        return t == 282 || t == 283;
    case 243:   /* EM_RISCV: JAL, CALL, CALL_PLT */
        return t == 17 || t == 18 || t == 19;
    case 8:     /* EM_MIPS: 26 */
        return t == 4;
    case 83:    /* EM_AVR: CALL */
        return t == 18;
    case 62:    /* EM_X86_64: PLT32 */
        return t == 4;
    case 258:   /* EM_LOONGARCH: B26, CALL36 */
        return t == 66 || t == 110;
    case 44:    /* EM_TRICORE: 24REL */
        return t == 2;
    default:
        return 0;
    }
}

struct osym { char *name; unsigned shndx; u64 value, size; int type, bind; };

static void read_object(const char *path, int obj)
{
    g_buf = slurp(path, &g_len, 1);
    if (g_len < 52 || memcmp(g_buf, "\177ELF", 4) != 0)
        die("%s: not an ELF file", path);
    g_is64 = g_buf[4] == 2;
    g_be = g_buf[5] == 2;
    if (rd(16, 2) != 1)
        die("%s: not a relocatable object (give embrt the .o files, which "
            "keep their relocations)", path);
    int w = g_is64 ? 8 : 4;
    unsigned machine = (unsigned)rd(18, 2);
    u64 shoff = rd(g_is64 ? 40 : 32, w);
    unsigned shentsize = (unsigned)rd(g_is64 ? 58 : 46, 2);
    unsigned shnum = (unsigned)rd(g_is64 ? 60 : 48, 2);
    struct osym *sy = NULL;
    u64 nsy = 0;
    unsigned symsec = 0;
    /* the symbol table */
    for (unsigned k = 0; k < shnum; k++) {
        size_t o = (size_t)shoff + (size_t)k * shentsize;
        if (rd(o + 4, 4) != 2)
            continue;
        symsec = k;
        u64 off = rd(o + (g_is64 ? 24 : 16), w);
        u64 size = rd(o + (g_is64 ? 32 : 20), w);
        unsigned link = (unsigned)rd(o + (g_is64 ? 40 : 24), 4);
        u64 ent = rd(o + (g_is64 ? 56 : 36), w);
        size_t lo = (size_t)shoff + (size_t)link * shentsize;
        u64 stroff = rd(lo + (g_is64 ? 24 : 16), w);
        if (!ent)
            ent = (u64)(g_is64 ? 24 : 16);
        nsy = size / ent;
        sy = xcalloc((size_t)nsy, sizeof *sy);
        for (u64 i = 0; i < nsy; i++) {
            size_t so = (size_t)(off + i * ent);
            unsigned name = (unsigned)rd(so, 4);
            unsigned info = (unsigned)rd(so + (g_is64 ? 4 : 12), 1);
            size_t no = (size_t)(stroff + name);
            sy[i].name = dupstr(no < g_len ? (const char *)g_buf + no : "");
            sy[i].shndx = (unsigned)rd(so + (g_is64 ? 6 : 14), 2);
            sy[i].value = rd(so + (g_is64 ? 8 : 4), w);
            sy[i].size = rd(so + (g_is64 ? 16 : 8), w);
            sy[i].type = (int)(info & 15);
            sy[i].bind = (int)(info >> 4);
            if (machine == 40 && sy[i].type == 2)
                sy[i].value &= ~(u64)1;     /* a Thumb function's address */
        }
        break;
    }
    if (!sy)
        die("%s: no symbol table", path);
    /* the functions it defines */
    for (u64 i = 1; i < nsy; i++)
        if (sy[i].type == 2 && sy[i].shndx != 0 && sy[i].shndx < 0xff00) {
            int local = sy[i].bind == 0;
            int k = local ? fn_find(sy[i].name, obj, 1)
                          : fn_find(sy[i].name, -1, 0);
            if (k < 0)
                k = fn_add(sy[i].name, obj, local);
            g_fn[k].obj = obj;
        }
    /* the relocations: calls, and addresses taken */
    for (unsigned k = 0; k < shnum; k++) {
        size_t o = (size_t)shoff + (size_t)k * shentsize;
        unsigned type = (unsigned)rd(o + 4, 4);
        if (type != 4 && type != 9)     /* SHT_RELA, SHT_REL */
            continue;
        if ((unsigned)rd(o + (g_is64 ? 40 : 24), 4) != symsec)
            continue;
        unsigned target = (unsigned)rd(o + (g_is64 ? 44 : 28), 4);
        u64 off = rd(o + (g_is64 ? 24 : 16), w);
        u64 size = rd(o + (g_is64 ? 32 : 20), w);
        u64 ent = rd(o + (g_is64 ? 56 : 36), w);
        if (!ent)
            ent = (u64)(type == 4 ? (g_is64 ? 24 : 12) : (g_is64 ? 16 : 8));
        for (u64 i = 0; i < size / ent; i++) {
            size_t ro = (size_t)(off + i * ent);
            u64 where = rd(ro, w), info = rd(ro + (size_t)w, w);
            long long addend = type == 4 ? (long long)rd(ro + 2 * (size_t)w, w) : 0;
            unsigned rt = (unsigned)(g_is64 ? info & 0xffffffffu : info & 0xff);
            u64 si = g_is64 ? info >> 32 : info >> 8;
            if (si == 0 || si >= nsy)
                continue;
            if (!g_is64 && w == 4 && type == 4)
                addend = (long long)(int)addend;
            /* what the relocation names: a function symbol, or a section
             * symbol plus an offset into the function there */
            const struct osym *t = &sy[si];
            const char *tname = NULL;
            int tlocal = 0;
            if (t->type == 2) {
                tname = t->name;
                tlocal = t->bind == 0;
            } else if (t->type == 3 || t->type == 0) {
                if (t->type == 0 && t->shndx == 0) {
                    tname = t->name;    /* undefined: another object's */
                } else {
                    for (u64 j = 1; j < nsy; j++)
                        if (sy[j].type == 2 && sy[j].shndx == t->shndx &&
                            (u64)addend + t->value >= sy[j].value &&
                            (u64)addend + t->value < sy[j].value + (sy[j].size ? sy[j].size : 1)) {
                            tname = sy[j].name;
                            tlocal = sy[j].bind == 0;
                            break;
                        }
                }
            }
            if (!tname || !*tname)
                continue;
            int to = tlocal ? fn_find(tname, obj, 1) : -1;
            if (to < 0)
                to = fn_ref(tname, obj);
            if (is_call_reloc(machine, rt)) {
                /* the caller: the function whose bytes hold the site */
                for (u64 j = 1; j < nsy; j++)
                    if (sy[j].type == 2 && sy[j].shndx == target &&
                        where >= sy[j].value &&
                        where < sy[j].value + sy[j].size) {
                        int from = sy[j].bind == 0
                                 ? fn_find(sy[j].name, obj, 1)
                                 : fn_find(sy[j].name, -1, 0);
                        if (from >= 0 && from != to)
                            edge(from, to);
                        else if (from >= 0)
                            edge(from, to);
                        break;
                    }
            } else {
                g_fn[to].addr_taken = 1;
            }
        }
    }
    for (u64 i = 0; i < nsy; i++)
        free(sy[i].name);
    free(sy);
    free(g_buf);
    g_buf = NULL;
}

/* OBJ.su beside OBJ.o: "file:line[:col]:name<TAB>bytes<TAB>qualifier" */
static int read_su(const char *path, int obj)
{
    size_t n;
    char *p = (char *)slurp(path, &n, 0);
    if (!p)
        return 0;
    for (char *line = strtok(p, "\n"); line; line = strtok(NULL, "\n")) {
        char *t1 = strchr(line, '\t');
        if (!t1)
            continue;
        *t1 = 0;
        char *name = strrchr(line, ':');
        name = name ? name + 1 : line;
        int bytes = atoi(t1 + 1);
        char *t2 = strchr(t1 + 1, '\t');
        const char *q = t2 ? t2 + 1 : "static";
        int k = fn_find(name, obj, 1);
        if (k < 0)
            k = fn_find(name, -1, 0);
        if (k < 0)
            k = fn_add(name, obj, 0);
        g_fn[k].frame = bytes;
        /* "dynamic,bounded" is GCC's word for a frame that varies but
         * whose bytes ARE the bound; plain "dynamic" is unbounded */
        g_fn[k].dynamic = !strncmp(q, "dynamic", 7) && !strstr(q, "bounded");
    }
    free(p);
    return 1;
}

/* OBJ.ci: the edges, and which functions call through a pointer */
static void read_ci(const char *path, int obj)
{
    size_t n;
    char *p = (char *)slurp(path, &n, 0);
    if (!p)
        return;
    for (char *line = strtok(p, "\n"); line; line = strtok(NULL, "\n")) {
        char src[512], dst[512];
        const char *s = strstr(line, "sourcename: \"");
        const char *d = strstr(line, "targetname: \"");
        if (strncmp(line, "edge:", 5) || !s || !d)
            continue;
        if (sscanf(s, "sourcename: \"%511[^\"]\"", src) != 1 ||
            sscanf(d, "targetname: \"%511[^\"]\"", dst) != 1)
            continue;
        int from = fn_find(src, obj, 1);
        if (from < 0)
            from = fn_ref(src, obj);
        if (!strcmp(dst, "__indirect_call"))
            g_fn[from].indirect = 1;
        else
            edge(from, fn_ref(dst, obj));
    }
    free(p);
}

/* ---- the worst case --------------------------------------------------- */

static int g_nindirect_targets;

/* The deepest stack below f, f's frame included; -1 when it cannot be
 * bounded, with the reason in f->why. A call through a pointer may reach
 * any function whose address is taken. */
static long long worst(int fi)
{
    struct fn *f = &g_fn[fi];
    if (f->state == 2)
        return f->worst;
    if (f->state == 1) {
        f->why = "recursion";
        return -2;                  /* a cycle: the caller marks it */
    }
    f->state = 1;
    long long best = 0;
    int bad = 0;
    if (f->frame == UNKNOWN) {
        f->why = f->obj < 0 ? "no object defines it (a library without "
                              "-fstack-usage?)"
                            : "no frame size (compile it with -fstack-usage)";
        bad = 1;
    } else if (f->dynamic) {
        f->why = "its frame grows at run time (a variable-length array or "
                 "alloca)";
        bad = 1;
    }
    int ncall = f->ncallee;
    for (int k = 0; k < ncall + (f->indirect ? g_nfn : 0) && !bad; k++) {
        int c;
        if (k < ncall) {
            c = f->callee[k];
        } else {
            c = k - ncall;
            if (!g_fn[c].addr_taken)
                continue;
        }
        long long w = worst(c);
        if (w == -2 || (w < 0 && g_fn[c].state == 1)) {
            f->why = "recursion";
            bad = 1;
            f->next = c;
        } else if (w < 0) {
            f->why = g_fn[c].why;
            f->next = c;
            bad = 1;
        } else if (w > best) {
            best = w;
            f->next = c;
        }
    }
    f->state = 2;
    f->worst = bad ? -1 : best + f->frame;
    if (bad && f->next < 0)
        f->next = -1;
    return f->worst;
}

/* ---- main ------------------------------------------------------------- */

static int g_json;

/* Where the worst path from fi, followed through `next`, comes back to a
 * function already on it: that function's place on the path (0 is fi),
 * or -1. A recursive cycle's `next` links go round it for ever, so this
 * is where a printed path stops -- it printed `rec` 63 times. *at says
 * which step closes it: the path's at-th function calls the one returned. */
static int path_cycle(int fi, int *at)
{
    int path[64], n = 0;
    for (int k = fi; k >= 0 && n < 64; k = g_fn[k].next) {
        for (int j = 0; j < n; j++)
            if (path[j] == k) {
                *at = n - 1;
                return j;
            }
        path[n++] = k;
    }
    return -1;
}

/* The worst path from an entry, one line per function: its frame and
 * what is notable about it; where the bound is lost, why. A recursive
 * cycle is printed once, and named as one. */
static void path_text(int fi)
{
    int at = -1, back = path_cycle(fi, &at);
    for (int k = fi, n = 0; k >= 0 && n < 64; n++) {
        const struct fn *f = &g_fn[k];
        char fr[32];
        if (f->frame == UNKNOWN)
            snprintf(fr, sizeof fr, "?");
        else
            snprintf(fr, sizeof fr, "%d", f->frame);
        printf("    %s%-30s %6s bytes%s%s\n", n ? "-> " : "   ", f->name, fr,
               f->dynamic ? " (dynamic)" : "",
               f->indirect ? " (calls through a pointer)" : "");
        int nx = f->next;
        /* the cycle closes here: say which call, and the whole cycle */
        if (back >= 0 && n == at) {
            int j = fi;
            printf("       unbounded here: recursion (%s calls %s%s",
                   f->name, nx == k ? "itself" : g_fn[nx].name,
                   nx == k ? ")\n" : ": ");
            if (nx != k) {
                for (int q = 0; q < back; q++)
                    j = g_fn[j].next;
                for (int q = back; q <= at; q++, j = g_fn[j].next)
                    printf("%s -> ", g_fn[j].name);
                printf("%s)\n", g_fn[nx].name);
            }
            break;
        }
        /* stop where the bound is lost at this function itself */
        if (f->worst < 0 && (nx < 0 || g_fn[nx].worst >= 0 ||
                             f->why != g_fn[nx].why)) {
            printf("       unbounded here: %s%s%s\n", f->why ? f->why : "?",
                   nx >= 0 && f->why && !strcmp(f->why, "recursion")
                       ? " through " : "",
                   nx >= 0 && f->why && !strcmp(f->why, "recursion")
                       ? g_fn[nx].name : "");
            break;
        }
        k = nx;
    }
}

static void json_name(const char *s)
{
    putchar('"');
    for (; *s; s++)
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else
            putchar(*s);
    putchar('"');
}

int main(int argc, char **argv)
{
    char **objs = xcalloc((size_t)argc, sizeof *objs);
    char **entries = xcalloc((size_t)argc, sizeof *entries);
    char **isrs = xcalloc((size_t)argc, sizeof *isrs);
    int nobj = 0, nent = 0, nisr = 0, nested = 0, isr_frame = 0;
    long long max_stack = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--entry") && i + 1 < argc)
            entries[nent++] = argv[++i];
        else if (!strcmp(a, "--isr") && i + 1 < argc)
            isrs[nisr++] = argv[++i];
        else if (!strcmp(a, "--isr-frame") && i + 1 < argc)
            isr_frame = atoi(argv[++i]);
        else if (!strcmp(a, "--nested"))
            nested = 1;
        else if (!strcmp(a, "--max-stack") && i + 1 < argc) {
            char *e;
            max_stack = strtoll(argv[++i], &e, 0);
            if (*e == 'K' || *e == 'k') { max_stack <<= 10; e++; }
            if (*e || max_stack < 0)
                die("--max-stack %s: a size in bytes (e.g. 2K)", argv[i]);
        } else if (!strcmp(a, "--json"))
            g_json = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            fprintf(stderr, "usage: embrt OBJ.o... [--entry NAME]... "
                    "[--isr NAME]... [--isr-frame BYTES] [--nested]\n"
                    "             [--max-stack SIZE] [--json]\n");
            return 2;
        } else if (a[0] == '-')
            die("unknown option %s (embrt --help)", a);
        else
            objs[nobj++] = argv[i];
    }
    if (!nobj)
        die("%s", "no objects: embrt OBJ.o... (each with OBJ.su beside it)");
    if (!nent)
        entries[nent++] = "main";

    int missing_su = 0;
    for (int k = 0; k < nobj; k++) {
        read_object(objs[k], k);
        char *base = dupstr(objs[k]), *dot = strrchr(base, '.');
        char *alt = xcalloc(strlen(base) + 8, 1);
        if (dot && !strchr(dot, '/'))
            *dot = 0;
        sprintf(alt, "%s.su", base);
        if (!read_su(alt, k)) {
            fprintf(stderr, "embrt: %s has no %s beside it (compile with "
                    "-fstack-usage)\n", objs[k], alt);
            missing_su = 1;
        }
        sprintf(alt, "%s.ci", base);
        read_ci(alt, k);
        free(alt);
        free(base);
    }
    for (int k = 0; k < g_nfn; k++)
        g_nindirect_targets += g_fn[k].addr_taken;

    int status = missing_su ? 1 : 0;
    long long main_worst = 0, isr_worst = 0, isr_sum = 0;
    int unbounded = 0;
    if (g_json)
        printf("{\n  \"entries\": [");
    for (int e = 0; e < nent + nisr; e++) {
        const char *name = e < nent ? entries[e] : isrs[e - nent];
        int fi = fn_find(name, -1, 0);
        if (fi < 0)
            for (int k = 0; k < g_nfn && fi < 0; k++)
                if (!strcmp(g_fn[k].name, name))
                    fi = k;
        if (fi < 0 || g_fn[fi].obj < 0)
            die("entry %s: no object defines it", name);
        long long w = worst(fi);
        if (w < 0)
            unbounded = 1;
        else if (e < nent) {
            if (w > main_worst)
                main_worst = w;
        } else {
            if (w + isr_frame > isr_worst)
                isr_worst = w + isr_frame;
            isr_sum += w + isr_frame;
        }
        if (g_json) {
            printf("%s\n    {\"name\": ", e ? "," : "");
            json_name(name);
            printf(", \"kind\": \"%s\", \"bytes\": %lld",
                   e < nent ? "entry" : "isr", w);
            if (w < 0 && g_fn[fi].why) {
                printf(", \"unbounded\": ");
                json_name(g_fn[fi].why);
            }
            /* each function once: a recursive cycle ends the path where
             * it closes, and "recursion" names the function it calls
             * back into */
            int at = -1, back = path_cycle(fi, &at);
            printf(", \"path\": [");
            for (int k = fi, n = 0; k >= 0 && n < 64; k = g_fn[k].next, n++) {
                printf("%s{\"name\": ", n ? ", " : "");
                json_name(g_fn[k].name);
                printf(", \"frame\": %d}", g_fn[k].frame);
                if (back >= 0 && n == at)
                    break;
                if (g_fn[k].worst < 0 && g_fn[k].next >= 0 &&
                    g_fn[g_fn[k].next].state != 2)
                    break;
            }
            printf("]");
            if (back >= 0) {
                int j = fi;
                for (int q = 0; q < back; q++)
                    j = g_fn[j].next;
                printf(", \"recursion\": ");
                json_name(g_fn[j].name);
            }
            printf("}");
        } else {
            if (w >= 0)
                printf("%s %s: %lld bytes at most\n",
                       e < nent ? "entry" : "interrupt", name, w);
            else
                printf("%s %s: no bound -- %s\n",
                       e < nent ? "entry" : "interrupt", name,
                       g_fn[fi].why ? g_fn[fi].why : "unknown");
            path_text(fi);
        }
    }
    long long total = main_worst + (nested ? isr_sum : isr_worst);
    if (g_json) {
        printf("\n  ],\n  \"indirect_targets\": %d", g_nindirect_targets);
        if (!unbounded)
            printf(",\n  \"total\": %lld", total);
        printf("\n}\n");
    } else {
        if (g_nindirect_targets)
            for (int k = 0; k < g_nfn; k++)
                if (g_fn[k].indirect && g_fn[k].state == 2) {
                    printf("\na call through a pointer is taken to reach "
                           "any of the %d functions whose address the "
                           "program takes\n", g_nindirect_targets);
                    break;
                }
        if (nisr && !unbounded)
            printf("\nworst case with interrupts%s: %lld bytes (%lld for "
                   "the entries, %lld for the handlers, %d per handler "
                   "for the hardware's frame)\n",
                   nested ? " nesting" : "", total, main_worst,
                   nested ? isr_sum : isr_worst, isr_frame);
    }
    if (unbounded) {
        fprintf(stderr, "embrt: no bound for every entry (see the report)\n");
        status = 1;
    } else if (max_stack >= 0 && total > max_stack) {
        fprintf(stderr, "embrt: the worst case is %lld bytes and the stack "
                "is %lld (--max-stack)\n", total, max_stack);
        status = 1;
    }
    return status;
}
