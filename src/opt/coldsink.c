/* Cold code out of the way: the blocks the profile never saw run
 * (-fprofile-use), the arm of an `if` __builtin_expect says is unlikely,
 * an arm that calls a noreturn or cold function -- every instruction of
 * which carries IR_HEAT_COLD -- moved to the end of the function, so
 * the code that runs is one straight line and its branches fall
 * through. GCC's and clang's block reordering does this with the
 * same information.
 *
 * A run of cold blocks leaves its place and follows the function's last
 * block, in its own order. What fell into it now jumps there -- or,
 * the usual case, the conditional branch that skipped it is inverted to
 * branch to it, and the hot path falls through to what followed it.
 * What it fell out of is a jump back. The entry block stays first.
 *
 * Not for a function with exception regions, whose ranges name
 * instruction positions the backends read; a local's lexical range
 * that a moved block was in becomes the whole function (the slot is
 * then shared with no other), and every other range moves with its
 * instructions. */

#include "opt_int.h"

static int is_end(enum ir_op op)
{
    return op == IR_JMP || op == IR_RET || op == IR_UD2 || op == IR_IGOTO ||
           op == IR_SWITCH;
}

static int block_cold(const struct ir_func *fn, int lo, int hi)
{
    int any = 0;
    for (int n = lo; n < hi; n++) {
        if (fn->ins[n].op == IR_LABEL)
            continue;
        if (fn->ins[n].heat != IR_HEAT_COLD)
            return 0;
        any = 1;
    }
    return any;
}

int pass_coldsink(struct ir_func *fn)
{
    int N = fn->nins;
    if (N < 4 || fn->neh)
        return 0;
    int any = 0;
    for (int n = 0; n < N && !any; n++)
        any = fn->ins[n].heat == IR_HEAT_COLD;
    if (!any)
        return 0;
    /* the last instruction must not fall off the end: code is about to
     * follow it */
    if (!is_end(fn->ins[N - 1].op))
        return 0;

    /* blocks */
    int *bs = xmalloc((size_t)(N + 1) * sizeof *bs), nbk = 0;
    for (int n = 0; n < N; n++) {
        enum ir_op op = fn->ins[n].op;
        if (n == 0 || op == IR_LABEL ||
            is_end(fn->ins[n - 1].op) || fn->ins[n - 1].op == IR_BRZ ||
            fn->ins[n - 1].op == IR_BRNZ)
            bs[nbk++] = n;
    }
    bs[nbk] = N;
    char *cold = xcalloc((size_t)nbk, 1);
    int ncold = 0;
    for (int b = 1; b < nbk; b++)
        if (block_cold(fn, bs[b], bs[b + 1])) {
            cold[b] = 1;
            ncold++;
        }
    /* a cold run that is already the function's end stays where it is */
    for (int b = nbk - 1; b >= 1 && cold[b]; b--) {
        cold[b] = 0;
        ncold--;
    }
    if (!ncold) {
        free(bs);
        free(cold);
        return 0;
    }

    /* each run's first block needs a label, and so does the block after
     * a run that falls out of it */
    int *lab = xmalloc((size_t)nbk * sizeof *lab);
    for (int b = 0; b < nbk; b++)
        lab[b] = fn->ins[bs[b]].op == IR_LABEL ? fn->ins[bs[b]].label : -1;
    for (int b = 1; b < nbk; b++) {
        if (cold[b] && !cold[b - 1] && lab[b] < 0)
            lab[b] = -2;                       /* to make */
        if (!cold[b] && cold[b - 1] && lab[b] < 0)
            lab[b] = -2;
    }
    for (int b = 0; b < nbk; b++)
        if (lab[b] == -2)
            lab[b] = -(3 + fn->nlabels++);     /* made, not yet placed */

    size_t cap = (size_t)N + 4 * (size_t)nbk + 8;
    struct ir_ins *out = xmalloc(cap * sizeof *out);
    int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
    char *moved = xcalloc((size_t)N + 1, 1);
    int o = 0;
#define LBL(b) (lab[b] < -2 ? -(lab[b] + 3) : lab[b])
#define PUT_LABEL(b) do {                                                  \
        if (lab[b] < -2) {                                                 \
            memset(&out[o], 0, sizeof out[o]);                             \
            out[o].op = IR_LABEL;                                          \
            out[o].label = LBL(b);                                         \
            out[o].dst = out[o].a = out[o].b = -1;                         \
            out[o].line = fn->ins[bs[b]].line;                             \
            out[o].synth = 1;                                              \
            o++;                                                           \
        }                                                                  \
    } while (0)
#define PUT_JMP(L, line_) do {                                             \
        memset(&out[o], 0, sizeof out[o]);                                 \
        out[o].op = IR_JMP;                                                \
        out[o].label = (L);                                                \
        out[o].dst = out[o].a = out[o].b = -1;                             \
        out[o].line = (line_);                                             \
        out[o].synth = 1;                                                  \
        o++;                                                               \
    } while (0)

    /* the hot blocks, in order */
    for (int b = 0; b < nbk; b++) {
        if (cold[b])
            continue;
        PUT_LABEL(b);
        for (int n = bs[b]; n < bs[b + 1]; n++) {
            newpos[n] = o;
            out[o++] = fn->ins[n];
        }
        if (b + 1 < nbk && cold[b + 1]) {
            /* what follows the run */
            int nx = b + 1;
            while (nx < nbk && cold[nx])
                nx++;
            struct ir_ins *t = &out[o - 1];
            int line = t->line;
            if ((t->op == IR_BRZ || t->op == IR_BRNZ) && nx < nbk &&
                t->label == LBL(nx)) {
                /* `if (c) <cold>`: branch to the cold code instead, and
                 * fall through to what followed it */
                t->op = t->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
                t->prob = (short)ir_prob_flip(t->prob);
                t->label = LBL(b + 1);
            } else if (!is_end(t->op)) {
                PUT_JMP(LBL(b + 1), line);
            }
        }
    }
    /* the cold runs, each with its way back */
    for (int b = 1; b < nbk; b++) {
        if (!cold[b])
            continue;
        PUT_LABEL(b);
        for (int n = bs[b]; n < bs[b + 1]; n++) {
            newpos[n] = o;
            moved[n] = 1;
            out[o++] = fn->ins[n];
        }
        if (b + 1 < nbk && !cold[b + 1]) {
            const struct ir_ins *t = &out[o - 1];
            if (!is_end(t->op))
                PUT_JMP(LBL(b + 1), t->line);
        }
    }
#undef LBL
#undef PUT_LABEL
#undef PUT_JMP
    newpos[N] = o;

    if (fn->var_scope_lo) {
        /* pm[n]: how many of the first n instructions moved */
        int *pm = xmalloc((size_t)(N + 1) * sizeof *pm);
        pm[0] = 0;
        for (int n = 0; n < N; n++)
            pm[n + 1] = pm[n] + moved[n];
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo < 0 || hi < 0 || lo > N || hi > N || lo > hi)
                continue;
            if (pm[hi] - pm[lo]) {
                fn->var_scope_lo[v] = 0;
                fn->var_scope_hi[v] = o;
            } else {
                fn->var_scope_lo[v] = newpos[lo];
                fn->var_scope_hi[v] = hi == N ? o : newpos[hi];
            }
        }
        free(pm);
    }
    free(fn->ins);
    fn->ins = out;
    fn->nins = o;
    fn->cap = (int)cap;
    free(newpos);
    free(moved);
    free(lab);
    free(cold);
    free(bs);
    return 1;
}


/* ---- block placement: a jump's target after it ----------------------
 *
 * The IR's blocks come in source order, and every `if` with an `else`,
 * every early return through a merge, every loop exit leaves a `jmp`
 * that a different order would not need: RV32 libc had 1,750 more
 * unconditional jumps than GCC's. This places the function as CHAINS --
 * runs of blocks that fall into each other, each ending in a jump, a
 * return, a switch or a trap -- and when the chain just placed ends in
 * `jmp L` and L begins a chain not yet placed, that chain goes next and
 * the jump goes. `brz c -> L; jmp M` with M placed already and L not:
 * the branch is turned to M and L follows. Otherwise the next chain in
 * source order. The entry chain stays first; a chain only moves whole,
 * so nothing that fell through stops falling through.
 *
 * Not for a function with exception regions (as pass_coldsink), nor one
 * whose last instruction falls off its end. A local's lexical range a
 * moved instruction was in becomes the whole function, as
 * pass_coldsink's. EMBCC_NO_LAYOUT=1 turns it off. */
int pass_layout(struct ir_func *fn)
{
    int N = fn->nins;
    if (N < 4 || fn->neh || getenv("EMBCC_NO_LAYOUT"))
        return 0;
    if (!is_end(fn->ins[N - 1].op))
        return 0;
    /* chains: a new one where the instruction before ends one */
    int *cs = xmalloc((size_t)(N + 1) * sizeof *cs), nc = 0;
    for (int n = 0; n < N; n++)
        if (n == 0 || is_end(fn->ins[n - 1].op))
            cs[nc++] = n;
    cs[nc] = N;
    if (nc < 3) {
        free(cs);
        return 0;
    }
    /* the chain each label begins, -1 for a label inside a chain */
    int *lchain = xmalloc((size_t)(fn->nlabels + 1) * sizeof *lchain);
    for (int l = 0; l <= fn->nlabels; l++)
        lchain[l] = -1;
    for (int c = 0; c < nc; c++)
        for (int n = cs[c]; n < cs[c + 1] && fn->ins[n].op == IR_LABEL; n++)
            if (fn->ins[n].label >= 0 && fn->ins[n].label < fn->nlabels)
                lchain[fn->ins[n].label] = c;
    /* who names each chain's labels from OUTSIDE it: a chain is pulled
     * forward only when the jump being followed is the one -- so no
     * other jump to it turns backward, which the backends would read as
     * a loop (ra_loop_depth: a backward branch's span), and a loop
     * inside the chain stays inside it. A chain whose label's address
     * is taken (&&L) or that a jump table names never moves. */
    int *ext = xcalloc((size_t)nc, sizeof *ext);
    int *chain_of = xmalloc((size_t)N * sizeof *chain_of);
    for (int c = 0; c < nc; c++)
        for (int n = cs[c]; n < cs[c + 1]; n++)
            chain_of[n] = c;
    int *lc = xmalloc((size_t)(fn->nlabels + 1) * sizeof *lc);
    for (int l = 0; l <= fn->nlabels; l++)
        lc[l] = -1;
    for (int n = 0; n < N; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < fn->nlabels)
            lc[fn->ins[n].label] = chain_of[n];
    for (int n = 0; n < N; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int here = chain_of[n];
#define REF(L, w) do { int l_ = (L); if (l_ >= 0 && l_ < fn->nlabels &&   \
            lc[l_] >= 0 && lc[l_] != here) ext[lc[l_]] += (w); } while (0)
        if (i->op == IR_JMP || i->op == IR_BRZ || i->op == IR_BRNZ)
            REF(i->label, 1);
        else if (i->op == IR_SWITCH) {
            REF(i->label, 1000);
            if (i->jt >= 0)
                for (int k = 0; k < fn->jt[i->jt].n; k++)
                    REF(fn->jt[i->jt].labels[k], 1000);
        } else if (i->op == IR_LABELADDR) {
            if (i->label >= 0 && i->label < fn->nlabels && lc[i->label] >= 0)
                ext[lc[i->label]] += 1000;
        }
#undef REF
    }
    free(chain_of);
    free(lc);
    int *order = xmalloc((size_t)nc * sizeof *order), no = 0;
    char *pulled = xcalloc((size_t)nc, 1);
    char *placed = xcalloc((size_t)nc, 1);
    /* what to do with each chain's last two instructions when the next
     * chain is the one its jump names: 1 drop the jmp; 2 also turn the
     * branch before it to the jmp's label */
    char *drop = xcalloc((size_t)nc, 1);
    int next_src = 0, moved_any = 0;
    while (no < nc) {
        int c = -1;
        if (no) {
            int p = order[no - 1], e = cs[p + 1] - 1;
            const struct ir_ins *j = &fn->ins[e];
            if (j->op == IR_JMP && j->label >= 0 && j->label < fn->nlabels) {
                int t = lchain[j->label];
                if (t > 0 && !placed[t] && ext[t] == 1) {
                    c = t;
                    drop[p] = 1;
                } else if (e - 1 >= cs[p] &&
                           (fn->ins[e - 1].op == IR_BRZ ||
                            fn->ins[e - 1].op == IR_BRNZ) &&
                           fn->ins[e - 1].label >= 0 &&
                           fn->ins[e - 1].label < fn->nlabels) {
                    int u = lchain[fn->ins[e - 1].label];
                    if (u > 0 && !placed[u] && ext[u] == 1) {
                        c = u;
                        drop[p] = 2;
                    }
                }
            }
        }
        while (next_src < nc && placed[next_src])
            next_src++;
        if (c < 0)
            c = next_src;
        else if (c != next_src)
            pulled[c] = 1;              /* out of source order */
        if (c != (no ? order[no - 1] + 1 : 0))
            moved_any = 1;
        placed[c] = 1;
        order[no++] = c;
    }
    int dropped = 0;
    for (int c = 0; c < nc; c++)
        dropped += drop[c] != 0;
    if (!dropped) {
        free(cs); free(lchain); free(order); free(placed); free(drop);
        free(ext); free(pulled);
        return 0;
    }
    (void)moved_any;
    struct ir_ins *out = xmalloc((size_t)(N + 1) * sizeof *out);
    int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
    char *moved = xcalloc((size_t)N + 1, 1);
    int o = 0;
    for (int k = 0; k < no; k++) {
        int c = order[k];
        int end = cs[c + 1];
        for (int n = cs[c]; n < end; n++) {
            newpos[n] = o;
            if (pulled[c])
                moved[n] = 1;
            if (n == end - 1 && drop[c])
                continue;                       /* the jmp */
            out[o] = fn->ins[n];
            if (n == end - 2 && drop[c] == 2) {
                /* `brz c -> L; jmp M`, L next: `brnz c -> M` */
                out[o].op = out[o].op == IR_BRZ ? IR_BRNZ : IR_BRZ;
                out[o].prob = (short)ir_prob_flip(out[o].prob);
                out[o].label = fn->ins[end - 1].label;
            }
            o++;
        }
    }
    newpos[N] = o;
    if (fn->var_scope_lo) {
        int *pm = xmalloc((size_t)(N + 1) * sizeof *pm);
        pm[0] = 0;
        for (int n = 0; n < N; n++)
            pm[n + 1] = pm[n] + moved[n];
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo < 0 || hi < 0 || lo > N || hi > N || lo > hi)
                continue;
            if (pm[hi] - pm[lo]) {
                fn->var_scope_lo[v] = 0;
                fn->var_scope_hi[v] = o;
            } else {
                fn->var_scope_lo[v] = newpos[lo];
                fn->var_scope_hi[v] = hi == N ? o : newpos[hi];
            }
        }
        free(pm);
    }
    free(fn->ins);
    fn->ins = out;
    fn->nins = o;
    fn->cap = N + 1;
    free(newpos); free(moved);
    free(cs); free(lchain); free(order); free(placed); free(drop);
    free(ext); free(pulled);
    return 1;
}
