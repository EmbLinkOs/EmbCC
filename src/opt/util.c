/* What every pass reads the IR with: which operations write a temp or
 * are pure, the walkers over an instruction's operands and labels, each
 * vreg's definitions (compute_defs), the constant a vreg is known to
 * hold, and the instruction buffer a pass rebuilds fn->ins into. */

/* ---- op classification ---- */

#include "opt_int.h"

/* Writes a fresh temporary as dst. IR_STVAR writes a LOCAL slot and is
 * handled apart; every other dst-writer produces a single-assignment temp. */
int writes_temp(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV:
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
    case IR_FADDR: case IR_LOAD: case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_CALL: case IR_XCHG:
    case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS: case IR_CAS16:
    case IR_FRAMEADDR: case IR_ALLOCA: case IR_SPSAVE:
    case IR_SELECT: case IR_MULH: case IR_MULW:
    /* `dst = &&label` defines dst. It was missing here for as long as
     * the optimizer skipped every function that used one, which meant
     * nothing counted it as a definition -- so the verifier reported
     * the jump that read it as reading a temp nothing writes, and was
     * right. */
    case IR_LABELADDR:
    /* A vector result is a temp like any other, 16 bytes wide. */
    case IR_VLOAD: case IR_VBIN: case IR_VSPLAT: case IR_VREDADD:
    case IR_VWIDEN:
        return 1;
    default:
        return 0;
    }
}

/* No side effect and cannot fault, so removable when its result is unused.
 * DIV/MOD (÷0 traps), LOAD (may fault or be volatile), CALL (effects), and
 * every store/branch/label are deliberately NOT pure. */
int is_pure(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV:
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
    case IR_FADDR: case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_SELECT:        /* both arms are values; it cannot trap */
    case IR_LABELADDR:     /* the address of a label is a constant */
    case IR_MULH: case IR_MULW:
        return 1;
    default:
        return 0;
    }
}

/* The vreg an instruction assigns (a temp, or a local for IR_STVAR); -1 if
 * it assigns nothing. */
int def_target(const struct ir_ins *i)
{
    if (i->op == IR_STVAR)
        return i->dst;
    if (writes_temp(i->op))
        return i->dst;
    /* An asm's value output (ir_asm_op.val), as a call's result. Not in
     * writes_temp: two asms are never the same computation, however
     * alike they look, so nothing may value-number one. */
    if (i->op == IR_ASM)
        return i->dst;
    return -1;
}

/* Visit &slot for every LABEL an instruction names: a branch's target, a
 * label address, a switch's default and each entry of its table. The one
 * place that knows where they all are, so a pass that retargets or counts
 * them cannot miss the table -- which lives in the function, not the
 * instruction, and is why this takes fn. */
void each_label(struct ir_func *fn, struct ir_ins *i,
                void (*cb)(int *, void *), void *ctx)
{
    switch (i->op) {
    case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_LABELADDR:
        cb(&i->label, ctx);
        break;
    case IR_SWITCH:
        cb(&i->label, ctx);
        for (int k = 0; k < fn->jt[i->jt].n; k++)
            cb(&fn->jt[i->jt].labels[k], ctx);
        break;
    default:
        break;
    }
}
void retarget_cb(int *p, void *ctx)
{
    struct retarget *r = ctx;
    if (*p == r->from) *p = r->to;
}

/* Visit &field for every vreg this instruction READS (never its dst).
 *
 * IR_LDVAR's and IR_ADDR's `a` name a FRAME SLOT, and they are here
 * anyway, which looks like a bug and is not: irgen starts temp numbering
 * at nvars (`fn->nvregs = f->nvars`), so slots and temps share one space
 * with the slots first. A slot index IS a vreg number.
 *
 * What makes rewriting them safe is the other half of that invariant:
 * every dst a pass propagates is a TEMP, so it is >= nvars and cannot
 * equal a slot. writes_temp() is where that holds -- IR_STVAR, the one
 * op whose dst is a slot, is deliberately not in it. Break either half
 * and copy propagation silently starts loading a different variable, so
 * a probe went looking first: over libc and libcxx on four targets and
 * the whole suite, copy propagation never once met a slot index equal to
 * the vreg it was replacing. */
void each_read(struct ir_ins *i, void (*cb)(int *, void *), void *ctx)
{
    switch (i->op) {
    case IR_MOV: case IR_NEG: case IR_BNOT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_LDVAR: case IR_ADDR: case IR_LOAD:
    case IR_MEMZERO: case IR_VA_START: case IR_STVAR:
    case IR_ALLOCA: case IR_SPRESTORE:
    /* `goto *p` reads p. It was missing here, and could not be noticed
     * while every function using one was skipped by the optimizer
     * whole: dead-code elimination saw no reader, dropped the load of
     * the label address, and left the jump reading a temp nothing
     * writes. Every visitor built on each_read -- the use counts, the
     * inliner's remap, the verifier -- was blind to it the same way. */
    case IR_IGOTO:
    case IR_SWITCH:               /* the table index */
        cb(&i->a, ctx);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP:
        cb(&i->a, ctx);
        if (!i->imm_b)           /* b folded to an immediate: not a vreg read */
            cb(&i->b, ctx);
        break;
    case IR_STORE: case IR_MEMCPY: case IR_XCHG:
    case IR_XADD: case IR_ARMW:
    case IR_VSTORE:
    case IR_MULH: case IR_MULW:     /* never an immediate b */
        cb(&i->a, ctx);
        cb(&i->b, ctx);
        break;
    case IR_VBIN:
        cb(&i->a, ctx);
        /* A lane-wise shift's count is a constant in `c`, so there is
         * no second vreg and `b` is -1. Reporting it anyway had DCE
         * counting a use of vreg -1 and incrementing the word BEFORE
         * its array -- which faults only when that word happens to sit
         * on an unmapped page, so it showed up as a compiler crash in
         * four of seventy-two parallel builds and never once on its
         * own. */
        if (i->b >= 0)
            cb(&i->b, ctx);
        break;
    case IR_VLOAD: case IR_VSPLAT: case IR_VREDADD: case IR_VWIDEN:
        cb(&i->a, ctx);
        break;
    case IR_CMPXCHG: case IR_CAS: case IR_CAS16: case IR_SELECT:
        cb(&i->a, ctx);
        cb(&i->b, ctx);
        cb(&i->c, ctx);
        break;
    case IR_BRZ: case IR_BRNZ:
        cb(&i->a, ctx);
        break;
    case IR_RET:
        if (i->a >= 0)
            cb(&i->a, ctx);
        break;
    case IR_CALL:
        if (i->indirect)
            cb(&i->a, ctx);
        for (int k = 0; k < i->nargs; k++)
            cb(&i->argv[k].vreg, ctx);
        break;
    case IR_ASM:
        for (int k = 0; k < i->asm_ir->nin; k++)
            cb(&i->asm_ir->in[k].temp, ctx);
        /* an output's lvalue ADDRESS is read; a `val` one is the dst */
        for (int k = 0; k < i->asm_ir->nout; k++)
            if (!i->asm_ir->out[k].val)
                cb(&i->asm_ir->out[k].temp, ctx);
        break;
    default:
        break;   /* CONST, STRADDR, GADDR, FADDR, LABEL, JMP: no reads */
    }
}

void opnd_cb(int *p, void *ctx)
{
    struct opnds *o = ctx;
    if (o->n < (int)(sizeof o->v / sizeof o->v[0]))
        o->v[o->n++] = *p;
    else
        o->over = 1;
}
void value_opnds(struct ir_ins *i, struct opnds *o)
{
    o->n = 0; o->over = 0;
    if (i->op == IR_LDVAR || i->op == IR_ADDR)
        return;
    each_read(i, opnd_cb, o);
}

/* Does this instruction read vreg `v` as a value? Asked directly rather
 * than through value_opnds, because a call has as many operands as it
 * has arguments and the question is still answerable -- refusing to
 * answer it for anything with a call in it would give up on most
 * functions worth optimizing. */
struct findv { int v, found; };
static void findv_cb(int *p, void *ctx)
{
    struct findv *f = ctx;
    if (*p == f->v)
        f->found = 1;
}
int ins_reads(struct ir_ins *i, int v)
{
    if (i->op == IR_LDVAR || i->op == IR_ADDR)
        return 0;                    /* a frame slot, not a value operand */
    struct findv f = { v, 0 };
    each_read(i, findv_cb, &f);
    return f.found;
}

/* ---- def analysis ---- */

/* A copied instruction that keeps its original: its own argument array.
 * argv is out of line (ir.h), so a struct copy shares it, and renaming
 * the copy's operands -- what a pass that duplicates code does next --
 * rewrote the original's too. Fuzz seed 5023: switch threading copied a
 * block with a call and renamed the copy, and the original call passed
 * the copy's values (all four boards, -O2). */
void ins_own_args(struct ir_ins *q)
{
    if (q->op == IR_CALL && q->argv)
        q->argv = ir_args_copy(q->argv, q->nargs);
}

static int cmp_args_ptr(const void *a, const void *b)
{
    const struct ir_ins *x = *(const struct ir_ins *const *)a;
    const struct ir_ins *y = *(const struct ir_ins *const *)b;
    uintptr_t p = (uintptr_t)x->argv, r = (uintptr_t)y->argv;
    return p < r ? -1 : p > r ? 1 : (x < y ? -1 : x > y);
}

/* ...and wherever a pass left two calls sharing one anyway, the second
 * gets its own before anything renames it: every pass that rewrites
 * operands starts from compute_defs, so this runs ahead of them all. The
 * order is by address only to find the pairs; which of two equal arrays
 * is copied does not change the program. */
static void unshare_call_args(struct ir_func *fn)
{
    int nc = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_CALL && fn->ins[n].argv)
            nc++;
    if (nc < 2)
        return;
    struct ir_ins **pv = xmalloc((size_t)nc * sizeof *pv);
    nc = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_CALL && fn->ins[n].argv)
            pv[nc++] = &fn->ins[n];
    qsort(pv, (size_t)nc, sizeof *pv, cmp_args_ptr);
    for (int k = 1; k < nc; k++)
        if (pv[k]->argv == pv[k - 1]->argv)
            ins_own_args(pv[k]);
    free(pv);
}

void compute_defs(struct ir_func *fn, struct defs *d)
{
    unshare_call_args(fn);
    d->cnt = xcalloc((size_t)fn->nvregs, sizeof *d->cnt);
    d->ins = xmalloc((size_t)fn->nvregs * sizeof *d->ins);
    d->first = d->next = NULL;
    for (int v = 0; v < fn->nvregs; v++)
        d->ins[v] = -1;
    /* a parameter is bound once at entry — count that as its definition */
    int np = fn->nparams;
    for (int v = 0; v < np && v < fn->nvregs; v++)
        d->cnt[v] = 1;
    for (int n = 0; n < fn->nins; n++) {
        /* IR_LANDING is the one instruction with TWO destinations -- the
         * exception pointer in dst and the selector in b, as the unwinder
         * left them. def_target returns a single index and writes_temp is
         * deliberately silent about it (an instruction in writes_temp is
         * one LVN and GCSE may value-number, and two landing pads are not
         * the same computation however identical they look). So its
         * definitions are counted here, explicitly. Without this its
         * results look undefined and DCE deletes the landing while
         * keeping the stores that read what it produced. */
        if (fn->ins[n].op == IR_LANDING) {
            int o[2] = { fn->ins[n].dst, fn->ins[n].b };
            for (int k = 0; k < 2; k++) {
                int v = o[k];
                if (v < 0 || v >= fn->nvregs)
                    continue;
                if (d->cnt[v]++ == 0)
                    d->ins[v] = n;
                else
                    d->ins[v] = -1;
            }
            continue;
        }
        int t = def_target(&fn->ins[n]);
        if (t < 0)
            continue;
        if (d->cnt[t]++ == 0)
            d->ins[t] = n;
        else
            d->ins[t] = -1;   /* more than one def: not single-assignment */
    }
}

void free_defs(struct defs *d)
{
    free(d->cnt);
    free(d->ins);
    free(d->first);
    free(d->next);
}

/* The definition lists (struct defs' first/next), by def_target. A
 * landing pad's two destinations are not among them, which is why a
 * reader counts what it walks against cnt. */
void defs_lists(struct ir_func *fn, struct defs *d)
{
    if (d->first)
        return;
    d->first = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *d->first);
    d->next = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *d->next);
    for (int v = 0; v < fn->nvregs; v++)
        d->first[v] = -1;
    for (int n = fn->nins - 1; n >= 0; n--) {
        int t = def_target(&fn->ins[n]);
        d->next[n] = -1;
        if (t >= 0 && t < fn->nvregs && fn->ins[n].op != IR_LANDING) {
            d->next[n] = d->first[t];
            d->first[t] = n;
        }
    }
}

/* Operand b as a constant, whether it has been folded into the
 * instruction or is still a CONST temp. pass_immfold runs after the
 * fixpoint this pass sits in, so inside the pipeline it is always the
 * latter -- which is not what the printed IR shows, because that is
 * printed after the fold. Reading only imm_b here found nothing at all. */
int const_b(struct ir_func *fn, struct defs *d, struct ir_ins *i,
            long *out)
{
    if (i->imm_b) { *out = i->imm; return 1; }
    int v = i->b;
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int n = d->ins[v];
    if (n < 0 || fn->ins[n].op != IR_CONST)
        return 0;
    *out = fn->ins[n].imm;
    return 1;
}

/* If vreg v holds an integer constant (single-def IR_CONST, not an SSE
 * bit-pattern), return 1 and its value. */
int get_const(struct ir_func *fn, struct defs *d, int v, long *out)
{
    if (v < 0 || d->cnt[v] != 1 || d->ins[v] < 0)
        return 0;
    struct ir_ins *di = &fn->ins[d->ins[v]];
    if (di->op != IR_CONST || di->flt)
        return 0;
    *out = di->imm;
    return 1;
}

/* A fresh instruction with no operands: what irgen's emit() starts from.
 * The vreg and label fields are -1, not 0 -- slot 0 is a real local, the
 * first parameter, and a field left at 0 names it. A branch built here
 * with dst 0 told AVR's width test the branch WROTE that parameter, so
 * where it was eight bytes a four-byte zero test was lowered over eight
 * registers: past r31 the encoder refused it, short of r31 four bytes of
 * an unrelated value decided the branch. */
void ins_blank(struct ir_ins *i)
{
    memset(i, 0, sizeof *i);
    i->dst = i->a = i->b = -1;
    i->label = -1;
}

/* EMBCC_IBUF_MOVE=1: every push MOVES the buffer and scribbles over the
 * old one. A pointer an earlier push returned is valid only until the
 * next, and holding one across it reads freed memory -- but only when
 * that push happens to grow the buffer, which is how such reads survive
 * ordinary testing. With this every one of them reads garbage at once
 * (tests/golden/ibuf-move.sh). Each push copies the whole buffer, so a
 * compile is many times slower: for testing only. */
static int g_ib_move = -1;

struct ir_ins *ib_push(struct ibuf *b)
{
    if (g_ib_move < 0)
        g_ib_move = getenv("EMBCC_IBUF_MOVE") != NULL;
    if (g_ib_move) {
        struct ir_ins *np = xmalloc((size_t)(b->n + 1) * sizeof *np);
        if (b->n)
            memcpy(np, b->p, (size_t)b->n * sizeof *np);
        if (b->p) {
            memset(b->p, 0xa5, (size_t)b->cap * sizeof *b->p);
            free(b->p);
        }
        b->p = np;
        b->cap = b->n + 1;
    } else if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, (size_t)b->cap * sizeof *b->p);
    }
    struct ir_ins *i = &b->p[b->n++];
    ins_blank(i);
    return i;
}

int ir_result_w(const struct ir_ins *i)
{
    return i->op == IR_CMP ? 4 : i->w;
}

int ir_result_flt(const struct ir_ins *i)
{
    return i->op == IR_CMP ? 0 : i->flt;
}
