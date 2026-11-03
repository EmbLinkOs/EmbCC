/* A function's worst-case cycles, from the code its backend made: the
 * timing half of --stack-info (src/driver/stackinfo.c holds the rest).
 *
 * Each IR block costs what its machine instructions cost, by the cycle
 * model the bench and EmbSim charge (src/arch/cost.h: Cortex-M4, RV32 and
 * RV64, the ATmega328P, a MIPS32 five-stage pipeline -- elsewhere one per
 * instruction, as an estimate), with a taken branch's refill on every
 * block that ends in one. The bound is the longest path from the entry,
 * with each natural loop collapsed -- innermost first -- into its header
 * at `trip count` times the longest path round it. A loop's trip count is
 * the IR's counted-loop shape: an induction variable started from a
 * constant, stepped by one in the loop, and compared with a constant to
 * leave; one bound is taken with a trip to spare, which keeps the bound an
 * upper one whichever side of the step the test is. A loop without such a
 * count makes the function's bound unknown, and the report says which
 * line it is.
 *
 * Every instruction also gets its multiplicity -- the product of its
 * loops' counts -- so a call's callee is charged as often as the call
 * can run. A callee's own cycles are added by the report, which has the
 * whole program; an indirect call, a call into code without a bound, or
 * recursion makes a caller's bound unknown there too. */

#include "opt_int.h"
#include "../arch/cost.h"

#define UNB 0xffffffffu

/* one instruction's bytes and cost, at p (n bytes left) */
static int insn(const unsigned char *p, int left, int *len, int *branch)
{
    *branch = 0;
    switch (target_get()) {
    case TARGET_THUMB: {
        unsigned h = left >= 2 ? (unsigned)(p[0] | p[1] << 8) : 0;
        *len = (h & 0xf800) >= 0xe800 && left >= 4 ? 4 : 2;
        return arm_cost(p, (size_t)*len, branch);
    }
    case TARGET_RISCV32: case TARGET_RISCV64:
        *len = (p[0] & 3) == 3 && left >= 4 ? 4 : 2;
        return rv_cost(p, (size_t)*len, branch);
    case TARGET_AVR: {
        unsigned w = left >= 2 ? (unsigned)(p[0] | p[1] << 8) : 0;
        /* two words: lds, sts, jmp, call */
        int two = (w & 0xfe0e) == 0x940c || (w & 0xfe0e) == 0x940e ||
                  (w & 0xfe0f) == 0x9000 || (w & 0xfe0f) == 0x9200;
        *len = two && left >= 4 ? 4 : 2;
        return avr_cost(p, (size_t)*len, branch);
    }
    case TARGET_MIPS32:
        *len = 4;
        return left >= 4 ? mips_cost(p, 4, branch) : 1;
    default:
        *len = left;            /* no decoder: one cycle per four bytes */
        return (left + 3) / 4;
    }
}

static int is_avr(void) { return target_get() == TARGET_AVR; }

/* the cycles of code [lo, hi), and whether its last instruction branches */
static unsigned long block_cycles(const unsigned char *code, int lo, int hi,
                                  int *ends_branch)
{
    unsigned long c = 0;
    *ends_branch = 0;
    for (int at = lo; at < hi; ) {
        int len, br;
        c += (unsigned long)insn(code + at, hi - at, &len, &br);
        if (len <= 0)
            len = 1;
        at += len;
        *ends_branch = br;
    }
    return c;
}

/* the trip count of the loop headed at h (blocks in[]), or -1 */
static long trip_count(struct ir_func *fn, struct bb *bb, int nbb, int h,
                       const char *in)
{
    (void)h;
    for (int b = 0; b < nbb; b++) {
        if (!in[b] || bb[b].end <= bb[b].start)
            continue;
        const struct ir_ins *t = &fn->ins[bb[b].end - 1];
        if (t->op != IR_BRZ && t->op != IR_BRNZ)
            continue;
        /* the test: c = cmp v, #N (or a const temp) */
        const struct ir_ins *c = NULL;
        for (int n = bb[b].end - 2; n >= bb[b].start; n--)
            if (fn->ins[n].dst == t->a && fn->ins[n].op == IR_CMP) {
                c = &fn->ins[n];
                break;
            }
        if (!c)
            continue;
        long lim;
        if (c->imm_b)
            lim = c->imm;
        else {
            int found = 0;
            for (int n = 0; n < fn->nins && !found; n++)
                if (fn->ins[n].dst == c->b && fn->ins[n].op == IR_CONST) {
                    lim = fn->ins[n].imm;
                    found = 1;
                }
            if (!found)
                continue;
        }
        int v = c->a;
        /* the induction variable: every def in the loop a step by a
         * constant (through copies), one def outside a constant */
        long step = 0, init = 0;
        int ok = 1, ninit = 0, nstep = 0;
        for (int n = 0; n < fn->nins && ok; n++) {
            const struct ir_ins *d = &fn->ins[n];
            if (d->dst != v)
                continue;
            int inside = 0;
            for (int q = 0; q < nbb; q++)
                if (in[q] && n >= bb[q].start && n < bb[q].end)
                    inside = 1;
            if (!inside) {
                if (d->op == IR_CONST) { init = d->imm; ninit++; }
                else ok = 0;
                continue;
            }
            if ((d->op == IR_ADD || d->op == IR_SUB) && d->imm_b && d->a == v) {
                step = d->op == IR_ADD ? d->imm : -d->imm;
                nstep++;
            } else if (d->op == IR_MOV) {
                /* v = mov w, w = v + k */
                int w = d->a, got = 0;
                for (int q = 0; q < fn->nins && !got; q++) {
                    const struct ir_ins *e = &fn->ins[q];
                    if (e->dst == w && (e->op == IR_ADD || e->op == IR_SUB) &&
                        e->imm_b && e->a == v) {
                        step = e->op == IR_ADD ? e->imm : -e->imm;
                        got = 1;
                    }
                }
                if (!got) ok = 0;
                nstep++;
            } else {
                ok = 0;
            }
        }
        if (!ok || ninit != 1 || nstep != 1 || !step)
            continue;
        long span = lim - init;
        if ((span > 0) != (step > 0) && span)
            continue;
        long n = (span < 0 ? -span : span) / (step < 0 ? -step : step);
        if (n < 0 || n > 100000000L)
            continue;
        return n + 2;           /* the test's side of the step, and <= */
    }
    return -1;
}

int wcet_func(struct ir_func *fn, const unsigned char *code, int code_lo,
              int code_hi, unsigned long *cycles, int *unbounded_line,
              unsigned long *mult)
{
    *cycles = 0;
    *unbounded_line = 0;
    for (int n = 0; n < fn->nins; n++)
        mult[n] = 1;
    if (!fn->ins_pc || fn->nins == 0)
        return -1;
    int nbb;
    int *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    free(l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder = 0;
    compute_rpo(bb, nbb, order, &norder);
    compute_idom(bb, order, norder);

    /* each block's own cycles: from its first instruction that made code
     * to the next block's */
    unsigned long *w = xcalloc((size_t)nbb, sizeof *w);
    int refill = is_avr() ? 1 : 2;
    for (int b = 0; b < nbb; b++) {
        int lo = -1, hi = -1;
        for (int n = bb[b].start; n < bb[b].end && lo < 0; n++)
            if (fn->ins_pc[n] >= 0) lo = fn->ins_pc[n];
        for (int n = bb[b].end; n <= fn->nins && hi < 0; n++)
            if (fn->ins_pc[n] >= 0) hi = fn->ins_pc[n];
        if (hi < 0) hi = code_hi;
        if (lo < 0 || hi <= lo || lo < code_lo || hi > code_hi)
            continue;
        int br;
        w[b] = block_cycles(code, lo, hi, &br);
        if (br)
            w[b] += (unsigned long)refill;
    }

    /* the loops, smallest (innermost) first */
    struct lp { int h, t, size; char *in; long trip; };
    struct lp *L = NULL;
    int nl = 0;
    for (int t = 0; t < nbb; t++)
        for (int k = 0; k < bb[t].nsucc; k++) {
            int h = bb[t].succ[k];
            if (!bb_dominates(bb, h, t))
                continue;
            char *in = xcalloc((size_t)nbb, 1);
            loop_body(bb, nbb, h, t, in);
            int sz = 0;
            for (int q = 0; q < nbb; q++) sz += in[q];
            L = xrealloc(L, (size_t)(nl + 1) * sizeof *L);
            L[nl].h = h; L[nl].t = t; L[nl].size = sz; L[nl].in = in;
            L[nl].trip = trip_count(fn, bb, nbb, h, in);
            nl++;
        }
    for (int i = 1; i < nl; i++)
        for (int j = i; j > 0 && L[j - 1].size > L[j].size; j--) {
            struct lp x = L[j]; L[j] = L[j - 1]; L[j - 1] = x;
        }
    int unb = 0;
    for (int i = 0; i < nl; i++)
        if (L[i].trip < 0 && !unb) {
            unb = 1;
            int n = bb[L[i].h].start;
            *unbounded_line = n < fn->nins ? fn->ins[n].line : 0;
        }
    for (int i = 0; i < nl; i++)
        for (int n = 0; n < fn->nins; n++)
            for (int b = 0; b < nbb; b++)
                if (L[i].in[b] && n >= bb[b].start && n < bb[b].end) {
                    unsigned long m = mult[n] * (unsigned long)(L[i].trip > 0
                                                                ? L[i].trip : 1);
                    mult[n] = m;
                }

    /* collapse: rep[b] the node b now is part of */
    int *rep = xmalloc((size_t)nbb * sizeof *rep);
    for (int b = 0; b < nbb; b++) rep[b] = b;
    unsigned long *dist = xcalloc((size_t)nbb, sizeof *dist);
    for (int i = 0; i < nl && !unb; i++) {
        char *in = L[i].in;
        int h = L[i].h;
        /* longest path inside the body from the header, in rpo order,
         * over edges between body nodes that are not back edges to h */
        for (int b = 0; b < nbb; b++) dist[b] = 0;
        dist[h] = w[h];
        unsigned long best = w[h];
        for (int oi = 0; oi < norder; oi++) {
            int b = order[oi];
            if (!in[b] || rep[b] != b || (!dist[b] && b != h))
                continue;
            for (int k = 0; k < bb[b].nsucc; k++) {
                int s = rep[bb[b].succ[k]];
                if (!in[s] || s == h || bb[s].rpo <= bb[b].rpo)
                    continue;
                if (dist[b] + w[s] > dist[s])
                    dist[s] = dist[b] + w[s];
            }
            if (dist[b] > best)
                best = dist[b];
        }
        w[h] = best * (unsigned long)L[i].trip;
        for (int b = 0; b < nbb; b++)
            if (in[b] && b != h)
                for (int q = 0; q < nbb; q++)
                    if (rep[q] == b) { rep[q] = h; w[q] = 0; }
        for (int b = 0; b < nbb; b++)
            if (in[b] && b != h) { rep[b] = h; w[b] = 0; }
    }
    unsigned long total = 0;
    if (!unb) {
        for (int b = 0; b < nbb; b++) dist[b] = 0;
        int e = order[0];
        dist[e] = w[e];
        for (int oi = 0; oi < norder; oi++) {
            int b = order[oi];
            if (rep[b] != b)
                continue;
            if (dist[b] > total)
                total = dist[b];
            /* the edges leaving b, and leaving any block collapsed into it */
            for (int q = 0; q < nbb; q++) {
                if (rep[q] != b)
                    continue;
                for (int k = 0; k < bb[q].nsucc; k++) {
                    int s = rep[bb[q].succ[k]];
                    if (s == b || bb[s].rpo <= bb[b].rpo)
                        continue;
                    if (dist[b] + w[s] > dist[s])
                        dist[s] = dist[b] + w[s];
                }
            }
        }
    }
    *cycles = unb ? UNB : total;
    for (int i = 0; i < nl; i++)
        free(L[i].in);
    free(L);
    free(rep);
    free(dist);
    free(w);
    free(order);
    free_cfg(bb, nbb);
    return 0;
}
