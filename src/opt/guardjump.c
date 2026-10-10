/* ==== -Os: enter a rotated loop at its test ==================================
 *
 * Rotation (rotate.c) leaves the loop's test twice: the original as a guard
 * in front of the body, the copy at the bottom.
 *
 *           t = *p                 <- the guard
 *           brz t -> Lexit
 *      Lbody:
 *           p = p + 1
 *           t' = *p                <- the copy
 *           brnz t' -> Lbody
 *      Lexit:
 *
 * The steady state is what rotation is for: one taken branch a trip. The
 * guard is what it costs, and at -Os that was most of the bytes rotation
 * added to lib/libc on ARMv7-M (750 of them). Jumping to the copy keeps
 * the steady state and drops the guard -- the form GCC gives a loop at -Os:
 *
 *           jmp Lt
 *      Lbody:
 *           p = p + 1
 *      Lt:  t' = *p
 *           brnz t' -> Lbody
 *      Lexit:
 *
 * It is the same computation only when the guard IS the copy: the longest
 * run of instructions ending at the guard's branch that matches the run
 * ending at the latch's, operand for operand, where an operand the guard
 * defines corresponds to the one the copy defines. And the guard's values
 * must not be read anywhere else (the copy writes its own names, not the
 * guard's), unless the two are the same name; a name only the copy writes
 * must be written nowhere else, so that writing it once more, on the way
 * in, disturbs nothing. Rotation makes this shape; folding the guard
 * (`i = 0; i < n` decided) makes the two differ, and then the guard stays.
 *
 * Only a run of plain computations and loads (not volatile, which is the
 * access itself): the guard runs where the copy now runs, once, on the
 * same values. Runs last, at -Os, on every target (pass_guardjump's
 * caller; written on ARM and gated to it, lib/libc then lost 162-504 bytes
 * on each of RV32, RV64, AVR, x86-64 and AArch64 when the gate went):
 * every loop pass is done, and the bottom-tested shape they match on is
 * gone afterwards -- the loop's header is now its test. */

#include "opt_int.h"

static int gj_op_ok(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR:
    case IR_XOR: case IR_SHL: case IR_SHR: case IR_NEG: case IR_BNOT:
    case IR_CMP: case IR_MOV: case IR_EXT: case IR_CONST: case IR_GADDR:
    case IR_STRADDR: case IR_ADDR: case IR_BITCAST:
        return 1;
    case IR_LOAD:
        return !i->vol && !i->flash;
    default:
        return 0;
    }
}

/* The fields an instruction's value depends on, besides its operands. */
int gj_same_fields(const struct ir_ins *g, const struct ir_ins *c)
{
    return g->op == c->op && g->w == c->w && g->size == c->size &&
           g->sign == c->sign && g->flt == c->flt && g->vol == c->vol &&
           g->flash == c->flash && g->imm == c->imm &&
           g->imm_b == c->imm_b && g->pred == c->pred &&
           g->label == c->label && g->callee == c->callee &&
           g->callee_sym == c->callee_sym && g->glob == c->glob &&
           g->glob_sym == c->glob_sym && g->memoff == c->memoff &&
           g->natural == c->natural && g->c == c->c;
}

struct gj_map { const int *tbl; int nv; };
static void gj_map_cb(int *p, void *ctx)
{
    const struct gj_map *m = ctx;
    if (*p >= 0 && *p < m->nv && m->tbl[*p] >= 0)
        *p = m->tbl[*p];
}

struct gj_reads { int *cnt; int nv; };
static void gj_count_cb(int *p, void *ctx)
{
    struct gj_reads *r = ctx;
    if (*p >= 0 && *p < r->nv)
        r->cnt[*p]++;
}

/* Does the guard's run [gs, gb) match the copy's run [cs, cb), and
 * the guard's branch gb the copy's cb? tbl is scratch, nvregs wide,
 * all -1 on entry and on return. */
static int gj_match(struct ir_func *fn, int gs, int gb, int cs, int cb,
                    int *tbl, const int *nreads, const struct defs *d)
{
    int L = gb - gs, ok = 1, nv = fn->nvregs;
    struct gj_map m = { tbl, nv };
    int *gread = xcalloc((size_t)nv, sizeof *gread);
    struct gj_reads gr = { gread, nv };
    for (int k = 0; k < L && ok; k++) {
        struct ir_ins g = fn->ins[gs + k];
        const struct ir_ins *c = &fn->ins[cs + k];
        if (!gj_op_ok(&g) || !gj_same_fields(&g, c)) { ok = 0; break; }
        each_read(&fn->ins[gs + k], gj_count_cb, &gr);
        each_read(&g, gj_map_cb, &m);
        if (g.a != c->a || g.b != c->b) { ok = 0; break; }
        int gd = def_target(&g), cd = def_target(c);
        if (gd < 0 || cd < 0 || gd >= nv || cd >= nv) { ok = 0; break; }
        /* A name only the copy writes must be written there alone. */
        if (gd != cd && d->cnt[cd] != 1) { ok = 0; break; }
        /* Each guard definition maps once: a second write of the same
         * name inside the run would make the table say two things. */
        if (tbl[gd] >= 0 && tbl[gd] != cd) { ok = 0; break; }
        tbl[gd] = cd;
    }
    if (ok) {
        struct ir_ins g = fn->ins[gb];
        each_read(&fn->ins[gb], gj_count_cb, &gr);
        each_read(&g, gj_map_cb, &m);
        if (g.a != fn->ins[cb].a || g.w != fn->ins[cb].w ||
            g.size != fn->ins[cb].size || g.flt != fn->ins[cb].flt)
            ok = 0;
    }
    /* The guard's own values are read only inside the guard -- unless
     * the copy writes the same name, which it then holds either way. */
    for (int k = 0; k < L && ok; k++) {
        int gd = def_target(&fn->ins[gs + k]);
        if (tbl[gd] != gd && gread[gd] != nreads[gd])
            ok = 0;
    }
    for (int k = 0; k < L; k++) {
        int gd = def_target(&fn->ins[gs + k]);
        if (gd >= 0 && gd < nv)
            tbl[gd] = -1;
    }
    free(gread);
    return ok;
}

/* The code between the guard's branch and the body's label -- copies and
 * address set-up the loop passes put on the way in (`p = d` before a
 * pointer walk) -- runs before the jump instead, on the loop-skipped path
 * as well. So it may only compute (gj_pre_ok), must not touch what the
 * test reads or writes, and what it writes must be read only from there
 * to the latch's branch -- the loop itself -- with nothing outside that
 * span jumping into it; and it may not read the guard's values, which
 * the jump no longer computes. */
static int gj_pre_ok(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_MOV: case IR_EXT: case IR_CONST: case IR_GADDR:
    case IR_STRADDR: case IR_ADDR: case IR_BITCAST: case IR_MUL:
        return !i->flt;
    default:
        return 0;
    }
}

struct gj_mark { char *m; int nv; };
static void gj_mark_cb(int *p, void *ctx)
{
    struct gj_mark *g = ctx;
    if (*p >= 0 && *p < g->nv)
        g->m[*p] = 1;
}
struct gj_lab { int *cnt; int nl; };
static void gj_lab_cb(int *p, void *ctx)
{
    struct gj_lab *g = ctx;
    if (*p >= 0 && *p < g->nl)
        g->cnt[*p]++;
}

static int gj_pre_check(struct ir_func *fn, int gs, int gb, int lb, int cb,
                        const int *nreads, const int *labrefs)
{
    int nv = fn->nvregs, ok = 1;
    char *gdef = xcalloc((size_t)nv, 1);
    for (int n = gs; n < gb; n++) {
        int dv = def_target(&fn->ins[n]);
        if (dv >= 0 && dv < nv) gdef[dv] = 1;
    }
    /* Every read of what it writes lies in [gb, cb]. That also keeps it
     * clear of the test: the guard reads what the copy reads, and the
     * guard is outside the span -- and a name the copy writes in place of
     * the guard is written nowhere else (gj_match). */
    int *rin = xcalloc((size_t)nv, sizeof *rin);
    struct gj_reads rr = { rin, nv };
    for (int n = gb + 1; n <= cb; n++)
        each_read(&fn->ins[n], gj_count_cb, &rr);
    char *rd = xcalloc((size_t)nv, 1);
    struct gj_mark rm = { rd, nv };
    for (int n = gb + 1; n < lb && ok; n++) {
        int dv = def_target(&fn->ins[n]);
        memset(rd, 0, (size_t)nv);
        each_read(&fn->ins[n], gj_mark_cb, &rm);
        for (int v = 0; v < nv && ok; v++)
            if (rd[v] && gdef[v])
                ok = 0;                 /* reads the guard's value */
        if (!ok || dv < 0 || dv >= nv || rin[dv] != nreads[dv])
            ok = 0;
    }
    /* nothing outside [gb, cb] jumps into it */
    if (ok) {
        int *inside = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1),
                              sizeof *inside);
        struct gj_lab gl = { inside, fn->nlabels };
        for (int n = gb; n <= cb; n++)
            each_label(fn, &fn->ins[n], gj_lab_cb, &gl);
        for (int n = lb; n <= cb && ok; n++)
            if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
                fn->ins[n].label < fn->nlabels &&
                inside[fn->ins[n].label] != labrefs[fn->ins[n].label])
                ok = 0;
        free(inside);
    }
    free(gdef); free(rin); free(rd);
    return ok;
}

/* Is instruction n inside a loop: between a label and a branch back to
 * it? */
int gj_in_loop(const struct ir_func *fn, int n, const int *labpos)
{
    for (int j = n + 1; j < fn->nins; j++) {
        const struct ir_ins *i = &fn->ins[j];
        if ((i->op == IR_JMP || i->op == IR_BRZ || i->op == IR_BRNZ) &&
            i->label >= 0 && i->label < fn->nlabels &&
            labpos[i->label] >= 0 && labpos[i->label] <= n)
            return 1;
    }
    return 0;
}

int pass_guardjump(struct ir_func *fn)
{
    int nv = fn->nvregs, done = 0;
    if (fn->nins < 4 || nv == 0)
        return 0;
    int *labpos = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *labpos);
    int *labrefs = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), sizeof *labrefs);
    struct gj_lab gl = { labrefs, fn->nlabels };
    for (int l = 0; l < fn->nlabels; l++)
        labpos[l] = -1;
    for (int n = 0; n < fn->nins; n++) {
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < fn->nlabels)
            labpos[fn->ins[n].label] = n;
        each_label(fn, &fn->ins[n], gj_lab_cb, &gl);
    }
    int *nreads = xcalloc((size_t)nv, sizeof *nreads);
    struct gj_reads rr = { nreads, nv };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], gj_count_cb, &rr);
    struct defs d;
    compute_defs(fn, &d);
    int *tbl = xmalloc((size_t)nv * sizeof *tbl);
    for (int v = 0; v < nv; v++)
        tbl[v] = -1;

    for (int gb = 1; gb + 1 < fn->nins && !done; gb++) {
        const struct ir_ins *g = &fn->ins[gb];
        if (g->op != IR_BRZ && g->op != IR_BRNZ)
            continue;
        /* the body's label, after at most a few set-up instructions */
        int lb = gb + 1;
        while (lb < fn->nins && lb - gb <= 8 && gj_pre_ok(&fn->ins[lb]))
            lb++;
        if (lb >= fn->nins || fn->ins[lb].op != IR_LABEL)
            continue;
        int Lbody = fn->ins[lb].label, Lexit = g->label;
        if (Lexit < 0 || Lexit >= fn->nlabels || labpos[Lexit] <= lb + 1)
            continue;
        int cb = labpos[Lexit] - 1;
        const struct ir_ins *c = &fn->ins[cb];
        if (c->op != (g->op == IR_BRZ ? IR_BRNZ : IR_BRZ) ||
            c->label != Lbody)
            continue;
        /* Not a loop inside another: the jump in is a taken branch each
         * time the loop is entered, which for an inner loop is every trip
         * of the outer one -- tools/bench's sort and hash ran 4-9% more
         * cycles at -Os for it. An outermost loop is entered once. */
        if (gj_in_loop(fn, gb, labpos))
            continue;
        /* The longest run that could match: back from both branches,
         * inside the guard's block and the latch's. */
        int max = 0;
        while (gb - max - 1 >= 0 && cb - max - 1 > lb &&
               gj_op_ok(&fn->ins[gb - max - 1]) &&
               gj_op_ok(&fn->ins[cb - max - 1]))
            max++;
        int L = 0;
        for (int k = max; k >= 1 && !L; k--)
            if (gj_match(fn, gb - k, gb, cb - k, cb, tbl, nreads, &d) &&
                gj_pre_check(fn, gb - k, gb, lb, cb, nreads, labrefs))
                L = k;
        if (!L)
            continue;

        /* [set-up] jmp Lt; Lbody: ...; Lt: <the copy's run>; branch */
        int Lt = fn->nlabels++, gs = gb - L, cs = cb - L;
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (n >= gs && n < lb && n != gb)
                continue;
            if (n == gb) {
                for (int q = gb + 1; q < lb; q++)
                    *ib_push(&nb) = fn->ins[q];
                struct ir_ins *j = ib_push(&nb);
                j->op = IR_JMP;
                j->label = Lt;
                j->line = fn->ins[gb].line;
                j->col = fn->ins[gb].col;
                j->synth = 1;
                continue;
            }
            if (n == cs) {
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL;
                l->label = Lt;
                l->line = fn->ins[n].line;
                l->col = fn->ins[n].col;
                l->synth = 1;
            }
            *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[fn->nins] = nb.n;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p;
        fn->nins = nb.n;
        fn->cap = nb.cap;
        done = 1;
        if (remarks_on() && fn->src)
            remark_add("opt", "entered-at-test", fn->name, "licm/guard-jump",
                       fn->file, fn->line,
                       "a rotated loop entered at its test: %d instructions "
                       "of guard dropped", L);
    }
    free(tbl);
    free(nreads);
    free_defs(&d);
    free(labpos);
    free(labrefs);
    return done;
}
