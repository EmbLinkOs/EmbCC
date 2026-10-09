/* ---- tail recursion into a loop -------------------------------------
 *
 * `return f(args)` where f is the function itself does not need a call
 * at all: assign the parameters and jump back to the top. Sibling calls
 * already stop the stack growing for this shape on x86-64, but a jump
 * is cheaper still -- no argument marshalling through the ABI
 * registers, no return address, and the body becomes a real loop that
 * LICM, rotation and strength reduction can then work on. That last
 * part is most of the value: a recursive function is opaque to every
 * loop pass, and a loop is not.
 *
 * The rewrite is the obvious one, and the ORDER inside it is the whole
 * correctness question:
 *
 *      f(a, b):                  f(a, b):
 *        ...                   L: ...
 *        return f(x, y)          t0 = x; t1 = y      <- read all
 *                                a = t0; b = t1      <- then write all
 *                                goto L
 *
 * Read-all-then-write-all, because `return f(b, a)` swaps its arguments
 * and assigning them one at a time would give f(b, b).
 *
 * Refused when a parameter's address escapes: the recursive call gets a
 * fresh frame and this does not, so anything holding a pointer to a
 * parameter would see it change underneath. */

#include "opt_int.h"

/* Is the call at `n` a tail call to this function, and if so how much
 * of what follows belongs to it?
 *
 * The result rarely flows straight into the `ret`. irgen forwards it
 * through the temp the expression was assigned to, and the `ret` itself
 * usually sits after a LABEL that the other arm of the conditional also
 * jumps to:
 *
 *      %15 = call @self(...)
 *      %2  = mov %15          <- ours to delete
 *   L1:                       <- shared: the other arm jumps here
 *      ret %2                 <- shared: must stay
 *
 * So the forwarding moves before the next label are ours, and anything
 * from the label on is not. Fills *ndel with how many instructions
 * after the call to drop. */
static int tailrec_at(struct ir_func *fn, struct func *f, int n, int np,
                      int *ndel)
{
    struct ir_ins *c = &fn->ins[n];
    if (c->op != IR_CALL || c->indirect || c->callee != f ||
        c->nargs != np || c->retsize || c->call_varargs || c->dst < 0)
        return 0;
    for (int k = 0; k < np; k++)
        if (c->argv[k].is_struct || c->argv[k].on_stack ||
            c->argv[k].is_float || c->argv[k].is_int128 || c->argv[k].byref)
            return 0;
    int cur = c->dst, j = n + 1, del = 0;
    while (j < fn->nins && fn->ins[j].op == IR_MOV &&
           fn->ins[j].a == cur && fn->ins[j].dst >= 0 && !fn->ins[j].vol) {
        cur = fn->ins[j].dst; j++; del++;
    }
    int k = j;
    while (k < fn->nins && fn->ins[k].op == IR_LABEL)
        k++;
    if (k >= fn->nins || fn->ins[k].op != IR_RET || fn->ins[k].a != cur)
        return 0;
    /* Nothing may read the forwarded value except that return -- if the
     * other arm's path also reads it, it reads ITS own value, and the
     * label between us means we cannot tell the two apart. */
    for (int m = 0; m < fn->nins; m++) {
        if (m > n && m <= k)
            continue;
        if (ins_reads(&fn->ins[m], cur))
            return 0;
    }
    *ndel = del;
    return 1;
}

int pass_tailrec(struct ir_func *fn)
{
    struct func *f = fn->src;
    if (!f || fn->nins == 0 || fn->is_varargs || fn->has_alloca || fn->neh)
        return 0;
    int np = fn->nparams;
    if (np > MAX_PARAMS)
        return 0;

    /* A parameter whose address is taken cannot be reassigned here. */
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= 0 &&
            fn->ins[n].a < np)
            return 0;

    int found = 0, junk;
    for (int n = 0; n < fn->nins; n++)
        if (tailrec_at(fn, f, n, np, &junk)) { found = 1; break; }
    if (!found)
        return 0;

    int Ltop = fn->nlabels++;
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0;
    {   /* the loop's head, before anything the body does */
        struct ir_ins *l = ib_push(&nb);
        l->op = IR_LABEL; l->label = Ltop; l->synth = 1;
        l->line = fn->line;
    }
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        struct ir_ins *c = &fn->ins[n];
        int ndel = 0;
        if (tailrec_at(fn, f, n, np, &ndel)) {
            int argv[MAX_PARAMS], tmp[MAX_PARAMS];
            for (int k = 0; k < np; k++)
                argv[k] = c->argv[k].vreg;
            for (int k = 0; k < np; k++) {          /* read all */
                tmp[k] = fn->nvregs++;
                struct ir_ins *m = ib_push(&nb);
                m->op = IR_MOV; m->dst = tmp[k]; m->a = argv[k];
                m->w = m2r_w(fn->locals[k].size);
                m->line = c->line; m->col = c->col; m->synth = 1;
            }
            for (int k = 0; k < np; k++) {          /* then write all */
                struct ir_ins *st = ib_push(&nb);
                st->op = IR_STVAR; st->dst = k; st->a = tmp[k];
                st->size = fn->locals[k].size;
                st->line = c->line; st->col = c->col; st->synth = 1;
            }
            struct ir_ins *j = ib_push(&nb);
            j->op = IR_JMP; j->label = Ltop;
            j->line = c->line; j->col = c->col; j->synth = 1;
            n += ndel;                  /* the forwarding moves go too */
            changed = 1;
            continue;
        }
        *ib_push(&nb) = *c;
    }
    if (!changed) { free(nb.p); free(newpos); fn->nlabels--; return 0; }
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
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    g_did.tailrec++;
    return 1;
}

/* ---- a returned call's return, copied to it ---------------------------
 *
 * `return c < 0x80 ? isdigit(c) : 0;` reaches its one `ret` from both
 * arms, so the call's arm is call, jump, and the return at the join:
 *
 *        %1 = call @isdigit(%0)          %1 = call @isdigit(%0)
 *        jmp L1                    ->    ret %1
 *      L0: %1 = const 0                L0: %1 = const 0
 *      L1: ret %1                      L1: ret %1
 *
 * A backend makes a TAIL call only of a call the return follows, so the
 * copy is what lets this one leave by a branch with no frame: clang's
 * iswdigit is four instructions and EmbCC's was eleven on RISC-V. Only
 * after a call whose value is the one returned (or a call in a function
 * returning nothing); elsewhere a copied return could add a move into
 * the return register for nothing. Last, after the tail merge, which
 * would otherwise share the two returns again. */
int pass_retdup(struct ir_func *fn)
{
    int changed = 0, nl = fn->nlabels;
    int *at;
    if (nl <= 0 || getenv("EMBCC_NO_RETDUP"))
        return 0;
    at = xmalloc((size_t)nl * sizeof *at);
    for (int l = 0; l < nl; l++)
        at[l] = -1;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < nl)
            at[fn->ins[n].label] = n;
    for (int n = 1; n < fn->nins; n++) {
        const struct ir_ins *j = &fn->ins[n], *c = &fn->ins[n - 1];
        int m;
        if (j->op != IR_JMP || c->op != IR_CALL || j->label < 0 ||
            j->label >= nl || at[j->label] < 0)
            continue;
        m = at[j->label];
        while (m < fn->nins && fn->ins[m].op == IR_LABEL)
            m++;
        if (m >= fn->nins || fn->ins[m].op != IR_RET ||
            (fn->ins[m].a >= 0 && fn->ins[m].a != c->dst))
            continue;
        fn->ins[n] = fn->ins[m];
        changed = 1;
    }
    free(at);
    return changed;
}
