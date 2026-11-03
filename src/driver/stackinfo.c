/* --stack-info and --stack-report: a whole program's worst-case stack and
 * cycles.
 *
 * --stack-info puts in each object a section .debug_embcc_stack (a debug
 * section, so a link keeps it, every linker concatenating the units' and
 * no loader loading it), one block per unit:
 *
 *   "ESTK" u32 version u32 block_bytes u32 nfunc u32 target u32 pool_off
 *   u32 unit_name (an offset into the pool)
 *   per function: u32 name, u32 flags, u32 frame, u32 cycles_lo,
 *     u32 cycles_hi, u32 unbounded_line, u32 signature, u32 ncall,
 *     u32 nicall; then ncall x {u32 name, u32 flags, u32 times} and
 *     nicall x {u32 signature, u32 times}
 *   the pool: NUL-terminated names
 *
 * all little-endian. A function's flags: 1 static, 2 an interrupt handler,
 * 4 its frame moves at run time (a VLA, alloca), 8 its address is taken,
 * 16 cycles unknown. A callee's: 1 static (this unit's). The calls are
 * the IR's direct ones and every call the backend made that the IR does
 * not show -- a run-time helper -- read from the function's relocations;
 * an indirect call is its signature (the parameters' classes), matched
 * at report time against the functions whose address is taken. `times`
 * is how often the call can run per call of the caller: its loops' trip
 * counts (src/opt/wcet.c), 0xffffffff when one is unknown.
 *
 * `embcc --stack-report IMAGE` reads the section back from a linked image
 * (or an object), finds every root -- main, Reset_Handler, each handler,
 * anything nothing calls -- and gives each its worst stack (frame plus
 * the deepest callee chain) and cycles (its own plus each call's times
 * the callee's), with the path; a recursion, an indirect call nothing
 * matches, a callee with no record or a loop without a bound makes that
 * root's number unknown, and says why. With interrupts: the worst main
 * path plus the worst handler plus the frame the hardware pushes, which
 * is the bound when handlers do not nest. --stack-limit=N fails the
 * report (exit 1) when that is more than N bytes. */

#include "util.h"
#include "stackinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../parse/ast.h"
#include "../ir/ir.h"
#include "../opt/opt.h"
#include "../arch/target.h"
#include "../arch/code.h"
#include "../elf/write.h"
#include "../platform/platform.h"

#define SI_STATIC 1
#define SI_ISR 2
#define SI_DYNAMIC 4
#define SI_ADDR 8
#define SI_NOCYC 16
#define UNB 0xffffffffu

static int g_si;
void stackinfo_set(int on) { g_si = on; }
int stackinfo_on(void) { return g_si; }

static void put32(struct outbuf *b, unsigned long v)
{
    char w[4] = { (char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) };
    ob_add(b, w, 4);
}

static unsigned pool_add(struct outbuf *pool, const char *s)
{
    unsigned at = (unsigned)pool->n;
    ob_add(pool, s, strlen(s) + 1);
    return at;
}

static unsigned sig_add(unsigned h, unsigned v)
{
    h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

static unsigned arg_sig(const struct ir_arg *a, int n)
{
    unsigned h = sig_add(0, (unsigned)n);
    for (int k = 0; k < n; k++)
        h = sig_add(h, (unsigned)(a[k].is_struct ? 0x1000 : a[k].is_float
                                                         ? 0x2000 : 0) |
                           (unsigned)(a[k].size & 0xfff));
    return h;
}

static int is_handler(const struct func *f)
{
    const char *n = f->name;
    size_t l = strlen(n);
    if (f->is_isr)
        return 1;
    if (l > 11 && !strcmp(n + l - 11, "_IRQHandler"))
        return 1;
    if (l > 8 && !strcmp(n + l - 8, "_Handler") &&
        strcmp(n + (l > 13 ? l - 13 : 0), "Reset_Handler"))
        return 1;
    return !strncmp(n, "__vector_", 9);
}

void stackinfo_emit(struct elfw *w, struct unit *u, struct ir_unit *iu,
                    const struct code *text, int text_ndx,
                    int (*text_at)(long off, long *o),
                    int (*sec_of)(int gi, int text_ndx), const char *src)
{
    if (!g_si)
        return;
    struct outbuf rec = { NULL, 0, 0 }, pool = { NULL, 0, 0 };
    unsigned unit_name = pool_add(&pool, src ? src : "?");
    /* the functions whose address is taken: in code (IR_FADDR) or in data */
    int nf = 0;
    for (int k = 0; k < iu->nfuncs; k++) {
        struct ir_func *fn = &iu->funcs[k];
        struct func *f = fn->src;
        if (!f || f->absorbed || !f->has_defn || f->code_len <= 0)
            continue;
        int addr = 0;
        for (int j = 0; j < iu->nfuncs && !addr; j++)
            for (int n = 0; n < iu->funcs[j].nins && !addr; n++)
                if (iu->funcs[j].ins[n].op == IR_FADDR &&
                    iu->funcs[j].ins[n].callee == f)
                    addr = 1;
        for (struct global *g = u->globals; g && !addr; g = g->next)
            for (int r = 0; !g->absorbed && r < g->nrelocs && !addr; r++)
                if (g->relocs[r].ftarget == f)
                    addr = 1;
        if (!f->is_static)
            addr = addr || 0;
        unsigned long cyc = 0, *mult = xcalloc((size_t)fn->nins + 1,
                                              sizeof *mult);
        int unb_line = 0;
        int have = wcet_func(fn, text->p, (int)f->code_off,
                             (int)(f->code_off + f->code_len), &cyc,
                             &unb_line, mult) == 0;
        unsigned flags = (f->is_static ? SI_STATIC : 0) |
                         (is_handler(f) ? SI_ISR : 0) |
                         (fn->has_alloca ? SI_DYNAMIC : 0) |
                         (addr ? SI_ADDR : 0) |
                         (!have || cyc == UNB ? SI_NOCYC : 0);
        /* the calls: the IR's direct ones with their times, and the
         * relocations of this function's code against other functions
         * the IR does not call or take the address of (the helpers) */
        struct cl { const char *name; unsigned flags; unsigned long times; } *cs = NULL;
        int ncs = 0;
        struct ic { unsigned sig; unsigned long times; } *is = NULL;
        int nis = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_CALL)
                continue;
            if (i->indirect) {
                is = xrealloc(is, (size_t)(nis + 1) * sizeof *is);
                is[nis].sig = arg_sig(i->argv, i->nargs);
                is[nis].times = mult[n];
                nis++;
                continue;
            }
            if (!i->callee)
                continue;
            int dup = -1;
            for (int q = 0; q < ncs; q++)
                if (!strcmp(cs[q].name, i->callee->name)) dup = q;
            if (dup >= 0) {
                cs[dup].times += mult[n];
                continue;
            }
            cs = xrealloc(cs, (size_t)(ncs + 1) * sizeof *cs);
            cs[ncs].name = i->callee->name;
            cs[ncs].flags = i->callee->is_static && i->callee->has_defn
                            ? SI_STATIC : 0;
            cs[ncs].times = mult[n];
            ncs++;
        }
        long o0;
        int gi = text_at(f->code_off, &o0);
        int sec = sec_of(gi, text_ndx);
        int nr = elfw_rela_count(w, sec);
        for (int r = 0; r < nr; r++) {
            unsigned long off;
            int sym, type;
            elfw_rela_at(w, sec, r, &off, &sym, &type);
            if ((long)off < o0 || (long)off >= o0 + f->code_len)
                continue;
            const char *sn = elfw_symbol_name(w, sym);
            int st = elfw_symbol_type(w, sym);
            if (!sn || !*sn || sn[0] == '.' || st == 3 /* STT_SECTION */ ||
                st == 1 /* STT_OBJECT */ || !strcmp(sn, f->name))
                continue;
            int known = 0;
            for (struct global *g = u->globals; g && !known; g = g->next)
                if (!g->absorbed && !strcmp(g->name, sn))
                    known = 1;
            for (int q = 0; q < ncs && !known; q++)
                if (!strcmp(cs[q].name, sn))
                    known = 1;
            for (int n = 0; n < fn->nins && !known; n++)
                if (fn->ins[n].op == IR_FADDR && fn->ins[n].callee &&
                    !strcmp(fn->ins[n].callee->name, sn))
                    known = 1;
            if (known)
                continue;
            /* the times of the instruction whose code holds it */
            unsigned long t = 1;
            if (fn->ins_pc)
                for (int n = 0; n < fn->nins; n++)
                    if (fn->ins_pc[n] >= 0 &&
                        fn->ins_pc[n] <= f->code_off + ((long)off - o0))
                        t = mult[n];
            cs = xrealloc(cs, (size_t)(ncs + 1) * sizeof *cs);
            cs[ncs].name = sn;
            cs[ncs].flags = 0;
            cs[ncs].times = t;
            ncs++;
        }
        put32(&rec, pool_add(&pool, f->name));
        put32(&rec, flags);
        put32(&rec, (unsigned long)(f->stack_bytes > 0 ? f->stack_bytes : 0));
        put32(&rec, cyc & 0xffffffffu);
        put32(&rec, (cyc >> 16) >> 16);
        put32(&rec, (unsigned long)unb_line);
        put32(&rec, fn->param_abi ? arg_sig(fn->param_abi, fn->nparams)
                                  : arg_sig(NULL, 0));
        put32(&rec, (unsigned long)ncs);
        put32(&rec, (unsigned long)nis);
        for (int q = 0; q < ncs; q++) {
            put32(&rec, pool_add(&pool, cs[q].name));
            put32(&rec, cs[q].flags);
            put32(&rec, cs[q].times > UNB ? UNB : cs[q].times);
        }
        for (int q = 0; q < nis; q++) {
            put32(&rec, is[q].sig);
            put32(&rec, is[q].times > UNB ? UNB : is[q].times);
        }
        free(cs);
        free(is);
        free(mult);
        nf++;
    }
    struct outbuf b = { NULL, 0, 0 };
    ob_add(&b, "ESTK", 4);
    put32(&b, 1);
    unsigned long total = 28 + rec.n + pool.n;
    put32(&b, total);
    put32(&b, (unsigned long)nf);
    put32(&b, (unsigned long)target_get());
    put32(&b, 28 + rec.n);
    put32(&b, unit_name);
    ob_add(&b, rec.p ? rec.p : "", rec.n);
    ob_add(&b, pool.p ? pool.p : "", pool.n);
    elfw_add_section(w, ".debug_embcc_stack", 1 /* SHT_PROGBITS */, 0,
                     b.p, (Elf64_Xword)b.n, 1);
    free(rec.p);
    free(pool.p);
}

/* ======================================================================
 * The report
 * ====================================================================== */

struct sfn {
    const char *name;
    int unit;
    unsigned flags, frame, unb_line, sig;
    unsigned long long cycles;
    int ncall, nicall;
    const unsigned char *calls, *icalls;
    const char *pool;
    /* the analysis */
    int state;                  /* 0 new, 1 on the stack, 2 done */
    unsigned long long worst_stack, worst_cycles;
    int stack_known, cycles_known;
    const char *why;
    int next;                   /* the callee on the worst path, or -1 */
    int called;
};

static struct sfn *g_s;
static int g_ns;
static const char *const *g_units;

static unsigned rd(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 |
           (unsigned)p[3] << 24;
}

static int find_fn(const char *name, int unit, int is_static)
{
    if (is_static)
        for (int i = 0; i < g_ns; i++)
            if (g_s[i].unit == unit && !strcmp(g_s[i].name, name))
                return i;
    for (int i = 0; i < g_ns; i++)
        if (!(g_s[i].flags & SI_STATIC) && !strcmp(g_s[i].name, name))
            return i;
    return -1;
}

static char g_why[4096];

static void analyze(int i)
{
    struct sfn *f = &g_s[i];
    if (f->state == 2)
        return;
    if (f->state == 1) {
        f->stack_known = f->cycles_known = 0;
        f->why = "recursive";
        return;
    }
    f->state = 1;
    f->stack_known = !(f->flags & SI_DYNAMIC);
    f->cycles_known = !(f->flags & SI_NOCYC);
    f->why = f->flags & SI_DYNAMIC ? "its frame grows at run time (a VLA "
                                     "or alloca)"
           : f->flags & SI_NOCYC ? "a loop with no bound" : NULL;
    unsigned long long deepest = 0, cyc = f->cycles;
    f->next = -1;
    for (int k = 0; k < f->ncall; k++) {
        const unsigned char *c = f->calls + 12 * k;
        const char *cn = f->pool + rd(c);
        unsigned cf = rd(c + 4), times = rd(c + 8);
        int j = find_fn(cn, f->unit, cf & SI_STATIC);
        if (j < 0) {
            f->stack_known = f->cycles_known = 0;
            static char b[256];
            snprintf(b, sizeof b, "it calls '%s', which has no stack "
                     "information (built without --stack-info?)", cn);
            f->why = xstrndup(b, strlen(b));
            continue;
        }
        g_s[j].called = 1;
        analyze(j);
        if (g_s[i].state == 1 && g_s[j].state == 1) {
            /* j is on the stack: a cycle through i */
            f->stack_known = f->cycles_known = 0;
            snprintf(g_why, sizeof g_why, "recursive (%s calls %s)",
                     f->name, cn);
            f->why = xstrndup(g_why, strlen(g_why));
            continue;
        }
        if (!g_s[j].stack_known) {
            f->stack_known = 0;
            if (!f->why || !strstr(f->why, "recursive"))
                f->why = g_s[j].why;
        }
        if (!g_s[j].cycles_known || times == UNB)
            f->cycles_known = 0;
        if (g_s[j].worst_stack > deepest) {
            deepest = g_s[j].worst_stack;
            f->next = j;
        }
        cyc += (unsigned long long)times * g_s[j].worst_cycles;
    }
    for (int k = 0; k < f->nicall; k++) {
        const unsigned char *c = f->icalls + 8 * k;
        unsigned sig = rd(c), times = rd(c + 4);
        int any = 0;
        for (int j = 0; j < g_ns; j++) {
            if (!(g_s[j].flags & SI_ADDR) || g_s[j].sig != sig)
                continue;
            any = 1;
            g_s[j].called = 1;
            analyze(j);
            if (!g_s[j].stack_known)
                f->stack_known = 0;
            if (!g_s[j].cycles_known || times == UNB)
                f->cycles_known = 0;
            if (g_s[j].worst_stack > deepest) {
                deepest = g_s[j].worst_stack;
                f->next = j;
            }
            unsigned long long cj = (unsigned long long)times *
                                    g_s[j].worst_cycles;
            if (cj > 0)
                cyc += cj;          /* every candidate: an upper bound */
        }
        if (!any) {
            f->stack_known = f->cycles_known = 0;
            f->why = "an indirect call no function of the program matches";
        }
    }
    f->worst_stack = f->frame + deepest;
    f->worst_cycles = cyc;
    f->state = 2;
}

static unsigned char *elf_section(const unsigned char *d, long n,
                                  const char *want, long *len)
{
    if (n < 52 || memcmp(d, "\177ELF", 4))
        return NULL;
    int is64 = d[4] == 2, be = d[5] == 2;
#define R16(p) (be ? (unsigned)((p)[0] << 8 | (p)[1]) : (unsigned)((p)[0] | (p)[1] << 8))
#define R32(p) (be ? ((unsigned long)(p)[0] << 24 | (unsigned long)(p)[1] << 16 | \
                      (unsigned long)(p)[2] << 8 | (p)[3]) : rd(p))
#define R64(p) (be ? (R32(p) << 32 | R32((p) + 4)) : (R32(p) | R32((p) + 4) << 32))
    unsigned long shoff = is64 ? (unsigned long)R64(d + 40) : R32(d + 32);
    unsigned shentsize = R16(d + (is64 ? 58 : 46));
    unsigned shnum = R16(d + (is64 ? 60 : 48)), shstrndx = R16(d + (is64 ? 62 : 50));
    if (!shoff || shoff + (unsigned long)shnum * shentsize > (unsigned long)n ||
        shstrndx >= shnum)
        return NULL;
    const unsigned char *ss = d + shoff + (unsigned long)shstrndx * shentsize;
    unsigned long stroff = is64 ? (unsigned long)R64(ss + 24) : R32(ss + 16);
    for (unsigned k = 0; k < shnum; k++) {
        const unsigned char *s = d + shoff + (unsigned long)k * shentsize;
        unsigned long nm = R32(s);
        unsigned long off = is64 ? (unsigned long)R64(s + 24) : R32(s + 16);
        unsigned long sz = is64 ? (unsigned long)R64(s + 32) : R32(s + 20);
        if (stroff + nm < (unsigned long)n &&
            !strcmp((const char *)d + stroff + nm, want) &&
            off + sz <= (unsigned long)n) {
            *len = (long)sz;
            return (unsigned char *)d + off;
        }
    }
#undef R16
#undef R32
#undef R64
    return NULL;
}

static const char *trap_frame(unsigned target)
{
    switch (target) {
    case TARGET_THUMB: return "32 bytes (Cortex-M's eight-word frame; 104 with the FPU's lazy context)";
    case TARGET_AVR: return "2 bytes (the return address; 3 on a part with a 22-bit PC)";
    default: return "0 bytes (the handler's own prologue saves what it needs)";
    }
}

static unsigned trap_bytes(unsigned target)
{
    return target == TARGET_THUMB ? 32 : target == TARGET_AVR ? 2 : 0;
}

int stackinfo_report(int argc, char **argv)
{
    const char *file = NULL;
    long limit = -1;
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "--stack-limit=", 14))
            limit = strtol(argv[i] + 14, NULL, 0);
        else if (argv[i][0] != '-')
            file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "embcc: error: --stack-report IMAGE: which image\n");
        return 1;
    }
    long n;
    unsigned char *d = (unsigned char *)plat_read_file(file, &n);
    if (!d) {
        fprintf(stderr, "embcc: error: cannot read '%s'\n", file);
        return 1;
    }
    long sl = 0;
    unsigned char *s = elf_section(d, n, ".debug_embcc_stack", &sl);
    if (!s) {
        fprintf(stderr, "embcc: error: '%s' has no stack information: "
                        "compile with --stack-info (and do not strip)\n", file);
        return 1;
    }
    /* the blocks, one per unit */
    int cap = 0, nunits = 0;
    const char **units = NULL;
    unsigned target = 0;
    g_ns = 0;
    for (long at = 0; at + 28 <= sl; ) {
        if (memcmp(s + at, "ESTK", 4)) { at++; continue; }   /* padding */
        unsigned bytes = rd(s + at + 8), nf = rd(s + at + 12);
        target = rd(s + at + 16);
        unsigned pool_off = rd(s + at + 20);
        const char *pool = (const char *)s + at + pool_off;
        units = xrealloc(units, (size_t)(nunits + 1) * sizeof *units);
        units[nunits] = pool + rd(s + at + 24);
        const unsigned char *r = s + at + 28;
        for (unsigned k = 0; k < nf; k++) {
            if (g_ns == cap) {
                cap = cap ? cap * 2 : 256;
                g_s = xrealloc(g_s, (size_t)cap * sizeof *g_s);
            }
            struct sfn *f = &g_s[g_ns++];
            memset(f, 0, sizeof *f);
            f->name = pool + rd(r);
            f->flags = rd(r + 4);
            f->frame = rd(r + 8);
            f->cycles = (unsigned long long)rd(r + 12) |
                        (unsigned long long)rd(r + 16) << 32;
            f->unb_line = rd(r + 20);
            f->sig = rd(r + 24);
            f->ncall = (int)rd(r + 28);
            f->nicall = (int)rd(r + 32);
            f->calls = r + 36;
            f->icalls = f->calls + 12 * f->ncall;
            f->pool = pool;
            f->unit = nunits;
            r = f->icalls + 8 * f->nicall;
        }
        nunits++;
        at += bytes ? bytes : 1;
    }
    g_units = units;
    for (int i = 0; i < g_ns; i++)
        analyze(i);
    /* the roots */
    printf("%-32s %8s %10s %14s  %s\n", "root", "frame", "stack", "cycles",
           "worst stack path");
    unsigned long long worst_main = 0, worst_isr = 0;
    int main_known = 1, isr_known = 1, any_isr = 0;
    for (int i = 0; i < g_ns; i++) {
        struct sfn *f = &g_s[i];
        int root = !strcmp(f->name, "main") || (f->flags & SI_ISR) ||
                   !strcmp(f->name, "Reset_Handler") || !f->called;
        if (!root)
            continue;
        char path[1024];
        int pn = 0;
        for (int j = i, d2 = 0; j >= 0 && d2 < 24 && pn < 1000;
             j = g_s[j].next, d2++)
            pn += snprintf(path + pn, sizeof path - (size_t)pn, "%s%s",
                           d2 ? " > " : "", g_s[j].name);
        char st[32], cy[32];
        snprintf(st, sizeof st, f->stack_known ? "%llu" : "%llu+?",
                 f->worst_stack);
        if (f->cycles_known)
            snprintf(cy, sizeof cy, "%llu", f->worst_cycles);
        else
            snprintf(cy, sizeof cy, "?");
        printf("%-32s %8u %10s %14s  %s%s\n", f->name, f->frame, st, cy,
               path, f->flags & SI_ISR ? "  (interrupt)" : "");
        if (!f->stack_known || !f->cycles_known)
            printf("%-32s %8s %10s %14s    unknown: %s%s\n", "", "", "", "",
                   f->why ? f->why : "?",
                   f->unb_line && (f->flags & SI_NOCYC) ? "" : "");
        if (f->flags & SI_ISR) {
            any_isr = 1;
            if (f->worst_stack > worst_isr) worst_isr = f->worst_stack;
            isr_known &= f->stack_known;
        } else if (!strcmp(f->name, "main") ||
                   !strcmp(f->name, "Reset_Handler")) {
            if (f->worst_stack > worst_main) worst_main = f->worst_stack;
            main_known &= f->stack_known;
        }
    }
    unsigned long long total = worst_main;
    if (any_isr)
        total += worst_isr + trap_bytes(target);
    printf("\nworst case: %llu bytes%s", total,
           main_known && isr_known ? "" : " at least (some paths are unknown)");
    if (any_isr)
        printf(" = %llu (the program) + %llu (the deepest interrupt "
               "handler) + %s pushed by the hardware; nesting interrupts "
               "add a handler's for each level", worst_main, worst_isr,
               trap_frame(target));
    printf("\n");
    int rc = 0;
    if (limit >= 0 && (total > (unsigned long long)limit ||
                       !main_known || !isr_known)) {
        if (main_known && isr_known)
            fprintf(stderr, "embcc: error: the stack needs %llu bytes, more "
                            "than --stack-limit=%ld\n", total, limit);
        else
            fprintf(stderr, "embcc: error: the stack needs at least %llu "
                            "bytes and some paths have no bound (see the "
                            "report), so --stack-limit=%ld cannot be shown "
                            "to hold\n", total, limit);
        rc = 1;
    }
    free(units);
    free(g_s);
    g_s = NULL;
    free(d);
    return rc;
}
