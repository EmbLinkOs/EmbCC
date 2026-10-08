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
    case IR_MULH: case IR_MULW:
        return in->dst;
    case IR_ASM:              /* its `val` output's value, or -1 */
        return in->dst;
    default:
        return -1;            /* STORE, RET, LABEL, JMP, branches, MEMCPY, ... */
    }
}

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
    case IR_MULH: case IR_MULW:
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


/* ---- liveness, by block -----------------------------------------------
 *
 * What the old ra_live_intervals computed, without its per-instruction
 * sets. It kept three bit sets of every vreg at every instruction, so its
 * time and its memory were the square of a function's size: 4000 plain
 * statements are 16000 instructions and 28000 vregs at -O2, 56 MB per
 * set, and the fixpoint swept all of it until nothing changed. Once the
 * optimizer was linear, that and what read the sets were most of an -O2
 * compile of such a function on Cortex-M.
 *
 * Here only the BLOCKS' live-in and live-out sets are kept, as sorted
 * lists, and found one vreg at a time: from each block that reads the
 * vreg before writing it, backwards through predecessors until a block
 * that writes it -- the least solution of the same equations, so the same
 * sets. Inside a block liveness is a straight backward scan from the
 * block's live-out set, and every consumer walks it that way
 * (ra_lset_out / ra_lset_step). The blocks are those of the
 * instruction-level graph the old fixpoint used: a successor is the next
 * instruction unless the op is JMP, RET, UD2, SWITCH or IGOTO, plus a
 * jump's or a switch's labels -- and `goto *p` reaches every label whose
 * address is taken (an IR_LABELADDR names it). It used to fall through
 * here, as it did there: a value read only at a label the jump went BACK
 * to looked dead after its last read, and the allocator gave its register
 * to the next value -- harmless only on the backends that switched the
 * allocator off for a function with a computed goto. */
struct ra_live {
    int nins, nvr, nbb;
    int *bstart;                    /* block b is [bstart[b], bstart[b+1]) */
    int *blk;                       /* instruction -> block */
    int *in_off, *in_v;             /* live-in of b: in_v[in_off[b]..in_off[b+1]) */
    int *out_off, *out_v;           /* live-out, likewise; both ascending */
};

struct ra_pairs { int *v, *b; int n, cap; };
static void ra_pairs_add(struct ra_pairs *p, int v, int b)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 256;
        p->v = xrealloc(p->v, (size_t)p->cap * sizeof *p->v);
        p->b = xrealloc(p->b, (size_t)p->cap * sizeof *p->b);
    }
    p->v[p->n] = v;
    p->b[p->n] = b;
    p->n++;
}

/* Sort pairs into CSR by `key` (0: by v, 1: by b), stably, so a list
 * keeps the order the pairs were made in. */
static void ra_pairs_csr(const struct ra_pairs *p, int key, int nkey,
                         int **off_out, int **val_out)
{
    int *off = xcalloc((size_t)nkey + 1, sizeof *off);
    int *val = xmalloc((size_t)(p->n ? p->n : 1) * sizeof *val);
    const int *k = key ? p->b : p->v, *x = key ? p->v : p->b;
    for (int j = 0; j < p->n; j++) off[k[j] + 1]++;
    for (int j = 0; j < nkey; j++) off[j + 1] += off[j];
    int *fill = xmalloc((size_t)(nkey ? nkey : 1) * sizeof *fill);
    for (int j = 0; j < nkey; j++) fill[j] = off[j];
    for (int j = 0; j < p->n; j++) val[fill[k[j]]++] = x[j];
    free(fill);
    *off_out = off;
    *val_out = val;
}

struct ra_ue { struct ra_pairs *ue; int *ustamp; const int *dstamp; int b, nvr; };
static void ra_ue_cb(int v, void *ctx)
{
    struct ra_ue *u = ctx;
    if (v >= u->nvr || u->dstamp[v] == u->b + 1 || u->ustamp[v] == u->b + 1)
        return;
    u->ustamp[v] = u->b + 1;
    ra_pairs_add(u->ue, v, u->b);
}

struct ra_fl { int *first, *last, nvr, i; };
static void ra_fl_upd(struct ra_fl *f, int v, int i)
{
    if (f->first[v] < 0 || i < f->first[v]) f->first[v] = i;
    if (i > f->last[v]) f->last[v] = i;
}
static void ra_fl_cb(int v, void *ctx)
{
    struct ra_fl *f = ctx;
    if (v < f->nvr)
        ra_fl_upd(f, v, f->i);
}

struct ra_live *ra_live_compute(const struct ir_func *fn, int *first, int *last)
{
    int nins = fn->nins, nvr = fn->nvregs;
    for (int v = 0; v < nvr; v++) { first[v] = -1; last[v] = -1; }
    struct ra_live *lv = xcalloc(1, sizeof *lv);
    lv->nins = nins;
    lv->nvr = nvr;
    if (nins == 0 || nvr == 0)
        return lv;

    /* blocks: a label starts one, and so does whatever follows an op
     * whose successor is not simply the next instruction */
    char *lead = xcalloc((size_t)nins, 1);
    lead[0] = 1;
    for (int i = 0; i < nins; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) lead[i] = 1;
        if ((op == IR_JMP || op == IR_BRZ || op == IR_BRNZ || op == IR_RET ||
             op == IR_UD2 || op == IR_SWITCH || op == IR_IGOTO) &&
            i + 1 < nins)
            lead[i + 1] = 1;
    }
    int nbb = 0;
    for (int i = 0; i < nins; i++) nbb += lead[i];
    lv->nbb = nbb;
    lv->bstart = xmalloc((size_t)(nbb + 1) * sizeof *lv->bstart);
    lv->blk = xmalloc((size_t)nins * sizeof *lv->blk);
    for (int i = 0, b = -1; i < nins; i++) {
        if (lead[i]) lv->bstart[++b] = i;
        lv->blk[i] = b;
    }
    lv->bstart[nbb] = nins;
    free(lead);

    /* successors and predecessors, in CSR */
    int *labelidx = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) *
                            sizeof *labelidx);
    for (int l = 0; l < fn->nlabels; l++) labelidx[l] = -1;
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_LABEL && fn->ins[i].label >= 0 &&
            fn->ins[i].label < fn->nlabels)
            labelidx[fn->ins[i].label] = i;
    /* the blocks a computed goto may reach: one per address-taken label */
    int *taken = NULL, ntaken = 0;
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_IGOTO) {
            char *seen = xcalloc((size_t)nbb, 1);
            taken = xmalloc((size_t)nbb * sizeof *taken);
            for (int k = 0; k < nins; k++) {
                int l = fn->ins[k].label;
                if (fn->ins[k].op != IR_LABELADDR || l < 0 ||
                    l >= fn->nlabels || labelidx[l] < 0 ||
                    seen[lv->blk[labelidx[l]]])
                    continue;
                seen[lv->blk[labelidx[l]]] = 1;
                taken[ntaken++] = lv->blk[labelidx[l]];
            }
            free(seen);
            break;
        }
    struct ra_pairs edge = { NULL, NULL, 0, 0 };     /* (succ, pred) */
    for (int b = 0; b < nbb; b++) {
        int i = lv->bstart[b + 1] - 1;
        const struct ir_ins *s = &fn->ins[i];
        if (s->op != IR_JMP && s->op != IR_RET && s->op != IR_UD2 &&
            s->op != IR_SWITCH && s->op != IR_IGOTO && i + 1 < nins)
            ra_pairs_add(&edge, b + 1, b);
        if (s->op == IR_IGOTO)
            for (int k = 0; k < ntaken; k++)
                ra_pairs_add(&edge, taken[k], b);
        if ((s->op == IR_JMP || s->op == IR_BRZ || s->op == IR_BRNZ) &&
            s->label >= 0 && s->label < fn->nlabels &&
            labelidx[s->label] >= 0)
            ra_pairs_add(&edge, lv->blk[labelidx[s->label]], b);
        if (s->op == IR_SWITCH)
            for (int k = -1; k < fn->jt[s->jt].n; k++) {
                int l = k < 0 ? s->label : fn->jt[s->jt].labels[k];
                int t = l >= 0 && l < fn->nlabels ? labelidx[l] : -1;
                if (t >= 0)
                    ra_pairs_add(&edge, lv->blk[t], b);
            }
    }
    free(labelidx);
    free(taken);
    int *poff, *pred;                 /* predecessors of each block */
    ra_pairs_csr(&edge, 0, nbb, &poff, &pred);
    free(edge.v); free(edge.b);

    /* Per block, the vregs read before any write in it (upward-exposed)
     * and the vregs written. */
    struct ra_pairs ue = { NULL, NULL, 0, 0 }, df = { NULL, NULL, 0, 0 };
    int *ustamp = xcalloc((size_t)nvr, sizeof *ustamp);
    int *dstamp = xcalloc((size_t)nvr, sizeof *dstamp);
    struct ra_ue uc = { &ue, ustamp, dstamp, 0, nvr };
    for (int b = 0; b < nbb; b++) {
        uc.b = b;
        for (int i = lv->bstart[b]; i < lv->bstart[b + 1]; i++) {
            ra_each_use(&fn->ins[i], ra_ue_cb, &uc);
            int d = ra_ins_def(&fn->ins[i]);
            if (d >= 0 && d < nvr && dstamp[d] != b + 1) {
                dstamp[d] = b + 1;
                ra_pairs_add(&df, d, b);
            }
        }
    }
    free(ustamp); free(dstamp);
    int *ueoff, *ueb, *dfoff, *dfb;   /* by vreg: its blocks */
    ra_pairs_csr(&ue, 0, nvr, &ueoff, &ueb);
    ra_pairs_csr(&df, 0, nvr, &dfoff, &dfb);
    free(ue.v); free(ue.b); free(df.v); free(df.b);

    /* One vreg at a time, in increasing order, so that each block's lists
     * come out sorted (the sort by block below is stable). A stamp of
     * v + 1 means "this block, for this vreg". */
    struct ra_pairs lin = { NULL, NULL, 0, 0 }, lout = { NULL, NULL, 0, 0 };
    int *inmk = xcalloc((size_t)nbb, sizeof *inmk);
    int *outmk = xcalloc((size_t)nbb, sizeof *outmk);
    int *defmk = xcalloc((size_t)nbb, sizeof *defmk);
    int *work = xmalloc((size_t)nbb * sizeof *work);
    for (int v = 0; v < nvr; v++) {
        if (ueoff[v] == ueoff[v + 1])
            continue;                   /* never read: never live */
        for (int k = dfoff[v]; k < dfoff[v + 1]; k++) defmk[dfb[k]] = v + 1;
        int nw = 0;
        for (int k = ueoff[v]; k < ueoff[v + 1]; k++) {
            int b = ueb[k];
            inmk[b] = v + 1;
            ra_pairs_add(&lin, v, b);
            work[nw++] = b;
        }
        while (nw > 0) {
            int b = work[--nw];
            for (int k = poff[b]; k < poff[b + 1]; k++) {
                int p = pred[k];
                if (outmk[p] != v + 1) {
                    outmk[p] = v + 1;
                    ra_pairs_add(&lout, v, p);
                }
                if (defmk[p] != v + 1 && inmk[p] != v + 1) {
                    inmk[p] = v + 1;
                    ra_pairs_add(&lin, v, p);
                    work[nw++] = p;
                }
            }
        }
    }
    free(inmk); free(outmk); free(defmk); free(work);
    free(ueoff); free(ueb); free(dfoff); free(dfb);
    free(poff); free(pred);
    ra_pairs_csr(&lin, 1, nbb, &lv->in_off, &lv->in_v);
    ra_pairs_csr(&lout, 1, nbb, &lv->out_off, &lv->out_v);
    free(lin.v); free(lin.b); free(lout.v); free(lout.b);

    /* first[v]/last[v]: the earliest and latest instruction where v is
     * live in, live out or written. Within a block the earliest is its
     * start if v is live into it and else v's first write there; the
     * latest is its end if v is live out of it and else v's last read or
     * write -- so those, and every read and write (each one a point where
     * v is live or written), are all the candidates there are. */
    struct ra_fl fl = { first, last, nvr, 0 };
    for (int b = 0; b < nbb; b++) {
        int s = lv->bstart[b], e = lv->bstart[b + 1];
        for (int k = lv->in_off[b]; k < lv->in_off[b + 1]; k++)
            ra_fl_upd(&fl, lv->in_v[k], s);
        for (int k = lv->out_off[b]; k < lv->out_off[b + 1]; k++)
            ra_fl_upd(&fl, lv->out_v[k], e - 1);
        for (int i = s; i < e; i++) {
            fl.i = i;
            ra_each_use(&fn->ins[i], ra_fl_cb, &fl);
            int d = ra_ins_def(&fn->ins[i]);
            if (d >= 0 && d < nvr)
                ra_fl_upd(&fl, d, i);
        }
    }
    return lv;
}

/* Only first[] and last[]: the slot coalescers and the pair passes. */
void ra_live_ranges(const struct ir_func *fn, int *first, int *last)
{
    ra_live_free(ra_live_compute(fn, first, last));
}

void ra_live_free(struct ra_live *lv)
{
    if (!lv)
        return;
    free(lv->bstart); free(lv->blk);
    free(lv->in_off); free(lv->in_v); free(lv->out_off); free(lv->out_v);
    free(lv);
}

int ra_live_nblocks(const struct ra_live *lv) { return lv->nbb; }
int ra_live_block_start(const struct ra_live *lv, int b) { return lv->bstart[b]; }

/* The live set as a sparse set: members in any order, and each vreg's
 * place among them (or -1), so adding, removing and asking are O(1) and
 * visiting the members costs only as many as there are. */
void ra_lset_init(struct ra_lset *s, int nvr)
{
    s->nvr = nvr;
    s->n = 0;
    s->chg = NULL;
    s->chg_ctx = NULL;
    s->mem = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *s->mem);
    s->pos = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *s->pos);
    for (int v = 0; v < nvr; v++) s->pos[v] = -1;
}

void ra_lset_free(struct ra_lset *s)
{
    free(s->mem); free(s->pos);
}

void ra_lset_add(struct ra_lset *s, int v)
{
    if (v < 0 || v >= s->nvr || s->pos[v] >= 0)
        return;
    s->pos[v] = s->n;
    s->mem[s->n++] = v;
    if (s->chg)
        s->chg(v, 1, s->chg_ctx);
}

void ra_lset_del(struct ra_lset *s, int v)
{
    if (v < 0 || v >= s->nvr || s->pos[v] < 0)
        return;
    int p = s->pos[v], last = s->mem[--s->n];
    s->mem[p] = last;
    s->pos[last] = p;
    s->pos[v] = -1;
    if (s->chg)
        s->chg(v, 0, s->chg_ctx);
}

/* Make s the set live out of block b: live after its last instruction. */
void ra_lset_out(struct ra_lset *s, const struct ra_live *lv, int b)
{
    while (s->n > 0)
        ra_lset_del(s, s->mem[s->n - 1]);
    for (int k = lv->out_off[b]; k < lv->out_off[b + 1]; k++)
        ra_lset_add(s, lv->out_v[k]);
}

static void ra_lset_use_cb(int v, void *ctx) { ra_lset_add(ctx, v); }

/* Step back over instruction `in`: s goes from the set live after it to
 * the set live before it -- its write removed, its reads added. */
void ra_lset_step(struct ra_lset *s, const struct ir_ins *in)
{
    int d = ra_ins_def(in);
    if (d >= 0)
        ra_lset_del(s, d);
    ra_each_use(in, ra_lset_use_cb, s);
}

struct ra_isuse { int v, found; };
static void ra_isuse_cb(int v, void *ctx)
{
    struct ra_isuse *u = ctx;
    if (v == u->v) u->found = 1;
}

/* Is v live INTO instruction i? A scan back from the end of i's block,
 * for the few questions that want one answer and not a walk. */
int ra_live_in_at(const struct ra_live *lv, const struct ir_func *fn,
                  int i, int v)
{
    if (i < 0 || i >= lv->nins || v < 0 || v >= lv->nvr)
        return 0;
    int b = lv->blk[i], live = 0;
    for (int k = lv->out_off[b]; k < lv->out_off[b + 1]; k++)
        if (lv->out_v[k] == v) { live = 1; break; }
    for (int k = lv->bstart[b + 1] - 1; k >= i; k--) {
        struct ra_isuse u = { v, 0 };
        if (ra_ins_def(&fn->ins[k]) == v) live = 0;
        ra_each_use(&fn->ins[k], ra_isuse_cb, &u);
        if (u.found) live = 1;
    }
    return live;
}

/* 100000 instructions: a function of some 12000 plain statements on
 * Cortex-M or RISC-V, 10000 on AVR. Allocating one of 6000 at -O0 takes
 * 0.1 s on Cortex-M and about 1 s on AVR, which generates every function
 * several times over to keep the shortest; at the limit AVR takes about
 * 2 s (on an idle machine; three times that on a loaded one). The limit
 * was nins x nvregs bits when that was what liveness cost, which
 * stopped at about 2000 statements. */
#define RA_O0_MAX_INS 100000

int ra_o0_too_big(const struct ir_func *fn)
{
    return fn->nins > RA_O0_MAX_INS;
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
struct ra_costacc { const int *eof; int *alias; unsigned long *cost;
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
    /* ra_find, which compresses the path: a value AVR copies into each
     * of 4000 cases is merged into each copy in turn, and walking that
     * chain at every use of it was quadratic. The root is the same. */
    c->cost[ra_find(c->alias, c->eof[v])] += c->w;
}

/* A min-heap of node numbers, for the simplify order. */
static void ra_heap_push(int *h, int *n, int x)
{
    int i = (*n)++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h[p] <= x)
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = x;
}

static int ra_heap_pop(int *h, int *n)
{
    int top = h[0], x = h[--*n], i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= *n)
            break;
        if (c + 1 < *n && h[c + 1] < h[c])
            c++;
        if (x <= h[c])
            break;
        h[i] = h[c];
        i = c;
    }
    if (*n > 0)
        h[i] = x;
    return top;
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

/* ---- the interference graph, sparse ----------------------------------
 *
 * It was a bit matrix, nodes x nodes, and everything that walked a node's
 * neighbours scanned its whole row: a function of 8000 statements has
 * 24000 to 50000 nodes, up to 300 MB a matrix (twice, with the
 * preferences), and a row of up to 780 words however few neighbours the
 * node has -- and most nodes of a big function have a handful.
 *
 * So a node with at most RG_ROW neighbours keeps them in a list, and only
 * one with more keeps a bit row, as in the matrix. "Do these two
 * interfere" is then a bit, or a scan of at most RG_ROW entries; walking
 * a node's neighbours costs its list, or its row; and past a few
 * thousand nodes the graph is never bigger than the matrix was -- a list
 * is RG_ROW entries, a row a bit per node. (Lists for every node, rows
 * on top, were tried first: a function storing 8000 struct fields has 18
 * million edges, and that took 1.3 GB.)
 *
 * Coalescing merges nodes, and a merge must leave each neighbour naming
 * the survivor once, as the matrix's rows did once a merge had cleared
 * the absorbed node's bits. That is a rename in every neighbour of the
 * absorbed node -- and on AVR, where each read of a two-byte int is a
 * copy that coalesces, a value live across a long function is absorbed
 * into its next copy again and again, thousands of neighbours each time:
 * 160 million renames in one 1000-statement compile. So lists and rows
 * belong to STORAGE slots, and a node to one slot (slot[]); entries and
 * bits name slots, and owner[] says which node a slot is. A merge keeps
 * the slot with more neighbours for the survivor and folds the other one
 * in, so only the smaller side's neighbours are touched. A list entry
 * also holds where the reverse entry is (`back`) while the neighbour has
 * a list, so a rename or removal there is direct; a neighbour that
 * outgrew its list is a bit to flip. An edge between two live nodes is
 * never removed, so the answer about two live nodes is always the
 * matrix's; the order of a list is never relied on. */
#define RG_ROW 32
struct rg_ent { int v, back; };    /* a neighbour slot; this slot's index
                                     * in its list, while it has one */
struct ra_graph {
    int n, words;
    int *slot, *owner;              /* node -> its slot; slot -> its node */
    struct rg_ent **nb;             /* per slot: its list, NULL once a row */
    int *nnb;
    unsigned long long **row;       /* per slot: its row, NULL while a list */
    int *deg;                       /* per NODE: live neighbours */
    int *buf;                       /* rg_merge's copy of a neighbourhood */
};

static void rg_init(struct ra_graph *g, int n)
{
    g->n = n;
    g->words = (n + 63) / 64;
    g->slot = xmalloc((size_t)(n ? n : 1) * sizeof *g->slot);
    g->owner = xmalloc((size_t)(n ? n : 1) * sizeof *g->owner);
    g->nb = xmalloc((size_t)(n ? n : 1) * sizeof *g->nb);
    for (int e = 0; e < n; e++) {
        g->slot[e] = g->owner[e] = e;
        g->nb[e] = xmalloc(RG_ROW * sizeof *g->nb[e]);
    }
    g->nnb = xcalloc((size_t)(n ? n : 1), sizeof *g->nnb);
    g->row = xcalloc((size_t)(n ? n : 1), sizeof *g->row);
    g->deg = xcalloc((size_t)(n ? n : 1), sizeof *g->deg);
    g->buf = xmalloc((size_t)(n ? n : 1) * 2 * sizeof *g->buf);
}

static void rg_free(struct ra_graph *g)
{
    for (int e = 0; e < g->n; e++) {
        free(g->nb[e]); free(g->row[e]);
    }
    free(g->slot); free(g->owner); free(g->nb); free(g->nnb); free(g->row);
    free(g->deg); free(g->buf);
}

/* Are slots a and b neighbours? */
static int rg_slot_has(const struct ra_graph *g, int a, int b)
{
    if (g->row[a])
        return (int)(g->row[a][b >> 6] >> (b & 63) & 1u);
    if (g->row[b])
        return (int)(g->row[b][a >> 6] >> (a & 63) & 1u);
    const struct rg_ent *l = g->nb[a];
    int n = g->nnb[a], x = b;
    if (g->nnb[b] < n) { l = g->nb[b]; n = g->nnb[b]; x = a; }
    for (int k = 0; k < n; k++)
        if (l[k].v == x)
            return 1;
    return 0;
}

/* Do nodes a and b interfere? */
static int rg_has(const struct ra_graph *g, int a, int b)
{
    return rg_slot_has(g, g->slot[a], g->slot[b]);
}

/* The index of the lowest set bit of a nonzero word (de Bruijn). */
static const unsigned char rg_db[64] = {
    0, 1, 48, 2, 57, 49, 28, 3, 61, 58, 50, 42, 38, 29, 17, 4, 62, 55, 59,
    36, 53, 51, 43, 22, 45, 39, 33, 30, 24, 18, 12, 5, 63, 47, 56, 27, 60,
    41, 37, 16, 54, 35, 52, 21, 44, 32, 23, 11, 46, 26, 40, 15, 34, 20, 31,
    10, 25, 14, 19, 9, 13, 8, 7, 6
};
#define RG_LOW(bits) \
    (rg_db[(((bits) & (0ULL - (bits))) * 0x03f79d71b4cb0a89ULL) >> 58])

/* The slots of a row, into out[]; how many. */
static int rg_row_slots(const struct ra_graph *g, const unsigned long long *r,
                        int *out)
{
    int n = 0;
    for (int w = 0; w < g->words; w++)
        for (unsigned long long bits = r[w]; bits; bits &= bits - 1)
            out[n++] = w * 64 + RG_LOW(bits);
    return n;
}

static int rg_popcount(unsigned long long x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

/* Node u's neighbours, as nodes, into out[]; how many. */
static int rg_nbrs(const struct ra_graph *g, int u, int *out)
{
    int s = g->slot[u], n;
    if (!g->row[s]) {
        const struct rg_ent *l = g->nb[s];
        for (n = 0; n < g->nnb[s]; n++)
            out[n] = g->owner[l[n].v];
        return n;
    }
    n = rg_row_slots(g, g->row[s], out);
    for (int k = 0; k < n; k++)
        out[k] = g->owner[out[k]];
    return n;
}

/* Slot a gains neighbour slot v, whose list holds a at index `back` (or
 * none). Returns the new entry's index in a's list, or -1 when a has a
 * row -- already, or now, because the list was full. */
static int rg_put(struct ra_graph *g, int a, int v, int back)
{
    if (!g->row[a] && g->nnb[a] == RG_ROW) {
        g->row[a] = xcalloc((size_t)(g->words ? g->words : 1), sizeof *g->row[a]);
        for (int k = 0; k < g->nnb[a]; k++)
            g->row[a][g->nb[a][k].v >> 6] |= 1ULL << (g->nb[a][k].v & 63);
        free(g->nb[a]);
        g->nb[a] = NULL;
        g->nnb[a] = 0;
    }
    if (g->row[a]) {
        g->row[a][v >> 6] |= 1ULL << (v & 63);
        return -1;
    }
    int k = g->nnb[a]++;
    g->nb[a][k].v = v;
    g->nb[a][k].back = back;
    return k;
}

/* Where slot a's list names slot v: `hint` when it is the place, else a
 * scan. -1 when a has a row. */
static int rg_find(const struct ra_graph *g, int a, int v, int hint)
{
    if (g->row[a])
        return -1;
    if (hint >= 0 && hint < g->nnb[a] && g->nb[a][hint].v == v)
        return hint;
    for (int k = 0; k < g->nnb[a]; k++)
        if (g->nb[a][k].v == v)
            return k;
    return -1;
}

/* Make nodes a and b interfere; nothing if they already do. */
static void rg_add(struct ra_graph *g, int a, int b)
{
    if (a == b)
        return;
    int sa = g->slot[a], sb = g->slot[b];
    if (rg_slot_has(g, sa, sb))
        return;
    int kb = g->row[sb] || g->nnb[sb] == RG_ROW ? -1 : g->nnb[sb];
    int ka = rg_put(g, sa, sb, kb);
    rg_put(g, sb, sa, ka);
    g->deg[a]++;
    g->deg[b]++;
}

/* Slot a no longer has neighbour slot v, named at index k of its list. */
static void rg_drop(struct ra_graph *g, int a, int v, int k)
{
    if (g->row[a]) {
        g->row[a][v >> 6] &= ~(1ULL << (v & 63));
        return;
    }
    int last = --g->nnb[a];
    if (k != last) {
        struct rg_ent m = g->nb[a][last];
        g->nb[a][k] = m;
        if (!g->row[m.v])                   /* m's reverse entry moved too */
            g->nb[m.v][m.back].back = k;
    }
}

/* Merge node y into node x, which do not interfere. A neighbour of both
 * loses one edge; one of either now has x. */
static void rg_merge(struct ra_graph *g, int x, int y)
{
    int big = g->slot[x], small = g->slot[y];
    int dbig = g->deg[x];
    if (g->deg[y] > g->deg[x]) {
        big = g->slot[y]; small = g->slot[x]; dbig = g->deg[y];
    }
    /* small's neighbours, with where small is in each one's list */
    int n = 0, *ns = g->buf, *at = g->buf + g->n;
    if (g->row[small]) {
        n = rg_row_slots(g, g->row[small], ns);
        for (int q = 0; q < n; q++)
            at[q] = -1;
    } else {
        for (int k = 0; k < g->nnb[small]; k++) {
            ns[n] = g->nb[small][k].v;
            at[n++] = g->nb[small][k].back;
        }
    }
    int renamed = 0;
    for (int q = 0; q < n; q++) {
        int v = ns[q];
        int j = rg_find(g, v, small, at[q]);
        if (rg_slot_has(g, big, v)) {          /* had both: one edge goes */
            rg_drop(g, v, small, j);
            g->deg[g->owner[v]]--;
            continue;
        }
        renamed++;
        if (g->row[v]) {                       /* v names big instead */
            g->row[v][small >> 6] &= ~(1ULL << (small & 63));
            g->row[v][big >> 6] |= 1ULL << (big & 63);
            rg_put(g, big, v, -1);
        } else {
            g->nb[v][j].v = big;
            g->nb[v][j].back = rg_put(g, big, v, j);
        }
    }
    free(g->nb[small]); free(g->row[small]);
    g->nb[small] = NULL; g->row[small] = NULL; g->nnb[small] = 0;
    g->slot[x] = big;
    g->owner[big] = x;
    g->owner[small] = -1;
    g->deg[x] = dbig + renamed;
    g->deg[y] = 0;
}

/* The pool positions a register occupies, as a mask of 1 << k for each
 * POOL[k] == r: what the colouring loops computed by walking the pool for
 * every neighbour. */
static int ra_poolmask(const int *pm, int npm, int r)
{
    return r >= 0 && r < npm ? pm[r] : 0;
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
    /* pmask[r]: the pool positions holding register r (ra_poolmask) */
    int npmask = 0;
    for (int k = 0; k < NP; k++)
        if (POOL[k] + 1 > npmask) npmask = POOL[k] + 1;
    int *pmask = xcalloc((size_t)(npmask ? npmask : 1), sizeof *pmask);
    for (int k = 0; k < NP; k++)
        if (POOL[k] >= 0) pmask[POOL[k]] |= 1 << k;

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
     * be unsound across a back-edge. The blocks' live sets, walked
     * backwards (ra_lset_step), drive the crossings and the interference
     * graph below. */
    struct ra_live *lv = ra_live_compute(fn, first, last);
    int nlb = ra_live_nblocks(lv);
    struct ra_lset ls;
    ra_lset_init(&ls, nvr);

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
    char *atcall = xcalloc((size_t)(nins ? nins : 1), 1);
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
        atcall[i] = 1;
    }
    for (int b = nlb - 1; b >= 0; b--) {
        ra_lset_out(&ls, lv, b);
        for (int i = ra_live_block_start(lv, b + 1) - 1;
             i >= ra_live_block_start(lv, b); i--) {
            if (atcall[i]) {
                const struct ir_asm *ia = fn->ins[i].op == IR_ASM
                                          ? fn->ins[i].asm_ir : NULL;
                int asm_here = ia && t->asm_in_reg && !fp && !ia->cont;
                unsigned long clob = asm_here ? ia->clob : 0;
                int dv = ra_ins_def(&fn->ins[i]);
                for (int k = 0; k < ls.n; k++) {   /* live after the call */
                    int v = ls.mem[k];
                    if (v != dv) {
                        if (clob) forbid[v] |= clob;
                        else      crosses[v] = 1;
                    }
                }
                /* An asm output written through an address reads that
                 * address AFTER the template has run, inside the same
                 * instruction: it has to survive the asm as a value live
                 * across it does. */
                if (asm_here)
                    for (int k = 0; k < ia->nout; k++) {
                        const struct ir_asm_op *o = &ia->out[k];
                        if (o->val || o->mem || o->temp < 0 || o->temp >= nvr)
                            continue;
                        if (clob) forbid[o->temp] |= clob;
                        else      crosses[o->temp] = 1;
                    }
            }
            ra_lset_step(&ls, &fn->ins[i]);
        }
    }
    free(atcall);
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
            /* a raw-slot result where it starts a walk of the frame chain
             * (imm 0: x86-64, AArch64, ColdFire); the level-0 forms (imm 1
             * and 2) are written as each backend writes any result */
            if (in->imm == 0)
                OPAQUE(in->dst);
            break;
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
            if (fn->ret_abi.is_struct ? t->memcpy_addr_in_reg < 2
                : in->flt && !t->float_in_gpr && !t->fp_reads_gpr
                     ? !fp : !t->ret_scalar_in_reg)
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
                if (in->argv[k].is_struct ? t->memcpy_addr_in_reg < 2
                    : in->argv[k].cls[0] == CLASS_SSE && !t->float_in_gpr
                         ? !fp : !t->call_int_arg_in_reg)
                    OPAQUE(in->argv[k].vreg);
            if (in->retsize ? t->memcpy_addr_in_reg < 2
                : in->flt && !t->float_in_gpr ? !fp : 0)
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

    struct ra_graph g;
    rg_init(&g, E);
    int *nbuf = xmalloc((size_t)(E ? E : 1) * sizeof *nbuf);   /* rg_nbrs */
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
    for (int b = E ? nlb - 1 : -1; b >= 0; b--) {
        ra_lset_out(&ls, lv, b);
        for (int i = ra_live_block_start(lv, b + 1) - 1;
             i >= ra_live_block_start(lv, b); i--) {
            struct ir_ins *in = &fn->ins[i];
            int dv = ra_ins_def(in);
            if (dv >= 0 && dv < nvr && eof[dv] >= 0) {
                int src = RA_COPY(in) ? in->a : -1;
                int a = eof[dv];
                for (int k = 0; k < ls.n; k++) {   /* live after the def */
                    int v = ls.mem[k];
                    if (eof[v] < 0 || v == src || v == dv)
                        continue;
                    rg_add(&g, a, eof[v]);
                }
            }
            ra_lset_step(&ls, in);
        }
    }
    if (E && nlb > 0) {                        /* the entry clique */
        int m = 0;
        for (int k = 0; k < ls.n; k++)         /* live into instruction 0 */
            if (eof[ls.mem[k]] >= 0)
                members[m++] = eof[ls.mem[k]];
        for (int p = 0; p < m; p++)
            for (int q = p + 1; q < m; q++)
                rg_add(&g, members[p], members[q]);
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
    /* A list per node, read only as "which partners", so a pair met twice
     * may be listed twice. */
    int **pref = xcalloc((size_t)(E ? E : 1), sizeof *pref);
    int *npref = xcalloc((size_t)(E ? E : 1), sizeof *npref);
    int *cappref = xcalloc((size_t)(E ? E : 1), sizeof *cappref);
    for (int i = 0; E && i < nins; i++) {
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
            if (rg_has(&g, ed, ea)) continue;
            for (int s2 = 0; s2 < 2; s2++) {
                int u = s2 ? ea : ed, w = s2 ? ed : ea;
                if (npref[u] == cappref[u]) {
                    cappref[u] = cappref[u] ? cappref[u] * 2 : 2;
                    pref[u] = xrealloc(pref[u], (size_t)cappref[u] * sizeof *pref[u]);
                }
                pref[u][npref[u]++] = w;
            }
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
    if (E) {
        /* The degrees the Briggs test reads: as they stood before any
         * merge, and after one only the merged node's is brought up to
         * date -- its neighbours keep theirs. */
        int *deg = xmalloc((size_t)E * sizeof *deg);
        for (int e = 0; e < E; e++)
            deg[e] = g.deg[e];
        /* hs[u]: how many of u's neighbours have significant degree
         * (deg >= NP) -- what the test counts, kept as merges change it,
         * so that a node with thousands of neighbours of low degree is
         * not walked at every copy it is a candidate for. A union has at
         * least as many as either side and at most their sum, so only
         * when neither bound decides is it counted out. */
        int *hs = xcalloc((size_t)E, sizeof *hs);
        int *hmark = xcalloc((size_t)E, sizeof *hmark), hstamp = 0;
        int *hn = xmalloc((size_t)E * sizeof *hn);
        char *hc = xmalloc((size_t)E);
        {   /* no merges yet, so slot e is node e: a row is counted
             * against the significant ones a word at a time */
            unsigned long long *sig = xcalloc((size_t)(g.words ? g.words : 1),
                                              sizeof *sig);
            for (int e = 0; e < E; e++)
                if (deg[e] >= NP)
                    sig[e >> 6] |= 1ULL << (e & 63);
            for (int e = 0; e < E; e++) {
                if (g.row[e]) {
                    for (int w = 0; w < g.words; w++)
                        hs[e] += rg_popcount(g.row[e][w] & sig[w]);
                    continue;
                }
                for (int q = 0; q < g.nnb[e]; q++)
                    if (deg[g.nb[e][q].v] >= NP)
                        hs[e]++;
            }
            free(sig);
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
            if (rg_has(&g, x, y)) continue;
            /* Briggs: the union's neighbours of degree >= NP must be
             * fewer than NP -- each counted once, so y's are counted
             * only where x does not have them too. */
            int high = hs[x] > hs[y] ? hs[x] : hs[y];
            if (high < NP && hs[x] + hs[y] >= NP) {
                high = 0;
                for (int s2 = 0; s2 < 2 && high < NP; s2++) {
                    int u = s2 ? y : x, n = rg_nbrs(&g, u, nbuf);
                    for (int q = 0; q < n && high < NP; q++) {
                        int ne = nbuf[q];
                        if (absorbed[ne] || ne == x || ne == y)
                            continue;
                        if (s2 && rg_has(&g, x, ne))
                            continue;
                        if (deg[ne] >= NP)
                            high++;
                    }
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
            /* merge y into x: y's neighbours lose it and gain x. The
             * neighbours of the side whose list rg_merge walks are noted
             * first, with whether they are common to both. */
            int sm = g.deg[y] > g.deg[x] ? x : y;
            int bg = sm == x ? y : x, nh = 0, inter = 0;
            int ox = deg[x] >= NP, oy = deg[y] >= NP;
            hstamp++;
            int nsm = rg_nbrs(&g, sm, nbuf);
            for (int q = 0; q < nsm; q++) {
                int ne = nbuf[q];
                hn[nh] = ne;
                hc[nh] = (char)rg_has(&g, bg, ne);
                if (hc[nh] && deg[ne] >= NP)
                    inter++;
                hmark[ne] = hstamp;
                nh++;
            }
            rg_merge(&g, x, y);
            int nx = g.deg[x] >= NP;
            hs[x] = hs[x] + hs[y] - inter;
            hs[y] = 0;
            for (int k = 0; k < nh; k++)
                hs[hn[k]] += nx - (hc[k] ? ox + oy : (sm == x ? ox : oy));
            if (nx != (bg == x ? ox : oy)) {     /* the other side's, at once */
                int nxn = rg_nbrs(&g, x, nbuf);
                for (int q = 0; q < nxn; q++) {
                    int ne = nbuf[q];
                    if (hmark[ne] != hstamp)
                        hs[ne] += nx - (bg == x ? ox : oy);
                }
            }
            alias[y] = x;
            absorbed[y] = 1;
            xcross[x] |= xcross[y];
            xforbid[x] |= xforbid[y];
            if (ndep[y] > ndep[x]) ndep[x] = ndep[y];
            if (ehint[x] < 0) ehint[x] = ehint[y];
            deg[x] = g.deg[x];
            deg[y] = 0;
        }
        free(deg); free(hs); free(hmark); free(hn); free(hc);
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
        for (int e = 0; e < E; e++)
            deg[e] = g.deg[e];
        char *gone = xcalloc((size_t)(E ? E : 1), 1);
        /* The node the scan wants is the LOWEST-numbered trivially
         * colourable one, and a node stays trivially colourable once it
         * is (degrees only fall), so they wait in a min-heap: entered when
         * their degree drops under the bound, taken smallest first. Finding
         * each by scanning every node was nodes x nodes -- a function of
         * 4000 statements has 8000 to 25000 of them. The spill choice keeps
         * its scan, in index order over the nodes still in the graph, so
         * its ties go where they always went. */
        int *heap = xmalloc((size_t)(E ? E : 1) * sizeof *heap), nheap = 0;
        char *inheap = xcalloc((size_t)(E ? E : 1), 1);
        int *rest = xmalloc((size_t)(E ? E : 1) * sizeof *rest), nrest = 0;
        for (int e = 0; e < E; e++)
            if (!absorbed[e]) {
                rest[nrest++] = e;
                if (deg[e] < (xcross[e] && !pool_k ? NCALLEE : NP)) {
                    ra_heap_push(heap, &nheap, e);
                    inheap[e] = 1;
                }
            }
        int sp = 0;
        for (int cnt = 0; cnt < E; cnt++) {
            int pick = -1;
            if (nheap > 0)                       /* a trivially-colourable node */
                pick = ra_heap_pop(heap, &nheap);
            if (pick < 0) {                      /* else the cheapest to spill */
                int j = 0;
                for (int r = 0; r < nrest; r++) {
                    int e = rest[r];
                    if (gone[e])
                        continue;
                    rest[j++] = e;
                    if (pick < 0 ||
                        (by_degree
                             ? deg[e] > deg[pick]
                             : (unsigned long long)cost[e] * deg[pick] * deg[pick] <
                               (unsigned long long)cost[pick] * deg[e] * deg[e]))
                        pick = e;
                }
                nrest = j;
            }
            if (pick < 0) break;                 /* only absorbed nodes left */
            gone[pick] = 1;
            order[sp++] = pick;                  /* push */
            int npk = rg_nbrs(&g, pick, nbuf);
            for (int q = 0; q < npk; q++) {
                int ne = nbuf[q];
                if (absorbed[ne] || gone[ne])
                    continue;
                deg[ne]--;
                if (!inheap[ne] &&
                    deg[ne] < (xcross[ne] && !pool_k ? NCALLEE : NP)) {
                    ra_heap_push(heap, &nheap, ne);
                    inheap[ne] = 1;
                }
            }
        }
        free(heap); free(inheap); free(rest);
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
    /* ...and the instruction a node is BORN at, when it begins with the
     * definition there and nothing else of it is live into that
     * instruction: a reserved range marked `born` that ends there does
     * not take its register from it. -1 for every other node. */
    int *nborn = xmalloc((size_t)(E ? E : 1) * sizeof *nborn);
    for (int e = 0; e < E; e++) {
        int at = nfirst[e], d = at < nins ? ra_ins_def(&fn->ins[at]) : -1;
        nborn[e] = g_ra_nres && d >= 0 && d < nvr && eof[d] >= 0 &&
                   ra_find(alias, eof[d]) == e ? at : -1;
    }
    if (g_ra_nres)
        for (int v = 0; v < nvr; v++) {
            if (eof[v] < 0 || first[v] < 0) continue;
            int e = ra_find(alias, eof[v]);
            if (nborn[e] >= 0 && first[v] == nborn[e] &&
                v != ra_ins_def(&fn->ins[nborn[e]]))
                nborn[e] = -1;
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
            if (g_ra_res[r].last >= nfirst[e] && g_ra_res[r].first <= nlast[e] &&
                !(g_ra_res[r].born && nborn[e] == g_ra_res[r].last))
                for (int k = 0; k < NP; k++)
                    if (POOL[k] == g_ra_res[r].reg) taken |= 1 << k;
        int ne_n = rg_nbrs(&g, e, nbuf);
        for (int q = 0; q < ne_n; q++) {
            int ne = nbuf[q];
            if (absorbed[ne])
                continue;
            int nl = loc[eidx[ne]];
            if (nl >= 0)
                taken |= ra_poolmask(pmask, npmask, nl);
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
        for (int q = 0; q < npref[e]; q++) {
            int pl = loc[eidx[pref[e][q]]];
            if (pl >= 0)
                want |= ra_poolmask(pmask, npmask, pl) & ~taken;
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
            for (int q = 0; q < ne_n; q++) {     /* nbuf still holds them */
                int ne = nbuf[q];
                if (!absorbed[ne] && loc[eidx[ne]] < 0 && ehint[ne] >= 0)
                    avoid |= ra_poolmask(pmask, npmask, ehint[ne]);
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
    free(ndep); free(idepth); free(nfirst); free(nlast); free(nborn);
    g_ra_res = NULL; g_ra_nres = 0;          /* consumed */
    free(cost);
    free(first); free(last); free(elig); free(crosses); free(forbid);
    free(eof); free(eidx);
    rg_free(&g);
    free(nbuf);
    for (int e = 0; e < E; e++) free(pref[e]);
    free(pref); free(npref); free(cappref);
    free(pmask);
    free(members); free(order);
    ra_live_free(lv);
    ra_lset_free(&ls);
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
            op == IR_RET || op == IR_UD2 || op == IR_SWITCH || op == IR_IGOTO)
            b++;
    }

    /* [first,last] instruction index over every appearance of each temp. */
    int *first = xmalloc((size_t)ntemp * sizeof *first);
    int *last  = xmalloc((size_t)ntemp * sizeof *last);
    for (int k = 0; k < ntemp; k++) { first[k] = -1; last[k] = -1; }
    for (int i = 0; i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        int vs[4]; int nv = 0;
        vs[nv++] = in->dst; vs[nv++] = in->a; vs[nv++] = in->b;
        /* `c` names a value only for these (src/opt/util.c, each_read): elsewhere
         * it is 0 -- irgen's emit and the optimizer's ins_blank leave it
         * there -- or a vector shift's constant count. Read regardless,
         * it made every instruction a reference to vreg 0, which in a
         * function with no locals or parameters is a temp: `double m(void)
         * { return 2.0 * 3.0; }` kept a 16-byte frame on x86-64 for it. */
        if (in->op == IR_CMPXCHG || in->op == IR_CAS || in->op == IR_CAS16 ||
            in->op == IR_SELECT)
            vs[nv++] = in->c;
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
    case IR_MULH: case IR_MULW:     /* 32-bit operands at either width */
        return 1;
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
                   int max_size, const char *wide, int regoff, int short_k)
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
    /* The loads and stores whose address each vreg is, in a list per
     * vreg: what the checks and the rewrite below look at for one ADD,
     * found once instead of by a pass over the function per ADD -- which
     * a function storing 8000 struct fields paid 8000 times. A list is
     * read only for an ADD whose result nothing else reads, and no fold
     * moves an access onto such a result, so the lists stay true. */
    int *ahead = xmalloc((size_t)nv * sizeof *ahead);
    int *anext = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *anext);
    for (int v = 0; v < nv; v++) ahead[v] = -1;
    for (int n = fn->nins - 1; n >= 0; n--) {
        const struct ir_ins *u = &fn->ins[n];
        anext[n] = -1;
        if ((u->op == IR_LOAD || u->op == IR_STORE) && u->a >= 0 && u->a < nv) {
            anext[n] = ahead[u->a];
            ahead[u->a] = n;
        }
    }
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
    /* `(base + K) + i` is `(base + i) + K`, and then K is the accesses'
     * offset: `p->a[i]` with the array 0x44 into the struct was
     * `addw r0, r4, #0x44; add.w r1, r0, r7, lsl #2; ldr r0, [r1]`
     * where `add.w r1, r4, r7, lsl #2; ldr r0, [r1, #0x44]` does. When
     * the sum is only ever an address, and the constant ADD has one use
     * (this one) and a base with one definition -- so the base holds
     * the same value here -- the sum takes the base and the accesses
     * the constant; the constant ADD is left dead.
     *
     * Where an access can add a register itself (Thumb's and aarch64's
     * [rn, rm, lsl #s]), one access is as short either way, so only an
     * address two or more accesses share is rewritten -- `t->a[i] |= v`
     * -- and on Thumb only where the offset stays in the two-byte
     * ldr/str's reach: `t->state[i]` 84 bytes in was `adds; ldrb [r1,
     * r0]` and would be `adds; ldrb.w [r0, #84]`. */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        int q = i->dst;
        if (i->op != IR_ADD || i->imm_b || i->flt || i->w != w_addr ||
            q < fn->nvars || q >= nv || defs[q] != 1 || !uses[q] ||
            uses[q] != addr_uses[q] || (regoff && uses[q] < 2))
            continue;
        for (int side = 0; side < 2; side++) {
            int x = side ? i->b : i->a, y = side ? i->a : i->b;
            if (x < fn->nvars || x >= nv || defs[x] != 1 || uses[x] != 1 ||
                y < 0 || y >= nv || x == y)
                continue;
            int dx = -1;
            for (int m = 0; m < n; m++)
                if (fn->ins[m].dst == x && ra_ins_def(&fn->ins[m]) == x) {
                    dx = m;
                    break;
                }
            if (dx < 0)
                continue;
            const struct ir_ins *a = &fn->ins[dx];
            int base = a->a;
            long k = a->imm;
            if (a->op != IR_ADD || !a->imm_b || a->flt || a->w != w_addr ||
                base < 0 || base >= nv || defs[base] != 1 || base == q)
                continue;
            int ok = 1;
            for (int m = 0; m < fn->nins && ok; m++) {
                const struct ir_ins *u = &fn->ins[m];
                if ((u->op == IR_LOAD || u->op == IR_STORE) && u->a == q) {
                    long off = (long)u->memoff + k;
                    if (off < lo || off > hi - u->size ||
                        (short_k && off > (long)short_k * u->size))
                        ok = 0;
                }
            }
            if (!ok)
                continue;
            for (int m = 0; m < fn->nins; m++) {
                struct ir_ins *u = &fn->ins[m];
                if ((u->op == IR_LOAD || u->op == IR_STORE) && u->a == q)
                    u->memoff += (int)k;
            }
            if (side) i->b = base; else i->a = base;
            uses[x]--;
            uses[base]++;
            drop[dx] = 1;
            changed = 1;
            break;
        }
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int p = i->dst, s = i->a;
        long k = i->imm;
        if (drop[n] || i->op != IR_ADD || !i->imm_b || i->flt ||
            i->w != w_addr || p < fn->nvars || p >= nv || s < 0 || s >= nv ||
            s == p || defs[p] != 1 || uses[p] != addr_uses[p] || !uses[p])
            continue;
        int ok = 1;
        for (int m = ahead[p]; m >= 0 && ok; m = anext[m]) {
            const struct ir_ins *u = &fn->ins[m];
            long off = (long)u->memoff + k;
            if (off < lo || off > hi - u->size) ok = 0;
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
        for (int m = ahead[p]; m >= 0; m = anext[m]) {
            struct ir_ins *u = &fn->ins[m];
            u->a = s;
            u->memoff += (int)k;
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
    free(ahead); free(anext);
    return changed;
}
