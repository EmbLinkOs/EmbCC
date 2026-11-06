/* Interprocedural constants (-O3): a parameter every call gives the same
 * constant is that constant inside the function.
 *
 *   static void filt(int n, int wrap) { ... j = (j + n) % n; ... }
 *   ... filt(128, w) ... filt(128, !w) ...
 *
 * `n` is 128 on every call, so in filt it is 128: the division by n
 * becomes a mask and the loop's trip count is known, whether or not filt
 * is inlined. The callee is not copied -- it is the one body every call
 * already reaches -- so it is the same size or smaller; the calls still
 * pass the argument, and the callee no longer reads it.
 *
 * Which functions. Only one whose every call this unit can see: static,
 * defined here, its address never taken (an FADDR anywhere, or a static
 * initializer's relocation), no alias naming it, not `used` and not
 * named by top-level asm (asm may call it), not weak, not a constructor,
 * a destructor, an interrupt handler or naked, and not variadic. Every
 * call is direct and passes as many arguments as it has parameters.
 *
 * Which parameters. An integer or a pointer of at most eight bytes, held
 * in a scalar frame slot that is not volatile, to which every call
 * passes an IR_CONST of one value and width -- found as the argument's
 * definition in the caller's own block. The callee then starts with
 * `%t = const C; stvar param, %t`, which the per-function passes see as
 * any other constant store to a local: mem2reg and SCCP do the rest.
 *
 * It runs before the inliner, so a call it is inlined into gets the
 * constant too, and only at -O3: -O2's code is the code its goldens and
 * measurements describe. */
#include "opt_int.h"

/* The constant an argument vreg holds at instruction `at`, or NULL: its
 * definition, the nearest before `at` in the same block, an IR_CONST. */
static const struct ir_ins *ipcp_arg(const struct ir_func *fn, int at, int v)
{
    for (int m = at - 1; m >= 0; m--) {
        const struct ir_ins *i = &fn->ins[m];
        if (i->op == IR_LABEL || i->op == IR_JMP || i->op == IR_BRZ ||
            i->op == IR_BRNZ || i->op == IR_RET || i->op == IR_SWITCH ||
            i->op == IR_IGOTO || i->op == IR_UD2)
            return NULL;
        if (def_target(i) == v)
            return i->op == IR_CONST && !i->flt && !i->imm_b ? i : NULL;
    }
    return NULL;
}

/* One parameter's verdict across the calls: -1 not yet seen, 1 the same
 * constant so far, 0 not one */
struct ipcp_p { int state; long imm; int w; };

/* A function that may be changed, and what its calls have said */
struct ipcp_c {
    const struct func *src;
    int k;                 /* its index in the unit */
    int ncalls, escape;
    struct ipcp_p *P;
};

static int ipcp_cmp(const void *a, const void *b)
{
    unsigned long x = (unsigned long)((const struct ipcp_c *)a)->src;
    unsigned long y = (unsigned long)((const struct ipcp_c *)b)->src;
    return x < y ? -1 : x > y;
}

static struct ipcp_c *ipcp_find(struct ipcp_c *C, int n, const struct func *f)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (C[mid].src == f)
            return &C[mid];
        if ((unsigned long)C[mid].src < (unsigned long)f)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static int ipcp_func_ok(const struct ir_func *cf)
{
    const struct func *c = cf->src;
    return c && cf->nins > 0 && c->is_static && !c->is_weak && !c->attr_used &&
           !c->is_ctor && !c->is_dtor && !c->is_isr && !c->is_naked &&
           !c->cmse_entry && !c->is_varargs && !c->inline_only &&
           !c->asm_name && !c->is_root &&           /* named by top-level asm */
           cf->nparams > 0 && cf->nparams <= cf->nvars && cf->locals;
}

/* The constants, ahead of the function's first instruction */
static void ipcp_apply(struct ir_func *cf, const struct ipcp_c *c)
{
    struct ibuf nb = { 0, 0, 0 };
    int line = cf->ins[0].line ? cf->ins[0].line : cf->line;
    for (int p = 0; p < cf->nparams; p++) {
        if (c->P[p].state != 1)
            continue;
        int t = cf->nvregs++;
        struct ir_ins *q = ib_push(&nb);
        q->op = IR_CONST; q->dst = t; q->imm = c->P[p].imm; q->w = c->P[p].w;
        q->line = line; q->synth = 1;
        q = ib_push(&nb);
        q->op = IR_STVAR; q->dst = p; q->a = t;
        q->size = cf->locals[p].size;
        q->line = line; q->synth = 1;
        if (remarks_on() && cf->src)
            remark_add("ipcp", "propagated", cf->name, "same-constant",
                       cf->file, line, "parameter %d is %ld on all %d calls",
                       p + 1, c->P[p].imm, c->ncalls);
    }
    int nadd = nb.n;
    for (int i = 0; i < cf->nins; i++)
        *ib_push(&nb) = cf->ins[i];
    /* every instruction index moved up by nadd */
    if (cf->var_scope_lo)
        for (int v = 0; v < cf->nvars; v++) {
            if (cf->var_scope_lo[v] > 0) cf->var_scope_lo[v] += nadd;
            if (cf->var_scope_hi[v] >= 0) cf->var_scope_hi[v] += nadd;
        }
    free(cf->ins);
    cf->ins = nb.p; cf->nins = nb.n; cf->cap = nb.cap;
}

/* One look at the unit: the candidates indexed by their source node, and
 * every call, address and initializer visited once -- a whole program
 * under --lto has thousands of functions, and a scan of the unit per
 * candidate was functions times instructions. */
int pass_ipcp(struct ir_unit *iu)
{
    int n = 0;
    struct ipcp_c *C = xmalloc((size_t)(iu->nfuncs ? iu->nfuncs : 1) * sizeof *C);
    for (int k = 0; k < iu->nfuncs; k++) {
        struct ir_func *cf = &iu->funcs[k];
        if (!ipcp_func_ok(cf))
            continue;
        struct ipcp_c *c = &C[n++];
        c->src = cf->src;
        c->k = k;
        c->ncalls = c->escape = 0;
        c->P = xmalloc((size_t)cf->nparams * sizeof *c->P);
        for (int p = 0; p < cf->nparams; p++) {
            const struct ir_local *l = &cf->locals[p];
            const struct type *t = cf->src->var_tys ? cf->src->var_tys[p] : NULL;
            int ok = l->is_scalar_int_or_ptr && !l->is_volatile && l->size <= 8 &&
                     t && (ty_is_integer(t) || t->kind == TY_PTR);
            c->P[p].state = ok ? -1 : 0;
            c->P[p].imm = 0;
            c->P[p].w = 0;
        }
    }
    if (!n) {
        free(C);
        return 0;
    }
    qsort(C, (size_t)n, sizeof *C, ipcp_cmp);
    /* an alias names it, or its address is in a static initializer -- a
     * handler table, a function pointer's initial value: a call this
     * unit's code never shows (`static int (*fp)(int) = f;`, `fp(9)`) */
    for (int f = 0; f < iu->nfuncs; f++) {
        const struct func *o = iu->funcs[f].src;
        if (!o || !o->alias_of)
            continue;
        for (int j = 0; j < n; j++)
            if (C[j].src->name && !strcmp(o->alias_of, C[j].src->name))
                C[j].escape = 1;
    }
    if (iu->src)
        for (const struct global *g = iu->src->globals; g; g = g->next)
            for (int r = 0; r < g->nrelocs; r++) {
                struct ipcp_c *c = g->relocs[r].ftarget
                    ? ipcp_find(C, n, g->relocs[r].ftarget) : NULL;
                if (c)
                    c->escape = 1;
            }
    for (int f = 0; f < iu->nfuncs; f++) {
        const struct ir_func *fn = &iu->funcs[f];
        for (int i = 0; i < fn->nins; i++) {
            const struct ir_ins *in = &fn->ins[i];
            if (in->op != IR_FADDR && in->op != IR_CALL)
                continue;
            struct ipcp_c *c = in->callee ? ipcp_find(C, n, in->callee) : NULL;
            if (!c || c->escape)
                continue;
            int np = iu->funcs[c->k].nparams;
            if (in->op == IR_FADDR || in->indirect || in->nargs != np ||
                !in->argv) {
                c->escape = 1;
                continue;
            }
            c->ncalls++;
            for (int p = 0; p < np; p++) {
                if (!c->P[p].state)
                    continue;
                const struct ir_ins *a = in->argv[p].is_struct ? NULL
                                       : ipcp_arg(fn, i, in->argv[p].vreg);
                if (!a)
                    c->P[p].state = 0;
                else if (c->P[p].state < 0) {
                    c->P[p].state = 1;
                    c->P[p].imm = a->imm;
                    c->P[p].w = a->w;
                } else if (a->imm != c->P[p].imm || a->w != c->P[p].w)
                    c->P[p].state = 0;
            }
        }
    }
    int changed = 0;
    for (int j = 0; j < n; j++) {
        struct ipcp_c *c = &C[j];
        int nk = 0;
        for (int p = 0; p < iu->funcs[c->k].nparams; p++)
            nk += c->P[p].state == 1;
        if (nk && !c->escape && c->ncalls) {
            ipcp_apply(&iu->funcs[c->k], c);
            changed = 1;
        }
        free(c->P);
    }
    free(C);
    return changed;
}
