/* Register allocation: see regalloc.h for why this is shared.
 *
 * Moved here from src/arch/x86_64/codegen.c with no behavioural change.
 * The proof of that is `tools/x86-identity.sh`, which compiles a corpus
 * with the compiler before the move and after it and requires the
 * OBJECTS to be byte-identical -- a stronger statement than "the tests
 * still pass", because it says nothing changed at all.
 *
 * What the move did change is the interface: the pool of registers, the
 * callee-saved predicate and the narrow-load question now arrive in a
 * `struct ra_target` instead of being file-scope constants of one
 * backend.
 */
#include "regalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../driver/remark.h"

/* The vreg WRITTEN by an instruction (its def), or -1. Kept in lockstep with
 * what codegen actually stores (cg_store / the STVAR store). Each temp is a
 * single-def SSA value; a local may be redefined, which liveness handles. */
int ra_ins_def(const struct ir_ins *in)
{
    switch (in->op) {
    case IR_CONST: case IR_MOV: case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_NEG: case IR_BNOT: case IR_CMP: case IR_LDVAR: case IR_ADDR:
    case IR_STRADDR: case IR_GADDR: case IR_FADDR: case IR_LOAD: case IR_EXT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_BSWAP: case IR_SQRT:
    case IR_SHL:
    case IR_SHR: case IR_XCHG: case IR_XADD: case IR_CMPXCHG:
    case IR_ARMW: case IR_CAS: case IR_CAS16: case IR_FRAMEADDR:
    case IR_ALLOCA: case IR_SPSAVE:
    case IR_STVAR:            /* the local written */
    case IR_CALL:             /* always stores a (possibly-unused) result temp */
    case IR_LABELADDR:        /* dst = &&label */
    /* A vector result is a 16-byte value in a slot, never a register the
     * integer allocator hands out -- but it IS a definition, and
     * liveness has to know that or the temp it overwrites looks live
     * across it. */
    case IR_VLOAD: case IR_VBIN: case IR_VSPLAT: case IR_VREDADD:
    case IR_VWIDEN: case IR_SELECT:
        return in->dst;
    case IR_ASM:              /* its `val` output's value, or -1 */
        return in->dst;
    default:
        return -1;            /* STORE, RET, LABEL, JMP, branches, MEMCPY, ... */
    }
}

/* Backward liveness dataflow. Fills first[v]/last[v] with the min/max
 * instruction index at which vreg v is live — a sound over-approximation of its
 * live range that spans loop back-edges (a naive first/last-appearance interval
 * does NOT, and would let a loop-carried value's register be clobbered mid-loop).
 * -1 for a vreg that is never live. ALSO returns, for the interference graph,
 * the per-instruction live-IN and live-OUT bitsets (each nins*words) and the def
 * vreg per instruction (all malloc'd, caller frees), and *words_out. Returns
 * NULL bitsets (and leaves the outputs NULL) for an empty function. */
/* Every vreg this instruction READS, once each.
 *
 * There were two copies of this switch -- the liveness scan below and
 * the x86 backend's count_vreg_uses -- and a third was about to be
 * written for aarch64. They are a hand-maintained list over an enum that
 * grows, which is the shape of thing that drifts silently: an op whose
 * operand one copy forgets is an operand nothing thinks is live. So
 * there is one, and the callers differ only in what they do with each
 * vreg. */
void ra_each_use(const struct ir_ins *s, void (*cb)(int v, void *ctx),
                 void *ctx)
{
#define U(v) do { int _v = (v); if (_v >= 0) cb(_v, ctx); } while (0)
    switch (s->op) {
    case IR_MOV: case IR_NEG: case IR_BNOT: case IR_EXT: case IR_BSWAP:
    case IR_SQRT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_LOAD: case IR_LDVAR:
    case IR_ADDR:
    case IR_STVAR: case IR_VA_START:
    case IR_ALLOCA: case IR_SPRESTORE:
    case IR_RET: case IR_BRZ: case IR_BRNZ:
    case IR_IGOTO:            /* `goto *p` reads p */
    case IR_SWITCH:           /* the table index */
    case IR_VLOAD:            /* the ADDRESS read, an integer temp */
    case IR_VSPLAT: case IR_VREDADD: case IR_VWIDEN:
        U(s->a); break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP: case IR_STORE: case IR_MEMCPY: case IR_MEMZERO:
    case IR_XCHG: case IR_XADD: case IR_ARMW:
    case IR_VSTORE: case IR_VBIN:
        U(s->a); U(s->b); break;
    case IR_CMPXCHG: case IR_CAS: case IR_CAS16: case IR_SELECT:
        U(s->a); U(s->b); U(s->c); break;
    case IR_CALL:
        if (s->indirect) U(s->a);
        for (int k = 0; k < s->nargs; k++) U(s->argv[k].vreg);
        break;
    case IR_ASM:
        if (s->asm_ir) {
            for (int k = 0; k < s->asm_ir->nin; k++) U(s->asm_ir->in[k].temp);
            /* an output's ADDRESS is read; a `val` one is the dst */
            for (int k = 0; k < s->asm_ir->nout; k++)
                if (!s->asm_ir->out[k].val) U(s->asm_ir->out[k].temp);
        }
        break;
    default: break;   /* CONST/STRADDR/GADDR/FADDR/LABEL/JMP/FENCE/UD2 */
    }
#undef U
}

struct ra_ucnt { int *cnt; int n; };
static void ra_ucnt_cb(int v, void *ctx)
{
    struct ra_ucnt *u = ctx;
    if (v < u->n) u->cnt[v]++;
}

void ra_count_vreg_uses(const struct ir_func *fn, int *cnt)
{
    struct ra_ucnt u = { cnt, fn->nvregs };
    for (int v = 0; v < fn->nvregs; v++) cnt[v] = 0;
    for (int i = 0; i < fn->nins; i++)
        ra_each_use(&fn->ins[i], ra_ucnt_cb, &u);
}

/* The liveness scan's use of ra_each_use: set this instruction's bit. */
struct ra_useset { unsigned long *use; int words, nvr, i; };
static void ra_useset_cb(int v, void *ctx)
{
    struct ra_useset *u = ctx;
    if (v < u->nvr)
        u->use[(size_t)u->i * u->words + (v >> 6)] |= 1UL << (v & 63);
}

unsigned long *ra_live_intervals(struct ir_func *fn, int *first,
                                             int *last, unsigned long **livein_out,
                                             int **defv_out, int *words_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    for (int v = 0; v < nvr; v++) { first[v] = -1; last[v] = -1; }
    *defv_out = NULL; *livein_out = NULL; *words_out = 0;
    if (nins == 0 || nvr == 0) return NULL;
    int words = (nvr + 63) / 64;

    unsigned long *use = xcalloc((size_t)nins * words, sizeof *use);
    unsigned long *in  = xcalloc((size_t)nins * words, sizeof *in);
    unsigned long *out = xcalloc((size_t)nins * words, sizeof *out);
    int *defv = xmalloc((size_t)nins * sizeof *defv);
    int *labelidx = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) *
                            sizeof *labelidx);
    for (int l = 0; l < fn->nlabels; l++) labelidx[l] = -1;
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_LABEL) labelidx[fn->ins[i].label] = i;

    struct ra_useset us = { use, words, nvr, 0 };
    for (int i = 0; i < nins; i++) {
        struct ir_ins *s = &fn->ins[i];
        defv[i] = ra_ins_def(s);
        us.i = i;
        ra_each_use(s, ra_useset_cb, &us);
    }
#undef USE

    /* iterate to a fixpoint: in[i] = use[i] ∪ (out[i] − def[i]);
     * out[i] = ∪ in[succ]. */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = nins - 1; i >= 0; i--) {
            struct ir_ins *s = &fn->ins[i];
            unsigned long *oi = out + (size_t)i * words;
            for (int w = 0; w < words; w++) oi[w] = 0;
            /* successors */
            if (s->op != IR_JMP && s->op != IR_RET && s->op != IR_UD2 &&
                s->op != IR_SWITCH && i + 1 < nins) {
                unsigned long *si = in + (size_t)(i + 1) * words;
                for (int w = 0; w < words; w++) oi[w] |= si[w];
            }
            if (s->op == IR_JMP || s->op == IR_BRZ || s->op == IR_BRNZ) {
                int t = labelidx[s->label];
                if (t >= 0) {
                    unsigned long *si = in + (size_t)t * words;
                    for (int w = 0; w < words; w++) oi[w] |= si[w];
                }
            }
            if (s->op == IR_SWITCH) {       /* the default and every entry */
                for (int k = -1; k < fn->jt[s->jt].n; k++) {
                    int l = k < 0 ? s->label : fn->jt[s->jt].labels[k];
                    int t = l >= 0 && l < fn->nlabels ? labelidx[l] : -1;
                    if (t < 0) continue;
                    unsigned long *si = in + (size_t)t * words;
                    for (int w = 0; w < words; w++) oi[w] |= si[w];
                }
            }
            /* in = use ∪ (out − def) */
            unsigned long *ii = in + (size_t)i * words;
            unsigned long *ui = use + (size_t)i * words;
            int dv = defv[i];
            for (int w = 0; w < words; w++) {
                unsigned long nv = oi[w];
                if (dv >= 0 && (dv >> 6) == w) nv &= ~(1UL << (dv & 63));
                nv |= ui[w];
                if (nv != ii[w]) { ii[w] = nv; changed = 1; }
            }
        }
    }

    /* occupied(v,i) = v ∈ in[i] ∪ out[i] ∪ {def[i]} -> update first/last */
    for (int i = 0; i < nins; i++) {
        unsigned long *ii = in + (size_t)i * words;
        unsigned long *oi = out + (size_t)i * words;
        for (int w = 0; w < words; w++) {
            unsigned long bits = ii[w] | oi[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int v = w * 64 + b;
                if (first[v] < 0) first[v] = i;
                last[v] = i;
                bits &= bits - 1;
            }
        }
        int dv = defv[i];
        if (dv >= 0) {
            if (first[dv] < 0) first[dv] = i;
            if (i > last[dv]) last[dv] = i;
        }
    }

    free(use); free(labelidx);
    *livein_out = in;
    *defv_out = defv;
    *words_out = words;
    return out;
}

/* 4M words: 32 MB for each of liveness's three sets. A function of 2000
 * plain statements is 3M (14000 instructions by 14000 vregs) and
 * allocates in 0.4 s; one of 4000 is 12M. */
#define RA_O0_DENSE_WORDS (4L << 20)

int ra_o0_too_big(const struct ir_func *fn)
{
    long words = ((long)fn->nvregs + 63) / 64;
    return (long)fn->nins * words > RA_O0_DENSE_WORDS;
}

/* mark vreg v ineligible (used at an opaque site) */
/* Why a value is not eligible, for the -fremarks-style accounting that
 * EMBCC_RA_WHY prints: "which values are in memory, and on whose rule"
 * is the question every size investigation in this backend starts from,
 * and it used to be answered by reading the switch below and guessing. */
static const char **g_ra_why;      /* NULL unless EMBCC_RA_WHY is set */
static int g_ra_why_n;

struct ra_touch { int *n; int nvr; };
static void ra_touch_cb(int v, void *ctx)
{
    struct ra_touch *t = ctx;
    if (v >= 0 && v < t->nvr) t->n[v]++;
}

#define OPAQUE_R(v, r) do { int _v = (v); \
    if (_v >= 0 && _v < nvr) { \
        if (elig[_v] && g_ra_why && _v < g_ra_why_n) g_ra_why[_v] = (r); \
        elig[_v] = 0; \
    } } while (0)
#define OPAQUE(v) OPAQUE_R(v, ir_opname(in->op))

/* Union-find over the interference graph's nodes, for coalescing. */
static int ra_find(int *alias, int x)
{
    while (alias[x] != x) { alias[x] = alias[alias[x]]; x = alias[x]; }
    return x;
}

/* Both register classes go through one body. What differs is which
 * values are eligible, which pool they are coloured from, and which of
 * those registers a call preserves -- everything after that (liveness,
 * interference, the colouring itself) is the same question asked of a
 * different set. Keeping it one function is deliberate: two copies of a
 * graph colourer would drift exactly the way the operand switches did.
 *
 * `fltmap` is cg_float_vregs: NULL for the integer pass, and for the
 * float pass the vregs that hold a float or a double. */
static int *ra_allocate_class(struct ir_func *fn, const struct ra_target *t,
                              const char *g_wide, const char *fltmap, int fp,
                              int *used_out, int *nused_out);

/* ---- spill cost -----------------------------------------------------
 *
 * What a value costs to keep in memory: a load at each read and a store at
 * each write, weighted by how deep in loops it happens. The simplify pass
 * below spills the node with the lowest cost for the square of its degree
 * -- Chaitin's rule, with the degree counted twice because a node that
 * interferes with more of the graph frees more of it -- where it used to
 * spill the one with the highest degree alone, the rule that sends a value
 * read on every trip of a loop to the stack because it is live across a
 * long switch. Measured against cost/degree and against no loop weight:
 * this is the smallest on all five targets together (-1411 bytes of code,
 * math included), AVR and x86-64 most. */
struct ra_costacc { const int *eof; const int *alias; unsigned long *cost;
                    unsigned long w; int nvr; };
/* Loop depth per instruction: a loop being the span of a backward
 * branch, which is what the IR has for one (a switch never closes a
 * loop; a branch to a label at or before itself does). Capped at five
 * levels by the callers that weight by it. */
static int *ra_loop_depth(const struct ir_func *fn)
{
    int nins = fn->nins;
    int *depth = xcalloc((size_t)(nins ? nins : 1), sizeof *depth);
    int *lab = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *lab);
    for (int l = 0; l < fn->nlabels; l++) lab[l] = -1;
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_LABEL && fn->ins[i].label >= 0 &&
            fn->ins[i].label < fn->nlabels)
            lab[fn->ins[i].label] = i;
    for (int i = 0; i < nins; i++) {
        const struct ir_ins *in = &fn->ins[i];
        if ((in->op == IR_JMP || in->op == IR_BRZ || in->op == IR_BRNZ) &&
            in->label >= 0 && in->label < fn->nlabels &&
            lab[in->label] >= 0 && lab[in->label] <= i)
            for (int k = lab[in->label]; k <= i; k++)
                depth[k]++;
    }
    free(lab);
    return depth;
}
struct ra_depacc { int *vdep; int nvr, d; };
static void ra_depth_cb(int v, void *ctx)
{
    struct ra_depacc *c = ctx;
    if (v >= 0 && v < c->nvr && c->vdep[v] < c->d) c->vdep[v] = c->d;
}

static void ra_cost_cb(int v, void *ctx)
{
    struct ra_costacc *c = ctx;
    if (v < 0 || v >= c->nvr || c->eof[v] < 0)
        return;
    int e = c->eof[v];
    while (c->alias[e] != e)
        e = c->alias[e];
    c->cost[e] += c->w;
}

static const struct ra_range *g_ra_res;
static int g_ra_nres;
void ra_reserve(const struct ra_range *r, int n) { g_ra_res = r; g_ra_nres = n; }

int *ra_allocate(struct ir_func *fn, const struct ra_target *t,
                 const char *g_wide, const char *fltmap,
                 int *used_out, int *nused_out)
{
    return ra_allocate_class(fn, t, g_wide, fltmap, 0, used_out, nused_out);
}

/* The floating-point half. `fltmap` says which vregs are in this class;
 * a backend without an fp_pool gets nothing back. */
int *ra_allocate_fp(struct ir_func *fn, const struct ra_target *t,
                    const char *g_wide, const char *fltmap,
                    int *used_out, int *nused_out)
{
    *nused_out = 0;
    int probe = 0;
    if (!t->fp_pool_for || !fltmap || !t->fp_pool_for(fn, &probe) || !probe)
        return NULL;
    return ra_allocate_class(fn, t, g_wide, fltmap, 1, used_out, nused_out);
}

static int *ra_allocate_class(struct ir_func *fn, const struct ra_target *t,
                              const char *g_wide, const char *fltmap, int fp,
                              int *used_out, int *nused_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    int nvars = fn->nvars;
    int *loc = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *loc);
    for (int v = 0; v < nvr; v++) loc[v] = -1;
    *nused_out = 0;
    if (nvr == 0) { g_ra_res = NULL; g_ra_nres = 0; return loc; }

    int NP = 0;
    const int *POOL = fp ? t->fp_pool_for(fn, &NP) : t->pool_for(fn, &NP);
    /* EMBCC_RA_MAXPOOL=N: the first N registers of the pool and no more,
     * for a test that has to reach the lowerings' SPILLED-operand paths --
     * which a real pool reaches only in the few functions that run out,
     * so a register those paths clobber can go unnoticed for years. */
    if (!fp && getenv("EMBCC_RA_MAXPOOL")) {
        int m = atoi(getenv("EMBCC_RA_MAXPOOL"));
        if (m >= 0 && m < NP)
            NP = m;
    }
    int (*callee_saved)(int) = fp && t->is_fp_callee_saved
                             ? t->is_fp_callee_saved : t->is_callee_saved;

    int *hint = xmalloc((size_t)nvr * sizeof *hint);
    for (int v = 0; v < nvr; v++) hint[v] = -1;
    if (t->abi_hints) t->abi_hints(fn, hint);

    /* EMBCC_RA_WHY=1: account for every value that does NOT get a
     * register, by the rule that excluded it, weighted by how many
     * instructions touch it -- which is how many memory accesses the
     * exclusion costs. One line per function on stderr. */
    const char **why = NULL;
    if (!fp && getenv("EMBCC_RA_WHY")) {
        why = xcalloc((size_t)nvr, sizeof *why);
        g_ra_why = why; g_ra_why_n = nvr;
    } else {
        g_ra_why = NULL; g_ra_why_n = 0;
    }

    int *first = xmalloc((size_t)nvr * sizeof *first);
    int *last  = xmalloc((size_t)nvr * sizeof *last);
    char *elig = xmalloc((size_t)nvr);
    /* live ranges from real dataflow (spans loops); appearance intervals would
     * be unsound across a back-edge. `liveout`/`defv` drive the interference
     * graph below. */
    int *defv = NULL, lwords = 0;
    unsigned long *livein = NULL;
    unsigned long *liveout = ra_live_intervals(fn, first, last,
                                                    &livein, &defv, &lwords);

    /* A value LIVE-OUT of a call survives it, so it cannot sit in a caller-saved
     * register (the call clobbers all of them) — it takes a callee-saved reg or
     * memory. The call's own result (defv[i]) is born at the call, so it does
     * not cross THIS one. A value that is merely a call ARGUMENT may still use
     * r8/r9: the parallel move in case IR_CALL shuffles arguments already held
     * in r8/r9 without clobbering. A leaf has no calls, so `crosses` stays 0. */
    char *crosses = xcalloc((size_t)(nvr ? nvr : 1), 1);
    /* Registers a value may not take because an asm it lives across may
     * change them (ir_asm.clob), bit r for register r. */
    unsigned long *forbid = xcalloc((size_t)(nvr ? nvr : 1), sizeof *forbid);
    for (int i = 0; i < nins; i++) {
        /* ...and a call the IR does not spell IR_CALL: a backend that
         * lowers some op to a runtime helper says so here, or its
         * caller-saved registers are not safe (regalloc.h). */
        /* ...and inline asm where it may be one (regalloc.h asm_in_reg):
         * when what it changes is known, a value live across it keeps
         * out of exactly that; a continuation changes nothing itself. */
        const struct ir_asm *ia = fn->ins[i].op == IR_ASM ? fn->ins[i].asm_ir
                                                           : NULL;
        int asm_here = ia && t->asm_in_reg && !fp && !ia->cont;
        if (fn->ins[i].op != IR_CALL &&
            !(t->op_calls_helper && t->op_calls_helper(&fn->ins[i])) &&
            !asm_here)
            continue;
        unsigned long clob = asm_here ? ia->clob : 0;
        unsigned long *lo = liveout + (size_t)i * lwords;
        for (int w = 0; w < lwords; w++) {
            unsigned long bits = lo[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int v = w * 64 + b;
                if (v < nvr && v != defv[i]) {
                    if (clob) forbid[v] |= clob;
                    else      crosses[v] = 1;
                }
                bits &= bits - 1;
            }
        }
        /* An asm output written through an address reads that address
         * AFTER the template has run, inside the same instruction: it has
         * to survive the asm as a value live across it does. */
        if (asm_here)
            for (int k = 0; k < ia->nout; k++) {
                const struct ir_asm_op *o = &ia->out[k];
                if (o->val || o->mem || o->temp < 0 || o->temp >= nvr)
                    continue;
                if (clob) forbid[o->temp] |= clob;
                else      crosses[o->temp] = 1;
            }
    }
    if (!fp && t->saved_only) {
        const char *so = t->saved_only(fn);
        for (int v = 0; so && v < nvr; v++)
            if (so[v]) crosses[v] = 1;
    }
    for (int v = 0; v < nvr; v++) {
        if (fp) {
            /* this class is exactly the values cg_float_vregs found */
            elig[v] = fltmap[v] != 0;
            continue;
        }
        if (v >= nvars) {
            elig[v] = 1;                          /* a temp */
        } else {
            const struct ir_local *L = &fn->locals[v]; /* param or local */
            int sz = L->size;
            /* any scalar int/pointer that fits a GPR — char/short included: a
             * narrow write keeps the low bytes, a read movsx/movzx-extends. */
            elig[v] = (L->is_int_or_ptr ||
                       ((t->float_in_gpr || t->fp_reads_gpr) && !fp &&
                        L->is_scalar_float)) &&
                      (sz == 1 || sz == 2 || sz == 4 || sz == 8);
            if (!elig[v] && g_ra_why && v < g_ra_why_n)
                g_ra_why[v] = L->is_int_or_ptr ? "local-odd-size"
                                               : "local-not-scalar";
        }
    }

    /* A volatile local lives in its slot, in either class: every access
     * the program makes must be one to memory. In a register, longjmp
     * put back the value it had at setjmp -- which C promises it will
     * not, for a volatile -- and `volatile int v = 3; (void)v;` read
     * nothing at all. mem2reg already refused them. */
    for (int v = 0; v < nvr && v < nvars; v++)
        if (fn->locals[v].is_volatile) {
            if (elig[v] && g_ra_why && v < g_ra_why_n)
                g_ra_why[v] = "volatile";
            elig[v] = 0;
        }
    /* a long double lives in its 16-byte slot, never a register */
    for (int v = 0; v < nvr; v++)
        if (g_wide && g_wide[v]) {
            if (elig[v] && g_ra_why && v < g_ra_why_n)
                g_ra_why[v] = "16-byte";
            elig[v] = 0;
        }
    /* ...and a value belongs to ONE class. Without this the integer pass
     * would hand a GPR to a double and the float pass an FP register to
     * the same vreg, and the two halves of codegen would each believe
     * their own answer. */
    if (!fp && fltmap)
        for (int v = 0; v < nvr; v++)
            if (fltmap[v]) {
                if (elig[v] && g_ra_why && v < g_ra_why_n)
                    g_ra_why[v] = "float-class";
                elig[v] = 0;
            }

    for (int i = 0; i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        int is_float = in->flt || in->op == IR_I2F || in->op == IR_F2I ||
                       in->op == IR_F2F;
        /* On the integer pass a float operand is somebody else's: it has
         * no GPR home. On the float pass it is the whole point. */
        if (is_float && !fp && !t->float_in_gpr && !t->fp_reads_gpr) {
            OPAQUE(in->dst); OPAQUE(in->a); OPAQUE(in->b);
        }
        switch (in->op) {
        case IR_ADDR:      OPAQUE(in->a); break;          /* address-taken */
        /* IR_STORE's address is register-aware now (codegen stores to [reg]),
         * so it is NOT opaque — only its raw value path was. */
        case IR_VA_START:  OPAQUE(in->a); break;
        case IR_XCHG: case IR_XADD: case IR_ARMW:
            if (!t->atomic_in_reg) {                      /* raw addr/val slots */
                OPAQUE(in->a); OPAQUE(in->b);
            }
            break;
        case IR_CMPXCHG: case IR_CAS:
            if (!t->atomic_in_reg) {
                OPAQUE(in->a); OPAQUE(in->b); OPAQUE(in->c);
            }
            break;
        case IR_CAS16:                  /* a pair operation: never */
            OPAQUE(in->a); OPAQUE(in->b); OPAQUE(in->c); break;
        case IR_FRAMEADDR:
            OPAQUE(in->dst); break;                        /* a raw-slot result */
        case IR_MEMCPY: case IR_MEMZERO:
            /* The operands are ADDRESSES, used as the copy base. A
             * backend that takes them from a register leaves nothing
             * opaque here; one that reads the slot needs them there. */
            if (!t->memcpy_addr_in_reg) { OPAQUE(in->a); OPAQUE(in->b); }
            break;
        case IR_RET:
            /* A struct or float return reads its slot raw on every
             * backend; a scalar one only where the backend can take it
             * from a register. */
            if (fn->ret_abi.is_struct ||
                (in->flt && !t->float_in_gpr && !t->fp_reads_gpr
                     ? !fp : !t->ret_scalar_in_reg))
                OPAQUE(in->a);
            break;
        case IR_CALL:
            /* An INDIRECT call's target has to survive the argument
             * setup, and the argument setup writes the argument
             * registers -- so a target sitting in one is read after it
             * has been overwritten. Backends load the target last,
             * because that used to be safe: no pool held an argument
             * register. tests/exec/nested-decl.c is what says otherwise
             * now, with `mov x0, x14; mov x1, x13; mov x11, x0; blr x11`
             * calling whatever argument 0 happened to be.
             *
             * Keeping it in memory costs one load at an indirect call
             * and needs no backend to get an ordering right. */
            if (in->indirect) OPAQUE(in->a);
            /* A struct or float (SSE) argument loads its slot raw
             * everywhere. A scalar-integer one is moved straight into
             * its argument register only by a backend that knows how. */
            for (int k = 0; k < in->nargs; k++)
                if (in->argv[k].is_struct ||
                    (in->argv[k].cls[0] == CLASS_SSE && !t->float_in_gpr
                         ? !fp : !t->call_int_arg_in_reg))
                    OPAQUE(in->argv[k].vreg);
            if (in->retsize || (in->flt && !t->float_in_gpr ? !fp : 0))
                OPAQUE(in->dst);                     /* float/struct result */
            break;
        case IR_ASM:
            if (in->asm_ir && !t->asm_in_reg) {
                for (int k = 0; k < in->asm_ir->nin; k++)
                    OPAQUE(in->asm_ir->in[k].temp);
                for (int k = 0; k < in->asm_ir->nout; k++)
                    OPAQUE(in->asm_ir->out[k].temp);
                OPAQUE(in->dst);
            }
            break;
        default: break;
        }
    }

    /* A vreg live across inline asm cannot sit in a callee reg the asm might
     * clobber (the clobber set is not visible here), so exclude it. And a vreg
     * that never appears has nothing to allocate. */
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_ASM && !(t->asm_in_reg && !fp))
            for (int v = 0; v < nvr; v++)
                if (elig[v] && first[v] >= 0 && first[v] <= i && i <= last[v]) {
                    if (g_ra_why && v < g_ra_why_n)
                        g_ra_why[v] = "live-across-asm";
                    elig[v] = 0;
                }
    for (int v = 0; v < nvr; v++)
        if (first[v] < 0) elig[v] = 0;

    /* Number the eligible vregs 0..E-1 and build the PRECISE interference graph:
     * at each instruction the vregs in live-out(i) ∪ {def(i)} are simultaneously
     * live and so interfere pairwise. This is tighter than interval overlap —
     * two vregs whose ranges overlap but are never live at the same point don't
     * interfere, and a result may reuse a dying operand's register. */
    if (why) {
        int *touch = xcalloc((size_t)nvr, sizeof *touch);
        struct ra_touch tc = { touch, nvr };
        for (int i = 0; i < nins; i++) {
            ra_each_use(&fn->ins[i], ra_touch_cb, &tc);
            int d = ra_ins_def(&fn->ins[i]);
            if (d >= 0 && d < nvr) touch[d]++;
        }
        for (int v = 0; v < nvr; v++) {
            if (elig[v] || first[v] < 0) continue;
            fprintf(stderr, "ra-why %s %s %d\n",
                    fn->src && fn->name ? fn->name : "?",
                    why[v] ? why[v] : "no-rule", touch[v]);
        }
        free(touch); free(why);
        g_ra_why = NULL; g_ra_why_n = 0;
    }

    int *eof = xmalloc((size_t)nvr * sizeof *eof);   /* vreg -> eligible index */
    int E = 0;
    for (int v = 0; v < nvr; v++) eof[v] = elig[v] ? E++ : -1;
    int *eidx = xmalloc((size_t)(E ? E : 1) * sizeof *eidx);
    for (int v = 0; v < nvr; v++) if (eof[v] >= 0) eidx[eof[v]] = v;

    int ew = (E + 63) / 64;
    unsigned long *adj = E ? xcalloc((size_t)E * ew, sizeof *adj) : NULL;
    int *members = xmalloc((size_t)(E ? E : 1) * sizeof *members);
    /* Which instructions are COPIES the colourer may give one register:
     * the same test the coalescer below applies, since both are asking
     * whether the two ends hold the same value. */
#define RA_COPY(in) ((in)->op == IR_MOV || \
        ((in)->op == IR_LDVAR && t->ldvar_plain((in)->size, (in)->sign, (in)->w)) || \
        ((in)->op == IR_EXT && t->ext_plain && t->ext_plain(in)) || \
        ((in)->op == IR_STVAR && (in)->size >= 8))
    /* Interference, Chaitin's way: an edge from each DEFINITION to every
     * value live after it -- a dead def too, which still clobbers its
     * register -- except, for a copy, its source, which holds the value
     * just written. Values live into the function's first instruction
     * (parameters, and anything read before it is written) are live
     * together there with no definition to find them, so they are made
     * a clique.
     *
     * The copy exception is the point. `p2 = p; ...; p = p2 + 1` around
     * a loop keeps p live the whole way, so p and its copy were live at
     * once, and marking everything live together as interfering kept
     * them apart -- one more register, and a move between them each
     * time round. Nothing redefines either while both hold the value, and
     * if something did, THAT definition would add the edge. A value dying
     * at an instruction still shares a register with one born there, as
     * before: the def is only tied to what is live after it. */
    for (int i = 0; adj && i <= nins; i++) {
        int m = 0, dv = -1, src = -1;
        unsigned long *grp;
        if (i == nins) {                       /* the entry clique */
            if (!nins) break;
            grp = livein;
        } else {
            struct ir_ins *in = &fn->ins[i];
            dv = defv[i];
            if (dv < 0 || dv >= nvr || eof[dv] < 0)
                continue;
            if (RA_COPY(in))
                src = in->a;
            grp = liveout + (size_t)i * lwords;
        }
        for (int w = 0; w < lwords; w++) {
            unsigned long bits = grp[w];
            while (bits) {
                int b = 0; unsigned long tt = bits;
                while (!(tt & 1)) { tt >>= 1; b++; }
                int v = w * 64 + b;
                if (v < nvr && eof[v] >= 0 && v != src && v != dv)
                    members[m++] = eof[v];
                bits &= bits - 1;
            }
        }
        if (i < nins) {
            int a = eof[dv];
            for (int q = 0; q < m; q++) {
                int b = members[q];
                adj[(size_t)a * ew + (b >> 6)] |= 1UL << (b & 63);
                adj[(size_t)b * ew + (a >> 6)] |= 1UL << (a & 63);
            }
        } else {
            for (int p = 0; p < m; p++)
                for (int q = p + 1; q < m; q++) {
                    int a = members[p], b = members[q];
                    adj[(size_t)a * ew + (b >> 6)] |= 1UL << (b & 63);
                    adj[(size_t)b * ew + (a >> 6)] |= 1UL << (a & 63);
                }
        }
    }

    /* Move-preference (coalescing) graph: two vregs that share a register let
     * codegen drop a move, so record a preference edge between them when they do
     * NOT interfere; colouring then biases each toward a move-partner's colour.
     * Two sources of a preferred pair:
     *  - a plain copy `dst = a` (IR_MOV / IR_STVAR / non-extending IR_LDVAR):
     *    same register makes the copy a self-move that vanishes.
     *  - a two-address binop `dst = a OP b`: codegen emits `OP b,dst` with no
     *    setup move when dst holds operand a (`dst == a`), or the commuted
     *    `OP a,dst` when dst holds b and OP commutes. So dst prefers a, and for a
     *    commutative op with a register b, dst prefers b too. SUB/SHL/SHR do not
     *    commute (dst == b would force the RAX fallback), so only a is preferred.
     * Each edge is a BIAS, not a constraint, and is dropped when the pair
     * interferes — which is exactly when operand a is still live after the op, so
     * a genuinely reusable (dying) operand is the only one ever coalesced. */
    unsigned long *pref = E ? xcalloc((size_t)E * ew, sizeof *pref) : NULL;
    for (int i = 0; pref && i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        enum ir_op op = in->op;
        int d = in->dst, partner[2], np = 0;
        if (op == IR_MOV || op == IR_STVAR ||
            (op == IR_LDVAR && t->ldvar_plain(in->size, in->sign, in->w)) ||
            (op == IR_EXT && t->ext_plain && t->ext_plain(in))) {
            partner[np++] = in->a;
        } else if (op == IR_ADD || op == IR_SUB || op == IR_MUL ||
                   op == IR_DIV || op == IR_AND || op == IR_OR ||
                   op == IR_XOR || op == IR_SHL || op == IR_SHR) {
            partner[np++] = in->a;                       /* dst prefers a */
            /* DIVIDE is here for the two-operand SSE form (`divsd d,b`
             * means `d /= b`), which the coalescer below has always
             * known about while this bias did not. It does not commute,
             * so only operand a is preferred -- and on the integer side
             * idiv's operands are fixed in rax:rdx, so a preference for
             * a pool register changes nothing there either way. */
            int commut = op == IR_ADD || op == IR_MUL || op == IR_AND ||
                         op == IR_OR || op == IR_XOR;
            if (commut && !in->imm_b) partner[np++] = in->b;
        }
        for (int k = 0; k < np; k++) {
            int a = partner[k];
            if (d < 0 || d >= nvr || a < 0 || a >= nvr) continue;
            int ed = eof[d], ea = eof[a];
            if (ed < 0 || ea < 0 || ed == ea) continue;
            if (adj[(size_t)ed * ew + (ea >> 6)] & (1UL << (ea & 63))) continue;
            pref[(size_t)ed * ew + (ea >> 6)] |= 1UL << (ea & 63);
            pref[(size_t)ea * ew + (ed >> 6)] |= 1UL << (ed & 63);
        }
    }

    /* ---- coalescing ---------------------------------------------------
     *
     * A preference is not a promise. `pref` asks the colourer to give a
     * copy's two ends the same register and it obliges when one is
     * free, which over lib/libc and lib/libcxx leaves 8132
     * register-to-register moves on x86-64 against gcc's 2721. The
     * moves that survive are the ones where the partner's colour was
     * taken -- and the way to not lose those is to stop treating the
     * two ends as two nodes.
     *
     * So: where a copy's ends do not interfere, MERGE them. They then
     * cannot be coloured differently and the move is gone before the
     * colourer ever runs.
     *
     * Conservatively, by Briggs' test -- merge only when the combined
     * node has fewer than NP neighbours of significant degree, so the
     * merge cannot turn a colourable graph into an uncolourable one.
     *
     * Both of the obvious ways to get more of them were measured and
     * are worse. Dropping the test entirely costs 3904 bytes of x86-64
     * .text and 208 of aarch64's: aggressive coalescing trades moves
     * for spills and a spill here is a value in memory for its whole
     * life, so the trade loses. Iterating to a fixpoint -- a merge can
     * free two nodes that interfered only through the one absorbed --
     * costs 64 and 68 bytes, because the extra merges it finds are the
     * marginal ones whose constraint then falls on somebody else. One
     * conservative pass is the answer. */
    int *alias = xmalloc((size_t)(E ? E : 1) * sizeof *alias);
    char *absorbed = xcalloc((size_t)(E ? E : 1), 1);
    /* Per-NODE facts, because a merged node answers for all its members:
     * if any of them crosses a call the register must survive one, and
     * any ABI wish one of them had is the node's. */
    char *xcross = xcalloc((size_t)(E ? E : 1), 1);
    unsigned long *xforbid = xcalloc((size_t)(E ? E : 1), sizeof *xforbid);
    int *ehint = xmalloc((size_t)(E ? E : 1) * sizeof *ehint);
    /* ...and how deep in a loop a node's members live, for the one
     * merge that is refused below. */
    int *idepth = ra_loop_depth(fn);
    int *ndep = xcalloc((size_t)(E ? E : 1), sizeof *ndep);
    {
        int *vdep = xcalloc((size_t)nvr, sizeof *vdep);
        struct ra_depacc da = { vdep, nvr, 0 };
        for (int i = 0; i < nins; i++) {
            da.d = idepth[i] > 5 ? 5 : idepth[i];
            ra_each_use(&fn->ins[i], ra_depth_cb, &da);
            ra_depth_cb(fn->ins[i].op == IR_STVAR ? fn->ins[i].dst
                                                  : ra_ins_def(&fn->ins[i]), &da);
        }
        for (int e = 0; e < E; e++) ndep[e] = vdep[eidx[e]];
        free(vdep);
    }
    for (int e = 0; e < E; e++) {
        alias[e] = e;
        xcross[e] = crosses[eidx[e]];
        xforbid[e] = forbid[eidx[e]];
        ehint[e] = hint[eidx[e]];
    }
    if (adj) {
        int *deg = xmalloc((size_t)E * sizeof *deg);
        for (int e = 0; e < E; e++) {
            int d = 0;
            unsigned long *row = adj + (size_t)e * ew;
            for (int w = 0; w < ew; w++) {
                unsigned long b = row[w];
                while (b) { d++; b &= b - 1; }
            }
            deg[e] = d;
        }
        for (int i = 0; i < nins; i++) {
            struct ir_ins *in = &fn->ins[i];
            /* Only a copy that moves the WHOLE value: `pref` may ask
             * for the same register on a narrowing one because it is
             * only asking, and if the colourer says no the move is
             * still emitted and still truncates. Coalescing does not
             * ask, so a narrowing copy between merged vregs would
             * become a move from a register to itself and truncate
             * nothing. */
            int alu = 0;
            switch (in->op) {
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
            case IR_AND: case IR_OR: case IR_XOR:
            case IR_SHL: case IR_SHR:
                /* A two-operand machine computes `d = a op b` as
                 * `d = a; d op= b`, so d and a want the same register
                 * and the move is the price of not getting it. They can
                 * only share one when a dies here -- which is exactly
                 * what "they do not interfere" means. x86-64's SSE is
                 * two-operand too, and __ieee754_atan2 was paying 49
                 * movaps for it. */
                alu = t->alu_dst_is_lhs; break;
            default: break;
            }
            if (!alu && in->op != IR_MOV &&
                !(in->op == IR_LDVAR &&
                  t->ldvar_plain(in->size, in->sign, in->w)) &&
                !(in->op == IR_EXT && t->ext_plain && t->ext_plain(in)) &&
                !(in->op == IR_STVAR && in->size >= 8))
                continue;
            int d = in->dst, a = in->a;
            if (d < 0 || d >= nvr || a < 0 || a >= nvr) continue;
            if (eof[d] < 0 || eof[a] < 0) continue;
            int x = ra_find(alias, eof[d]), y = ra_find(alias, eof[a]);
            if (x == y) continue;
            if (adj[(size_t)x * ew + (y >> 6)] & (1UL << (y & 63))) continue;
            /* Briggs: the union's neighbours of degree >= NP must be
             * fewer than NP. */
            int high = 0;
            for (int w = 0; w < ew && high < NP; w++) {
                unsigned long b = adj[(size_t)x * ew + w] |
                                  adj[(size_t)y * ew + w];
                while (b) {
                    int bit = 0; unsigned long tt = b;
                    while (!(tt & 1)) { tt >>= 1; bit++; }
                    int ne = w * 64 + bit;
                    if (ne != x && ne != y && !absorbed[ne] && deg[ne] >= NP)
                        high++;
                    b &= b - 1;
                }
            }
            if (high >= NP) continue;
            /* A node that crosses a call takes a callee-saved register,
             * and so would the union. When the OTHER node is the deeper
             * one in a loop -- the inside name a live-range split gave a
             * value, crossing nothing -- the merge would charge every
             * iteration for a crossing made once outside: the moves it
             * saves are the two on the loop's edges, the moves it costs
             * are the ones into and out of the callee-saved register on
             * every trip. Keep them apart. Equal depth merges as before:
             * a call's result copied into a value that crosses the next
             * call has to move out of r0 anyway. */
            if (xcross[x] != xcross[y]) {
                int dc = xcross[x] ? ndep[x] : ndep[y];
                int dn = xcross[x] ? ndep[y] : ndep[x];
                if (dn > dc) continue;
            }
            /* merge y into x */
            for (int w = 0; w < ew; w++) {
                unsigned long b = adj[(size_t)y * ew + w];
                while (b) {
                    int bit = 0; unsigned long tt = b;
                    while (!(tt & 1)) { tt >>= 1; bit++; }
                    int ne = w * 64 + bit;
                    adj[(size_t)ne * ew + (y >> 6)] &= ~(1UL << (y & 63));
                    if (ne != x) {
                        adj[(size_t)x * ew + (ne >> 6)] |= 1UL << (ne & 63);
                        adj[(size_t)ne * ew + (x >> 6)] |= 1UL << (x & 63);
                    }
                    b &= b - 1;
                }
                adj[(size_t)y * ew + w] = 0;
            }
            alias[y] = x;
            absorbed[y] = 1;
            xcross[x] |= xcross[y];
            xforbid[x] |= xforbid[y];
            if (ndep[y] > ndep[x]) ndep[x] = ndep[y];
            if (ehint[x] < 0) ehint[x] = ehint[y];
            int d2 = 0;
            for (int w = 0; w < ew; w++) {
                unsigned long b = adj[(size_t)x * ew + w];
                while (b) { d2++; b &= b - 1; }
            }
            deg[x] = d2;
            deg[y] = 0;
        }
        free(deg);
    }

    /* Each node's spill cost (ra_cost_cb): its reads and writes, a loop
     * level multiplying their weight by four -- a loop being the span of a
     * backward branch, which is what the IR has for one. */
    unsigned long *cost = xcalloc((size_t)(E ? E : 1), sizeof *cost);
    /* EMBCC_RA_DEGREE_SPILL=1: the old rule, highest degree first, for
     * bisecting a difference to this choice. */
    int by_degree = getenv("EMBCC_RA_DEGREE_SPILL") != NULL;
    {
        int *depth = idepth;
        struct ra_costacc ca;
        ca.eof = eof; ca.alias = alias; ca.cost = cost; ca.nvr = nvr;
        for (int i = 0; i < nins; i++) {
            int d = depth[i] > 5 ? 5 : depth[i];
            const struct ir_ins *in = &fn->ins[i];
            ca.w = 1UL << (2 * d);
            ra_each_use(in, ra_cost_cb, &ca);
            ra_cost_cb(in->op == IR_STVAR ? in->dst : ra_ins_def(in), &ca);
        }
    }

    /* Chaitin-Briggs simplify order. Repeatedly remove a node of degree < NCALLEE
     * (trivially colourable) onto a stack; when none remains, remove the node
     * with the lowest spill cost for its degree as an OPTIMISTIC spill
     * candidate. Colouring then pops the stack
     * (below) — a spill candidate popped early may still find a free colour, so
     * fewer values actually spill than a fixed first-appearance order gives.
     * Deterministic: ties broken by the lowest eligible index.
     *
     * "Trivially colourable" is a count of the registers the node may
     * TAKE, and one that crosses a call may take only the callee-saved
     * ones (colouring below forbids the rest). Counting the whole pool for
     * it as well -- 20 on RISC-V, 11 of them callee-saved -- passed every
     * such node as colourable, so the cost above never chose among them:
     * the stack order did. In a loop of calls that kept the constants and
     * addresses LICM had hoisted, read once a trip, and spilled the loop's
     * own counter: `i++; i < 700` was seven instructions through two
     * stack slots, around two calls a trip. EMBCC_RA_POOL_K=1 counts
     * the whole pool again, for bisecting a difference to this. */
    int *order = xmalloc((size_t)(E ? E : 1) * sizeof *order);
    int norder = 0;                  /* < E once coalescing absorbed nodes */
    int NCALLEE = 0, pool_k = getenv("EMBCC_RA_POOL_K") != NULL;
    for (int k = 0; k < NP; k++)
        if (callee_saved(POOL[k])) NCALLEE++;
    {
        int *deg = xmalloc((size_t)(E ? E : 1) * sizeof *deg);
        for (int e = 0; e < E; e++) {
            int d = 0;
            unsigned long *row = adj + (size_t)e * ew;
            for (int w = 0; w < ew; w++) {
                unsigned long b = row[w];
                while (b) { d++; b &= b - 1; }
            }
            deg[e] = d;
        }
        char *gone = xcalloc((size_t)(E ? E : 1), 1);
        int sp = 0;
        for (int cnt = 0; cnt < E; cnt++) {
            int pick = -1;
            for (int e = 0; e < E; e++)          /* a trivially-colourable node */
                if (!gone[e] && !absorbed[e] &&
                    deg[e] < (xcross[e] && !pool_k ? NCALLEE : NP)) {
                    pick = e;
                    break;
                }
            if (pick < 0)                        /* else the cheapest to spill */
                for (int e = 0; e < E; e++)
                    if (!gone[e] && !absorbed[e] &&
                        (pick < 0 ||
                         (by_degree
                              ? deg[e] > deg[pick]
                              : (unsigned long long)cost[e] * deg[pick] * deg[pick] <
                                (unsigned long long)cost[pick] * deg[e] * deg[e])))
                        pick = e;
            if (pick < 0) break;                 /* only absorbed nodes left */
            gone[pick] = 1;
            order[sp++] = pick;                  /* push */
            unsigned long *row = adj + (size_t)pick * ew;
            for (int w = 0; w < ew; w++) {
                unsigned long b = row[w];
                while (b) {
                    int bit = 0; unsigned long t = b;
                    while (!(t & 1)) { t >>= 1; bit++; }
                    int ne = w * 64 + bit;
                    if (!gone[ne]) deg[ne]--;
                    b &= b - 1;
                }
            }
        }
        norder = sp;
        for (int i = 0; i < sp / 2; i++) {       /* pop order = reverse of push */
            int t = order[i]; order[i] = order[sp - 1 - i]; order[sp - 1 - i] = t;
        }
        free(deg); free(gone);
    }

    /* Each node's span, for the reserved ranges below: the earliest
     * first and the latest last of its members. */
    int *nfirst = xmalloc((size_t)(E ? E : 1) * sizeof *nfirst);
    int *nlast = xmalloc((size_t)(E ? E : 1) * sizeof *nlast);
    for (int e = 0; e < E; e++) { nfirst[e] = nins; nlast[e] = -1; }
    if (g_ra_nres)
        for (int v = 0; v < nvr; v++) {
            if (eof[v] < 0 || first[v] < 0) continue;
            int e = ra_find(alias, eof[v]);
            if (first[v] < nfirst[e]) nfirst[e] = first[v];
            if (last[v] > nlast[e]) nlast[e] = last[v];
        }
    int reg_used[RA_MAXPOOL];
    int nspill = 0;
    for (int k = 0; k < NP; k++) reg_used[k] = 0;
    for (int oi = 0; oi < norder; oi++) {
        int e = order[oi];
        if (e < 0 || absorbed[e]) continue;
        int taken = 0;                        /* bitmask of neighbour registers */
        /* ...and the registers a pair holds while this node lives */
        for (int r = 0; r < g_ra_nres; r++)
            if (g_ra_res[r].last >= nfirst[e] && g_ra_res[r].first <= nlast[e])
                for (int k = 0; k < NP; k++)
                    if (POOL[k] == g_ra_res[r].reg) taken |= 1 << k;
        unsigned long *row = adj + (size_t)e * ew;
        for (int w = 0; w < ew; w++) {
            unsigned long bits = row[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int ne = w * 64 + b;
                int nl = loc[eidx[ne]];
                if (nl >= 0)
                    for (int k = 0; k < NP; k++)
                        if (POOL[k] == nl) taken |= 1 << k;
                bits &= bits - 1;
            }
        }
        /* A value that crosses a call may not take a caller-saved register: the
         * call clobbers them, so forbid them here (leaving callee-saved/spill). */
        if (xcross[e])
            for (int k = 0; k < NP; k++)
                if (!callee_saved(POOL[k])) taken |= 1 << k;
        /* ...nor one an asm it lives across may change */
        if (xforbid[e])
            for (int k = 0; k < NP; k++)
                if (POOL[k] >= 0 && POOL[k] < 64 && (xforbid[e] >> POOL[k] & 1))
                    taken |= 1 << k;
        /* preferred colours: registers a colored, non-interfering move-partner
         * already holds (and that are still free) */
        int want = 0;
        unsigned long *prow = pref + (size_t)e * ew;
        for (int w = 0; w < ew; w++) {
            unsigned long bits = prow[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int pe = w * 64 + b;
                int pl = loc[eidx[pe]];
                if (pl >= 0)
                    for (int k = 0; k < NP; k++)
                        if (POOL[k] == pl && !(taken & (1 << k)))
                            want |= 1 << k;
                bits &= bits - 1;
            }
        }
        /* ...and the one the ABI would like, on the same terms */
        if (ehint[e] >= 0)
            for (int k = 0; k < NP; k++)
                if (POOL[k] == ehint[e] && !(taken & (1 << k)))
                    want |= 1 << k;
        int pick = -1;
        /* On a three-operand machine the ABI's hint comes FIRST. The
         * arithmetic bias (dst prefers an operand) is a tie-breaker there
         * -- it lets Thumb use its two-operand 16-bit forms -- and letting
         * it outvote a parameter's own register is what made `int add(int
         * a, int b) { return a + b; }` put b in the result's r0, a in r1,
         * and open with a three-move swap, on Thumb, RISC-V and aarch64. A
         * two-operand machine keeps the old order: there the bias saves a
         * move on every operation. */
        if (!t->alu_dst_is_lhs && ehint[e] >= 0)
            for (int k = 0; k < NP; k++)
                if (POOL[k] == ehint[e] && !(taken & (1 << k))) { pick = k; break; }
        if (pick < 0)
        for (int k = 0; k < NP; k++)                  /* a free preferred reg */
            if ((want & (1 << k)) && !(taken & (1 << k))) { pick = k; break; }
        /* Else the lowest free register -- but first one no interfering
         * value still to be coloured has asked for. Colouring order is
         * the simplify stack's, not the program's, so a constant could
         * take a0 ahead of the parameter that ARRIVES in a0 and is passed
         * in it again: `mv a1, a0` at entry and `mv a0, a1` before the
         * call, around nothing. */
        if (pick < 0) {
            int avoid = 0;
            for (int w = 0; w < ew; w++) {
                unsigned long bits = row[w];
                while (bits) {
                    int b = 0; unsigned long tt = bits;
                    while (!(tt & 1)) { tt >>= 1; b++; }
                    int ne = w * 64 + b;
                    if (!absorbed[ne] && loc[eidx[ne]] < 0 && ehint[ne] >= 0)
                        for (int k = 0; k < NP; k++)
                            if (POOL[k] == ehint[ne]) avoid |= 1 << k;
                    bits &= bits - 1;
                }
            }
            /* ...but not at the price of a save: a register the ABI
             * wants for someone else is still better than a callee-saved
             * one the prologue must push for this value. */
            int first = -1;
            for (int k = 0; k < NP && first < 0; k++)
                if (!(taken & (1 << k))) first = k;
            for (int k = 0; k < NP; k++)
                if (!(taken & (1 << k)) && !(avoid & (1 << k)) &&
                    (!callee_saved(POOL[k]) ||
                     (first >= 0 && callee_saved(POOL[first])))) {
                    pick = k;
                    break;
                }
        }
        if (pick < 0)
            for (int k = 0; k < NP; k++)               /* else lowest free */
                if (!(taken & (1 << k))) { pick = k; break; }
        if (pick >= 0) { loc[eidx[e]] = POOL[pick]; reg_used[pick] = 1; }
        else nspill++;               /* no colour: this value lives in memory */
    }
    /* A coalesced vreg has no node of its own any more: it takes the
     * colour of the one it was merged into, which is the whole point --
     * the copy between them is now a move from a register to itself. */
    for (int v = 0; v < nvr; v++)
        if (eof[v] >= 0) {
            int r = ra_find(alias, eof[v]);
            if (r != eof[v]) loc[v] = loc[eidx[r]];
        }
    /* Why a value ended up in memory is the other question people ask of an
     * optimizer, and the answer is a property of the whole function -- how
     * many values were live at once against how many registers exist -- not
     * of any one value. So it is reported once, with both numbers. */
    if (nspill && getenv("EMBCC_RA_WHY"))
        fprintf(stderr, "ra-spill %s %s %d of %d eligible, pool %d\n",
                fp ? "fp" : "int", fn->name ? fn->name : "?", nspill, E, NP);
    if (nspill && remarks_on())
        remark_add("regalloc", "spilled-to-stack",
                   fn->src ? fn->name : NULL, "no-register-free",
                   fn->src ? fn->file : NULL,
                   fn->src ? fn->line : 0,
                   "%d of %d values did not get one of the %d allocatable "
                   "registers", nspill, E, NP);

    /* Inline asm can hard-code a callee-saved register the allocator never sees
     * — cpuid writes RBX (its "=b" output), and any operand fixed to rbx/r12..r15
     * loads or overwrites that register. Such a register is clobbered by this
     * function all the same, so the prologue must preserve it: mark it used.
     * (Without this a caller that keeps a live value in rbx across the call gets
     * it silently corrupted — invisible until an optimization puts one there.) */
    for (int n = 0; n < fn->nins; n++) {
        if (fn->ins[n].op != IR_ASM || !fn->ins[n].asm_ir)
            continue;
        struct ir_asm *a = fn->ins[n].asm_ir;
        for (int j = 0; j < a->nout; j++)
            for (int k = 0; k < NP; k++)
                if (POOL[k] == a->out[j].reg) reg_used[k] = 1;
        for (int j = 0; j < a->nin; j++)
            for (int k = 0; k < NP; k++)
                if (POOL[k] == a->in[j].reg) reg_used[k] = 1;
    }

    /* Only the callee-saved registers actually used need a prologue save. */
    int nu = 0;
    for (int k = 0; k < NP; k++)
        if (reg_used[k] && callee_saved(POOL[k])) used_out[nu++] = POOL[k];
    *nused_out = nu;

    /* EMBCC_RA_TRACE=1: every allocated or eligible value, whether it
     * crosses a call, what the ABI hinted and where it went -- the view
     * that showed a live-range split's inside name being coalesced back
     * into the outside one. */
    if (getenv("EMBCC_RA_TRACE")) {
        fprintf(stderr, "ra %s pool(%d):", fn->name, NP);
        for (int k = 0; k < NP; k++) fprintf(stderr, " %d", POOL[k]);
        fprintf(stderr, " nins=%d E=%d\n", nins, E);
    }
    if (getenv("EMBCC_RA_TRACE"))
        for (int v = 0; v < nvr; v++)
            if (elig[v] || loc[v] >= 0)
                fprintf(stderr, "ra %s v%d cross=%d hint=%d loc=%d [%d,%d]\n",
                        fn->name, v, crosses[v], hint[v], loc[v], first[v], last[v]);
    free(hint); free(alias); free(absorbed); free(xcross); free(xforbid);
    free(ehint);
    free(ndep); free(idepth); free(nfirst); free(nlast);
    g_ra_res = NULL; g_ra_nres = 0;          /* consumed */
    free(cost);
    free(first); free(last); free(elig); free(crosses); free(forbid);
    free(eof); free(eidx); free(adj); free(pref);
    free(members); free(order);
    free(liveout); free(livein); free(defv);
    return loc;
}

#undef OPAQUE

/* ==== temp-slot coalescing, shared (D-011) =================================
 *
 * Two temps whose live ranges do not overlap can share one frame slot.
 * The x86-64 backend has done this since it was written; aarch64 gave
 * every temp its own eight bytes, which on a function the optimizer has
 * inflated with SSA versions is a frame several times larger than it
 * needs to be -- and on a recursive kernel function, a stack overflow.
 *
 * D-011 said what justifies lifting: a real algorithm duplicated
 * between two WORKING backends. This is that, and the check is that the
 * x86 objects come out byte-identical after the move -- tools/
 * x86-identity.sh compares emitted bytes, so "nothing changed" is a
 * statement about the code and not about the tests.
 *
 * What a backend supplies is the three things it knows and this does
 * not: which vregs live in registers (those need no slot at all), and
 * the two policy questions below. */
/* Which locals the code still names (regalloc.h). */
/* A local the allocator put in a register, whose slot therefore holds
 * nothing. Every read of such a local goes through in_reg(), so the slot
 * is dead storage -- and a function whose locals are ALL like this needs
 * no frame at all, which is what makes the leaf prologue below possible.
 *
 * The conditions are deliberately narrower than "in a register": an
 * aggregate is addressed as memory whatever the allocator thinks, an
 * address that escapes has to point at something, and a variadic
 * function's prologue writes the argument file to the frame. The
 * allocator already refuses most of these; the check does not rely on
 * that, because a slot that turns out to be live reads as garbage rather
 * than failing, and the whole point is that nothing quietly reads it. */
int ra_slot_dead(const struct ir_func *fn, const int *loc, const int *floc,
                 int v, int want_debug)
{
    const struct func *f = fn->src;
    int in_gp = loc && loc[v] >= 0, in_fp = floc && floc[v] >= 0;
    if ((!in_gp && !in_fp) || f->is_varargs || fn->has_alloca || want_debug)
        return 0;
    const struct type *t = f->var_tys[v];
    if (t->kind == TY_STRUCT || t->kind == TY_ARRAY || ty_size(t) > 8)
        return 0;
    /* A float local is fine when it is the FLOAT class that holds it --
     * and never when the integer one claims to, which it cannot. */
    if (ty_is_float(t) ? !in_fp : !in_gp)
        return 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a == v)
            return 0;
    return 1;
}

char *ra_locals_referenced(const struct ir_func *fn, int want_debug)
{
    size_t n = (size_t)(fn->nvars > 0 ? fn->nvars : 1);
    char *r = xcalloc(n, 1);
    if (want_debug || fn->has_alloca || (fn->src && fn->src->is_varargs)) {
        memset(r, 1, n);
        return r;
    }
    for (int i = 0; i < fn->nparams && i < fn->nvars; i++)
        r[i] = 1;
    /* IR_LDVAR's and IR_ADDR's `a` and IR_STVAR's `dst` are the only
     * operands anywhere in EmbIR that name a frame slot. */
    for (int i = 0; i < fn->nins; i++) {
        const struct ir_ins *in = &fn->ins[i];
        int v = in->op == IR_ADDR || in->op == IR_LDVAR ? in->a
              : in->op == IR_STVAR                      ? in->dst : -1;
        if (v >= 0 && v < fn->nvars)
            r[v] = 1;
    }
    return r;
}

int *ra_coalesce_temps(struct ir_func *fn, int nvars,
                       const struct ra_slots *o, int *npool_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    int ntemp = nvr - nvars;
    *npool_out = 0;
    if (ntemp <= 0)
        return NULL;

    /* basic-block id per instruction: a new block begins at a label and after
     * any branch/jump/return/ud2. */
    int *blk = xmalloc((size_t)(nins ? nins : 1) * sizeof *blk);
    int b = 0;
    for (int i = 0; i < nins; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) b++;
        blk[i] = b;
        if (op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
            op == IR_RET || op == IR_UD2 || op == IR_SWITCH)
            b++;
    }

    /* [first,last] instruction index over every appearance of each temp. */
    int *first = xmalloc((size_t)ntemp * sizeof *first);
    int *last  = xmalloc((size_t)ntemp * sizeof *last);
    for (int k = 0; k < ntemp; k++) { first[k] = -1; last[k] = -1; }
    for (int i = 0; i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        int vs[4]; int nv = 0;
        vs[nv++] = in->dst; vs[nv++] = in->a; vs[nv++] = in->b; vs[nv++] = in->c;
        for (int j = 0; j < nv; j++) {
            int v = vs[j];
            if (v >= nvars && v < nvr) {
                int k = v - nvars;
                if (first[k] < 0) first[k] = i;
                last[k] = i;
            }
        }
        if (in->op == IR_CALL)
            for (int a = 0; a < in->nargs; a++) {
                int v = in->argv[a].vreg;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
        if (in->op == IR_ASM && in->asm_ir) {
            struct ir_asm *ia = in->asm_ir;
            for (int a = 0; a < ia->nin; a++) {
                int v = ia->in[a].temp;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
            for (int a = 0; a < ia->nout; a++) {
                int v = ia->out[a].temp;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
        }
    }

    /* order temps by first-appearance (ties by temp index), via buckets keyed
     * on the first index — O(nins+ntemp) and deterministic. Never-appearing
     * temps bucket at `nins`. */
    int *head = xmalloc((size_t)(nins + 1) * sizeof *head);
    for (int i = 0; i <= nins; i++) head[i] = -1;
    int *nxt = xmalloc((size_t)ntemp * sizeof *nxt);
    for (int k = ntemp - 1; k >= 0; k--) {
        int fi = first[k] < 0 ? nins : first[k];
        nxt[k] = head[fi]; head[fi] = k;
    }

    /* linear scan: reuse a freed pool index for a coalescable temp once its
     * previous occupant is dead; a non-coalescable temp takes a fresh index it
     * never gives back. */
    int *slot = xmalloc((size_t)ntemp * sizeof *slot);
    int *freelist = xmalloc((size_t)ntemp * sizeof *freelist);
    int *act_last = xmalloc((size_t)ntemp * sizeof *act_last);
    int *act_idx  = xmalloc((size_t)ntemp * sizeof *act_idx);
    int nfree = 0, nact = 0, pool = 0;
    for (int i = 0; i <= nins; i++) {
        for (int k = head[i]; k >= 0; k = nxt[k]) {
            /* A register-resident temp (-O2 regalloc) never touches memory, so
             * it needs no stack slot — skip it, keeping the frame to the temps
             * that actually spill. (This also caps mem2reg's SSA-temp inflation:
             * the extra versions live in registers, not the frame.) */
            if ((o->loc && o->loc[k + nvars] >= 0) ||
                (o->floc && o->floc[k + nvars] >= 0)) {
                slot[k] = -1;
                continue;
            }
            if (first[k] < 0) {          /* never referenced: no slot needed */
                /* A temp that appears in no instruction is dead — nothing ever
                 * loads or stores it, so it needs no frame slot. This is common
                 * once the optimizer's immediate-fold detaches a CONST and DCE
                 * drops its defining instruction, leaving the temp unreferenced;
                 * giving each one an 8-byte throwaway slot inflates the frame for
                 * nothing, and a recursive kernel function (path walk, tree
                 * sweep) then overflows the kernel stack. Gated to optimizing
                 * builds so -O0 stays byte-identical (its throwaway layout is
                 * unchanged, which the self-host fixed point relies on). */
                slot[k] = o->opt_frames ? -1 : pool++;
                continue;
            }
            int coalescable = (blk[first[k]] == blk[last[k]]) && !o->has_cgoto;
            /* expire actives dead before this temp is defined */
            for (int a = 0; a < nact; ) {
                if (act_last[a] < first[k]) {
                    freelist[nfree++] = act_idx[a];
                    act_last[a] = act_last[nact - 1];
                    act_idx[a]  = act_idx[nact - 1];
                    nact--;
                } else {
                    a++;
                }
            }
            int idx = (coalescable && nfree > 0) ? freelist[--nfree] : pool++;
            slot[k] = idx;
            if (coalescable) {
                act_last[nact] = last[k];
                act_idx[nact]  = idx;
                nact++;
            }
        }
    }
    *npool_out = pool;

    free(blk); free(first); free(last); free(head); free(nxt);
    free(freelist); free(act_last); free(act_idx);
    return slot;
}


/* ---- a parallel move ----------------------------------------------------
 *
 * The algorithm is the standard one and the whole of it is the ordering.
 * Think of the pairs as a graph with an edge src -> dst: each dst has
 * exactly one incoming edge (a caller that gives two is a bug, and this
 * says so rather than emitting something plausible), and a register may
 * be the source of several.
 *
 * A move is SAFE to emit now when its destination is not still needed as
 * somebody's source -- writing it destroys nothing anybody is waiting
 * for. Emitting it removes that pair, which may make another safe. What
 * remains when nothing is safe is entirely cycles, because every node
 * left has its destination read by someone. Break one cycle by lifting a
 * value into `scratch` and pointing the pair that wanted it at scratch
 * instead; that node's destination is now read by nobody, and the chain
 * unwinds.
 *
 * regalloc.h says why this lives here before two backends asked for it.
 */
int ra_parallel_move(const int *dst, const int *src, int n, int scratch,
                     int *out_dst, int *out_src, int max)
{
    int d[RA_MAXPOOL * 2], s[RA_MAXPOOL * 2];
    int pending[RA_MAXPOOL * 2];
    int m = 0, nout = 0;

    if (n > (int)(sizeof d / sizeof d[0]))
        return -1;
    /* Drop the moves that are already in place, and reject a repeated
     * destination: two values cannot both end up in one register, and
     * an ordering cannot rescue that. */
    for (int i = 0; i < n; i++) {
        if (dst[i] == src[i])
            continue;
        for (int k = 0; k < m; k++)
            if (d[k] == dst[i])
                return -1;
        d[m] = dst[i];
        s[m] = src[i];
        pending[m] = 1;
        m++;
    }

    int left = m;
    while (left > 0) {
        int moved = 0;
        for (int i = 0; i < m; i++) {
            if (!pending[i])
                continue;
            /* Is d[i] still needed as a source by any pending move? */
            int needed = 0;
            for (int k = 0; k < m; k++)
                if (pending[k] && k != i && s[k] == d[i]) {
                    needed = 1;
                    break;
                }
            if (needed)
                continue;
            if (nout >= max)
                return -1;
            out_dst[nout] = d[i];
            out_src[nout] = s[i];
            nout++;
            pending[i] = 0;
            left--;
            moved = 1;
        }
        if (moved)
            continue;
        /* Only cycles remain. Lift one node's SOURCE into scratch and
         * make whoever reads that register read scratch instead -- so
         * the node whose destination it was becomes emittable. */
        for (int i = 0; i < m; i++) {
            if (!pending[i])
                continue;
            if (nout >= max)
                return -1;
            out_dst[nout] = scratch;
            out_src[nout] = d[i];
            nout++;
            for (int k = 0; k < m; k++)
                if (pending[k] && s[k] == d[i])
                    s[k] = scratch;
            break;
        }
    }
    return nout;
}

/* See regalloc.h: under -g the embedded backends keep every source
 * variable in its frame slot, so the location expression that names
 * the slot is the truth. Temporaries are untouched -- they have no
 * name and no DW_TAG_variable, so nothing describes them. */
char *ra_debug_pin_vars(const struct ir_func *fn)
{
    int n = fn->nvregs ? fn->nvregs : 1;
    char *m = xcalloc((size_t)n, 1);
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        m[v] = 1;
    return m;
}

/* A 64-bit shift right by 32 or more whose result is only ever read at
 * four bytes or fewer is the source's HIGH word, shifted: one register,
 * not a pair. The union forwarding (opt pass_punfwd) makes one of these
 * of every GET_HIGH_WORD, and as a pair each held two registers for one
 * word's worth of value -- enough, in fdlibm's larger functions, to push
 * others into memory. The result is left out of the wide map, and
 * the backend computes the one word (RV32 and ARMv7-M).
 *
 * Every reader has to say four bytes or fewer: an operation at w 4, an
 * extension of four or fewer, a narrow store or local or argument or
 * return. A copy that does not say four keeps it wide, since a copy
 * carries whatever width reaches it. */
static int nhs_reader(const struct ir_func *fn, const struct ir_ins *i,
                         int v)
{
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP: case IR_BRZ: case IR_BRNZ: case IR_NEG: case IR_BNOT:
        return i->w == 4 && !i->flt;
    case IR_MOV:
        return i->w == 4;
    case IR_EXT:
        return i->a == v && i->size <= 4;
    case IR_STORE:
        return i->b == v && i->a != v && i->size <= 4;
    case IR_STVAR:
        return i->size <= 4;
    case IR_RET:
        return !fn->ret_abi.is_struct && fn->ret_abi.size <= 4;
    case IR_CALL:
        if (i->a == v)
            return 0;
        for (int k = 0; k < i->nargs; k++)
            if (i->argv[k].vreg == v &&
                (i->argv[k].is_struct || i->argv[k].size > 4))
                return 0;
        return 1;
    default:
        return 0;
    }
}

char *ra_narrow_hishift(const struct ir_func *fn)
{
    int nv = fn->nvregs;
    char *nar = xcalloc((size_t)(nv ? nv : 1), 1);
    int *defs = xcalloc((size_t)(nv ? nv : 1), sizeof *defs);
    for (int n = 0; n < fn->nins; n++) {
        int d = fn->ins[n].dst;
        if (d >= 0 && d < nv) defs[d]++;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int v = i->dst, ok = 1, used = 0;
        if (i->op != IR_SHR || i->w != 8 || !i->imm_b || i->imm < 32 ||
            i->imm > 63 || v < fn->nvars || v >= nv || defs[v] != 1)
            continue;
        for (int m = 0; m < fn->nins && ok; m++) {
            const struct ir_ins *u = &fn->ins[m];
            int reads = u->a == v || (!u->imm_b && u->b == v) || u->c == v;
            if (u->op == IR_CALL)
                for (int k = 0; k < u->nargs; k++)
                    if (u->argv[k].vreg == v) reads = 1;
            if (!reads)
                continue;
            used = 1;
            ok = nhs_reader(fn, u, v);
        }
        nar[v] = (char)(ok && used);
    }
    free(defs);
    return nar;
}

/* ---- an address's constant offset, into the access ------------------
 *
 * `%p = add %s, #8; load [%p]` is a struct field on every target, and a
 * backend with a base+offset addressing mode spent an instruction on the
 * add and a register on %p for it: `addw r0, r5, #8; ldr r0, [r0]` where
 * `ldr r0, [r5, #8]` does. So an ADD of a constant whose EVERY use is the
 * address of a plain load or store folds into them -- the accesses take
 * its base and its offset (ir_ins.memoff) -- and the ADD goes.
 *
 * Before register allocation, because the base's live range grows to the
 * accesses. Only a base with one definition, so it holds the same value
 * there as at the ADD; only integer accesses of up to `max_size` bytes
 * (the paths that honour memoff: four on the 32-bit machines, eight on
 * aarch64); and only offsets the target says it can encode for that
 * access, [lo, hi - size]. */
int ra_fold_memoff(struct ir_func *fn, long lo, long hi, int w_addr,
                   int max_size, const char *wide)
{
    int nv = fn->nvregs, changed = 0;
    if (!nv)
        return 0;
    int *defs = xcalloc((size_t)nv, sizeof *defs);
    int *uses = xcalloc((size_t)nv, sizeof *uses);
    int *addr_uses = xcalloc((size_t)nv, sizeof *addr_uses);
    char *drop = xcalloc((size_t)(fn->nins ? fn->nins : 1), 1);
    for (int v = 0; v < fn->nparams && v < nv; v++)
        defs[v]++;
    for (int n = 0; n < fn->nins; n++) {
        int d = ra_ins_def(&fn->ins[n]);
        if (d >= 0 && d < nv) defs[d]++;
    }
    ra_count_vreg_uses(fn, uses);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* ...and not a store of a WIDE value, even a four-byte one: that
         * takes the backend's 64-bit path, which knows no memoff. */
        if ((i->op == IR_LOAD || i->op == IR_STORE) && i->a >= 0 &&
            i->a < nv && !i->flt && i->size >= 1 && i->size <= max_size &&
            i->w <= max_size && !(i->op == IR_STORE && i->b == i->a) &&
            !(wide && i->op == IR_STORE && i->b >= 0 && i->b < nv &&
              wide[i->b]) &&
            !(wide && i->op == IR_LOAD && i->dst >= 0 && i->dst < nv &&
              wide[i->dst]))
            addr_uses[i->a]++;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int p = i->dst, s = i->a;
        long k = i->imm;
        if (i->op != IR_ADD || !i->imm_b || i->flt || i->w != w_addr ||
            p < fn->nvars || p >= nv || s < 0 || s >= nv || s == p ||
            defs[p] != 1 || uses[p] != addr_uses[p] || !uses[p])
            continue;
        int ok = 1;
        for (int m = 0; m < fn->nins && ok; m++) {
            const struct ir_ins *u = &fn->ins[m];
            if ((u->op == IR_LOAD || u->op == IR_STORE) && u->a == p) {
                long off = (long)u->memoff + k;
                if (off < lo || off > hi - u->size) ok = 0;
            }
        }
        if (ok && defs[s] != 1) {
            /* A base written more than once -- a pointer a loop walks --
             * still holds the ADD's value at an access that follows it in
             * the same block with no write to the base between. Every
             * access must be there: an unrolled loop's `[p + 4]`,
             * `[p + 8]` are exactly this, and stayed an add and a load
             * each while the base's second definition, the walk itself,
             * kept the rule to one. */
            int seen = 0;
            for (int m = n + 1; m < fn->nins && ok; m++) {
                const struct ir_ins *u = &fn->ins[m];
                if (u->op == IR_LABEL || u->op == IR_JMP ||
                    u->op == IR_BRZ || u->op == IR_BRNZ || u->op == IR_RET ||
                    u->op == IR_IGOTO || u->op == IR_UD2 || u->op == IR_SWITCH)
                    break;
                if ((u->op == IR_LOAD || u->op == IR_STORE) && u->a == p)
                    seen++;
                if (seen == uses[p])
                    break;
                if (ra_ins_def(u) == s || (u->op == IR_STVAR && u->dst == s))
                    ok = 0;
            }
            if (seen != uses[p])
                ok = 0;
        }
        if (!ok)
            continue;
        for (int m = 0; m < fn->nins; m++) {
            struct ir_ins *u = &fn->ins[m];
            if ((u->op == IR_LOAD || u->op == IR_STORE) && u->a == p) {
                u->a = s;
                u->memoff += (int)k;
            }
        }
        drop[n] = 1;
        changed = 1;
    }
    if (changed) {
        int j = 0;
        for (int n = 0; n < fn->nins; n++)
            if (!drop[n]) fn->ins[j++] = fn->ins[n];
        fn->nins = j;
    }
    free(defs); free(uses); free(addr_uses); free(drop);
    return changed;
}
