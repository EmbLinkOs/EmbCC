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
