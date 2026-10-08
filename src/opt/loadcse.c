/* ---- global redundant-load elimination (available-expressions) ------------ *
 *
 * pass_gcse leaves memory reads (LDVAR/LOAD) to the block-local pass_lvn: their
 * value depends on a store history that crosses blocks, which dominance alone
 * cannot reason about (a store on a non-dominator-tree path still kills a load).
 * This pass does the real thing — an available-expressions dataflow. A load is
 * redundant at a point if an identical earlier load reaches it on EVERY path
 * with no intervening write that could alias it; the reload becomes a copy.
 *
 * Soundness rests on three things:
 *   - the meet is "same representative TEMP from all predecessors", so the reused
 *     value is one dominating definition (loads produce single-def temps), never
 *     a per-path phi;
 *   - a LOAD is keyed only when its address temp is single-def, so the address
 *     cannot change between the two loads -- and it is keyed by where that
 *     address points, base value and offset (mem_access), so `p->len` read
 *     through two different `add %p, #16` temps is one key. That is what -O1,
 *     which has no global CSE to merge the two adds, needs;
 *   - the kill model separates a store to a non-address-taken local (kills only
 *     that local's LDVARs) from a real memory write. A plain store kills the
 *     keys whose bytes it may overlap (acc_overlap); a volatile store, a memcpy
 *     or a memzero the keys on an object it may alias; a call, asm, an atomic,
 *     a fence or a vector store every LOAD and every address-taken local's
 *     LDVAR.
 */

#include "opt_int.h"

struct lkey {
    enum ir_op op;
    int a, size, sign, w;
    /* A LOAD whose address has a base (mem_access) is keyed by the base
     * and the offset, and `a` is -1; `rep` is one address temp of it,
     * for the object it reads. */
    int bkind, bid, rep;
    unsigned long off;
};

static int lkey_eq(const struct lkey *x, const struct lkey *y)
{
    return x->op == y->op && x->a == y->a && x->size == y->size &&
           x->sign == y->sign && x->w == y->w && x->bkind == y->bkind &&
           x->bid == y->bid && x->off == y->off;
}

static int lcse_kills_mem(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_CALL: case IR_MEMCPY: case IR_MEMZERO:
    case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS:
    case IR_CAS16: case IR_ASM: case IR_VA_START:
    case IR_FENCE:           /* a barrier: see writes_memory */
    /* A vectorized loop's stores. Missing here, a scalar load from
     * before such a loop was reused after it: `x = a[0]; for (...)
     * a[i] = b[i] + 1; return x + a[0];` returned 2 * x at -O2 on
     * x86-64, where the loop vectorizes. */
    case IR_VSTORE:
        return 1;
    default:
        return 0;
    }
}

/* Drop the cached loads a write could reach. A plain store names its
 * bytes, so only the keys that may overlap them go (acc_overlap): a
 * store to q->wr keeps q->len. A volatile store, a memcpy and a memzero
 * keep their older rule and name only an object. A call, inline asm, an
 * atomic, a fence or a vector store names nothing, so everything
 * reachable goes. */
static void lcse_kill(int *s, int nk, const char *is_mem,
                      const struct maccess *kacc, struct ir_ins *ins,
                      struct ir_func *fn, struct defs *d, const char *taken,
                      int nvars)
{
    struct maccess w = slot_access(-1);
    int named = 0;
    if (ins->op == IR_STORE && !ins->vol) {
        w = mem_access(fn, d, ins->a, ins->size);
        named = 1;      /* an unknown object still has its base */
    } else if (ins->op == IR_STORE || ins->op == IR_MEMCPY ||
               ins->op == IR_MEMZERO) {
        w = obj_access(fn, d, ins->a);
        named = w.obj.kind != MEM_UNKNOWN;
    } else if (ins->op == IR_CALL && !ins->indirect && ins->callee &&
               ins->callee->inf_no_write) {
        return;         /* it writes nothing the caller can see */
    }
    for (int k = 0; k < nk; k++) {
        if (!is_mem[k])
            continue;
        if (named && !acc_overlap(w, kacc[k], taken, nvars))
            continue;
        s[k] = -1;
    }
}

/* A store of v makes a later load of the same address read v: the key
 * becomes available with v as its value, as a load would have made it.
 * Only when nothing can change v meanwhile -- one definition: a temp
 * written once, or a parameter never reassigned -- and when the load
 * reads exactly what was stored: the same 4 or 8 bytes, no extension.
 * (A float stored and its bits loaded as an integer is the same value:
 * the forwarded copy ties the two to one register class, which is what
 * every backend's class analysis already does with a copy.) */
static void lcse_store_gen(int *s, int nk, const struct lkey *keys,
                           const struct ir_ins *ins,
                           struct ir_func *fn, struct defs *d)
{
    int v = ins->b;
    if (ins->vol || ins->imm_b || v < 0 || v >= fn->nvregs ||
        d->cnt[v] != 1 || (v >= fn->nparams && v < fn->nvars) ||
        (ins->size != 4 && ins->size != 8))
        return;
    struct maccess m = mem_access(fn, d, ins->a, ins->size);
    for (int k = 0; k < nk; k++) {
        if (keys[k].op != IR_LOAD || keys[k].size != ins->size ||
            keys[k].w != ins->size || keys[k].bkind != m.bkind)
            continue;
        if (m.bkind == BASE_NONE ? keys[k].a == ins->a
                                 : keys[k].bid == m.bid && keys[k].off == m.off)
            s[k] = v;
    }
}

/* On ARM: a load through an address temp written more than once -- a
 * pointer a loop walks -- is keyed by that temp too, and every write of
 * it drops the key, so `*p` read in each arm of an if-else chain is read
 * once: printf's flag loop read its character five times. Its value is a
 * temp written once (the load), so nothing else has to drop it. Thumb
 * only, measured there; the other targets' output stays as it was. */
static void lcse_kill_defs(int *s, int nk, const struct lkey *keys,
                           const struct ir_ins *ins)
{
    int t = def_target(ins);
    if (t < 0)
        return;
    for (int k = 0; k < nk; k++)
        if (keys[k].op == IR_LOAD && keys[k].bkind == BASE_NONE &&
            keys[k].a == t)
            s[k] = -1;
}

int pass_loadcse(struct ir_func *fn)
{
    int nvars = fn->nvars;
    int mdef = target_get() == TARGET_THUMB && !getenv("EMBCC_NO_LCSE_MDEF");
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        free_cfg(bb, nbb); free_defs(&d); return 0;
    }

    char *taken = slots_taken(fn);

    /* Enumerate distinct load keys; keyidx[i] maps a load instruction to one. */
    struct lkey *keys = NULL; int nk = 0, capk = 0;
    int *keyidx = xmalloc((size_t)fn->nins * sizeof *keyidx);
    for (int i = 0; i < fn->nins; i++) {
        keyidx[i] = -1;
        struct ir_ins *in = &fn->ins[i];
        struct lkey k;
        memset(&k, 0, sizeof k);
        k.bkind = BASE_NONE; k.bid = -1;
        if (in->op == IR_LDVAR && !in->vol && in->a >= 0 && in->a < nvars) {
            k.op = IR_LDVAR;
        } else if (in->op == IR_LOAD && !in->vol && in->a >= 0 &&
                   in->a < fn->nvregs &&
                   (d.cnt[in->a] == 1 ||
                    (mdef && in->a >= nvars && in->dst != in->a &&
                     in->dst >= 0 && in->dst < fn->nvregs &&
                     d.cnt[in->dst] == 1))) {
            k.op = IR_LOAD;
        } else {
            continue;
        }
        k.a = k.rep = in->a; k.size = in->size; k.sign = in->sign; k.w = in->w;
        if (k.op == IR_LOAD) {
            struct maccess m = mem_access(fn, &d, in->a, in->size);
            if (m.bkind != BASE_NONE) {
                k.a = -1; k.bkind = m.bkind; k.bid = m.bid; k.off = m.off;
            }
        }
        int found = -1;
        for (int j = 0; j < nk; j++)
            if (lkey_eq(&keys[j], &k)) { found = j; break; }
        if (found < 0) {
            if (nk == capk) { capk = capk ? capk * 2 : 32;
                keys = xrealloc(keys, (size_t)capk * sizeof *keys); }
            keys[nk] = k; found = nk++;
        }
        keyidx[i] = found;
    }
    if (nk == 0) {
        free(order); free(l2b); free(taken); free(keyidx); free(keys);
        free_cfg(bb, nbb); free_defs(&d); return 0;
    }
    /* is_mem[k]: a LOAD or an address-taken local's LDVAR (killed by any write).
     * A non-address-taken local's LDVAR is killed only by a store to that local. */
    char *is_mem = xmalloc((size_t)nk);
    for (int k = 0; k < nk; k++)
        is_mem[k] = keys[k].op == IR_LOAD ||
                    (keys[k].op == IR_LDVAR && taken[keys[k].a]);

    /* What bytes each cached load reads, so a write can kill only the
     * ones it could actually reach. Without this a single store through
     * a pointer dropped every cached load in the function. */
    struct maccess *kacc = xmalloc((size_t)(nk ? nk : 1) * sizeof *kacc);
    for (int k = 0; k < nk; k++) {
        if (keys[k].op == IR_LDVAR)
            kacc[k] = slot_access(keys[k].a);
        else
            kacc[k] = mem_access(fn, &d, keys[k].rep, keys[k].size);
    }

    /* Dataflow. avail[b][k]: -2 top (init), -1 not available, >=0 the temp. */
    int *aout = xmalloc((size_t)nbb * (size_t)nk * sizeof *aout);
    int *ain  = xmalloc((size_t)nbb * (size_t)nk * sizeof *ain);
    for (int i = 0; i < nbb * nk; i++) aout[i] = -2;
    int *s = xmalloc((size_t)nk * sizeof *s);

    for (int iter = 0, changed = 1; changed && iter < nbb + 2; iter++) {
        changed = 0;
        for (int oi = 0; oi < nbb; oi++) {
            int b = order[oi];
            int *in = ain + (size_t)b * nk;
            if (b == 0) {
                for (int k = 0; k < nk; k++) in[k] = -1;   /* entry: empty */
            } else {
                for (int k = 0; k < nk; k++) in[k] = -2;    /* top */
                for (int p = 0; p < bb[b].npred; p++) {
                    int *po = aout + (size_t)bb[b].pred[p] * nk;
                    for (int k = 0; k < nk; k++) {
                        int m = in[k], v = po[k];        /* three-valued meet */
                        in[k] = (m == -1 || v == -1) ? -1
                              : (m == -2) ? v : (v == -2) ? m
                              : (m == v) ? m : -1;
                    }
                }
                for (int k = 0; k < nk; k++) if (in[k] == -2) in[k] = -1;
            }
            for (int k = 0; k < nk; k++) s[k] = in[k];
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *ins = &fn->ins[i];
                if (ins->op == IR_STVAR) {
                    for (int k = 0; k < nk; k++)
                        if ((keys[k].op == IR_LDVAR && keys[k].a == ins->dst) ||
                            (taken[ins->dst] && is_mem[k]))
                            s[k] = -1;
                } else if (lcse_kills_mem(ins->op)) {
                    lcse_kill(s, nk, is_mem, kacc, ins, fn, &d, taken, nvars);
                    if (ins->op == IR_STORE)
                        lcse_store_gen(s, nk, keys, ins, fn, &d);
                }
                if (mdef)
                    lcse_kill_defs(s, nk, keys, ins);
                int k = keyidx[i];
                if (k >= 0 && s[k] < 0) s[k] = ins->dst;   /* first def of the value */
            }
            int *out = aout + (size_t)b * nk;
            for (int k = 0; k < nk; k++)
                if (out[k] != s[k]) { out[k] = s[k]; changed = 1; }
        }
    }

    /* Replacement: replay each block from its (now stable) avail_in. */
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        int *in = ain + (size_t)b * nk;
        for (int k = 0; k < nk; k++) s[k] = in[k];
        for (int i = bb[b].start; i < bb[b].end; i++) {
            struct ir_ins *ins = &fn->ins[i];
            if (ins->op == IR_STVAR) {
                for (int k = 0; k < nk; k++)
                    if ((keys[k].op == IR_LDVAR && keys[k].a == ins->dst) ||
                        (taken[ins->dst] && is_mem[k]))
                        s[k] = -1;
            } else if (lcse_kills_mem(ins->op)) {
                lcse_kill(s, nk, is_mem, kacc, ins, fn, &d, taken, nvars);
                if (ins->op == IR_STORE)
                    lcse_store_gen(s, nk, keys, ins, fn, &d);
            }
            if (mdef)
                lcse_kill_defs(s, nk, keys, ins);
            int k = keyidx[i];
            if (k < 0) continue;
            if (s[k] >= 0 && s[k] != ins->dst) {
                to_mov(ins, s[k]); changed = 1; g_did.loadcse++;  /* redundant reload */
            } else if (s[k] < 0) {
                s[k] = ins->dst;
            }
        }
    }

    free(order); free(l2b); free(taken); free(keyidx); free(keys);
    free(is_mem); free(kacc); free(aout); free(ain); free(s);
    free_cfg(bb, nbb); free_defs(&d);
    return changed;
}
