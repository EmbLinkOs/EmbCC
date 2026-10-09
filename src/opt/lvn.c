/* ---- local value numbering (CSE within a basic block) ---- */

#include "opt_int.h"

/* An op that may write memory (so a cached load past it is stale). */
int writes_memory(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_STVAR: case IR_CALL: case IR_MEMCPY:
    case IR_MEMZERO: case IR_XCHG: case IR_XADD: case IR_CMPXCHG:
    case IR_ARMW: case IR_CAS: case IR_CAS16: case IR_ASM: case IR_VA_START:
    case IR_VSTORE:
    /* A fence writes nothing, but a load cached from before it must not be
     * reused after it: that reuse IS the reordering the fence forbids, and a
     * loop polling a flag across __sync_synchronize() would spin on a stale
     * value forever. */
    case IR_FENCE:
        return 1;
    default:
        return 0;
    }
}

int vn_eq(const struct vn *x, const struct vn *y)
{
    return x->op == y->op && x->a == y->a && x->b == y->b && x->w == y->w &&
           x->flt == y->flt &&
           x->sign == y->sign && x->size == y->size && x->pred == y->pred &&
           x->imm == y->imm && x->ptr == y->ptr && x->label == y->label &&
           x->memver == y->memver &&
           x->has_ca == y->has_ca && x->has_cb == y->has_cb &&
           (!x->has_ca || x->ca == y->ca) && (!x->has_cb || x->cb == y->cb);
}

/* Build the value key for a CSE-able instruction; returns 0 if it is not one
 * (VOLATILE loads/ldvars, calls, stores — anything with an effect or that we
 * don't number).
 *
 * A float or double operation is numbered like an integer one. EmbCC
 * compiles every program as if FENV_ACCESS were off (the rounding mode is
 * the default one, and no flag is ever tested), and then the same operation
 * on the same operands gives the same bits every time -- a NaN included,
 * since the machine that makes it is the same -- so computing it once is
 * computing it. `(x*y)/(x*y)` in fdlibm was two multiplies, two calls on a
 * Cortex-M3, with x and y kept live across the first. `flt` is part of the
 * key: an integer and a floating add of the same temps are different
 * values. Long double is not numbered (w == 16, below). */
int vn_key(struct ir_ins *i, int memver, struct vn *k)
{
    memset(k, 0, sizeof *k);
    k->op = i->op; k->a = -1; k->b = -1;
    k->flt = i->flt;
    /* An __int128 constant's key would be its low half only, so two
     * different ones would number the same. */
    if (i->w == 16)
        return 0;
    switch (i->op) {
    case IR_CONST:               /* same literal -> one temp, so uses of it CSE */
        k->imm = i->imm; k->w = i->w; return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_MULH: case IR_MULW:
        k->a = i->a; k->b = i->b; k->w = i->w; k->sign = i->sign; return 1;
    case IR_CMP:
        k->a = i->a; k->b = i->b; k->w = i->w; k->sign = i->sign;
        k->pred = i->pred; return 1;
    case IR_NEG: case IR_BNOT: case IR_SQRT:
        /* `op` is already part of the key, so these three cannot
         * collide with each other. */
        k->a = i->a; k->w = i->w; return 1;
    case IR_EXT:
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size; return 1;
    case IR_BSWAP:
        k->a = i->a; k->size = i->size; return 1;
    case IR_ADDR:                       /* &local: a frame-relative constant */
        k->a = i->a; return 1;
    case IR_GADDR: k->ptr = i->glob; return 1;
    case IR_FADDR: k->ptr = i->callee; return 1;
    case IR_STRADDR: k->label = i->label; return 1;
    case IR_LDVAR:                      /* a variable read (memory-versioned) */
        if (i->vol) return 0;
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size;
        k->memver = memver; return 1;
    case IR_LOAD:                       /* a pointer deref (memory-versioned) */
        if (i->vol) return 0;
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size;
        k->memver = memver; return 1;
    default:
        return 0;
    }
}

/* A numbered instruction becomes a copy of the earlier result. The copy
 * moves the RESULT, so it takes the result's form rather than the
 * operation's: a float comparison's result is an int 0/1 (its `w` and
 * `flt` describe the operands), and a float value is copied the way
 * irgen's own merges copy one, with `flt` clear. (A comparison's copy
 * has not been seen to survive: its destination is written once, so
 * copy propagation removes it. This keeps the form right if it does.) */
void vn_to_mov(struct ir_ins *i, int src)
{
    if (i->flt) {
        if (i->op == IR_CMP) {
            i->w = 4;
            i->sign = 0;
        }
        i->flt = 0;
    }
    to_mov(i, src);
}

/* Replace a computation that reproduces an earlier one in the same block with a
 * copy of that earlier result; fold/copyprop/dce then remove the redundancy.
 * The block is the run between labels; a store removes the loads whose bytes it
 * may overlap, and any other memory write bumps `memver` (part of a load's
 * key), so no load is reused across a write that could have changed it. Sound: temps are
 * single-assignment, so equal operand temps => equal value; VOLATILE accesses
 * are never numbered (vn_key rejects them), preserving every MMIO access. */
/* Does this instruction's key actually NAME a value?
 *
 * A key is built out of vreg numbers, so it identifies a value only
 * where a vreg identifies a value -- that is, where the vreg is assigned
 * once. mem2reg's phi destruction breaks that for exactly the vregs a
 * loop revolves around: an induction variable is assigned on entry and
 * again at the latch, so `cmp lt %i, %n` before the loop and the same
 * text inside it are two different comparisons with one key.
 *
 * Nothing had noticed, because irgen emits a loop's test once and there
 * was no second copy to collide with it. Loop rotation makes a second
 * copy on purpose, and GCSE promptly replaced the rotated test with the
 * guard's result -- a loop whose condition was evaluated once, before it
 * started. The number was not wrong; the assumption under it was.
 *
 * So: every value operand, and the result, must be singly assigned. */
int vn_stable(struct ir_func *fn, const struct defs *d, struct ir_ins *i)
{
    if (i->dst < 0 || i->dst >= fn->nvregs || d->cnt[i->dst] != 1)
        return 0;
    struct opnds o;
    value_opnds(i, &o);
    if (o.over)
        return 0;
    for (int k = 0; k < o.n; k++) {
        int v = o.v[k];
        if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
            return 0;
    }
    return 1;
}

static unsigned vn_mix(unsigned h, unsigned long x)
{
    h ^= (unsigned)x;
    h *= 0x9E3779B1u;
    h ^= (unsigned)(x >> 16 >> 16);
    h *= 0x85EBCA77u;
    return h ^ (h >> 15);
}

/* A hash of exactly the fields vn_eq compares, so equal keys always
 * meet in one bucket. */
static unsigned vn_hash(const struct vn *k)
{
    unsigned char pb[sizeof k->ptr];
    unsigned long pv = 0;
    memcpy(pb, &k->ptr, sizeof pb);
    for (size_t j = 0; j < sizeof pb; j++)
        pv = pv * 257 + pb[j];
    unsigned h = vn_mix(0, (unsigned long)k->op);
    h = vn_mix(h, (unsigned long)(long)k->a);
    h = vn_mix(h, (unsigned long)(long)k->b);
    h = vn_mix(h, (unsigned long)(long)k->w * 64 + (unsigned long)(long)k->size);
    h = vn_mix(h, (unsigned long)(long)k->sign * 4 + (unsigned long)(long)k->flt);
    h = vn_mix(h, (unsigned long)k->pred);
    h = vn_mix(h, (unsigned long)k->imm);
    h = vn_mix(h, pv);
    h = vn_mix(h, (unsigned long)(long)k->label);
    h = vn_mix(h, (unsigned long)(long)k->memver);
    h = vn_mix(h, (unsigned long)(k->has_ca * 2 + k->has_cb));
    if (k->has_ca) h = vn_mix(h, (unsigned long)k->ca);
    if (k->has_cb) h = vn_mix(h, (unsigned long)k->cb);
    return h;
}

/* `nins` bounds how many entries one walk can make; the buckets are
 * twice that, so a chain is short. */
void vntab_init(struct vntab *t, int nins, int nv, int index)
{
    memset(t, 0, sizeof *t);
    unsigned nb = 16;
    while (nb < (unsigned)nins * 2u && nb < (1u << 30))
        nb *= 2;
    t->mask = nb - 1;
    t->head = xmalloc((size_t)nb * sizeof *t->head);
    for (unsigned b = 0; b < nb; b++)
        t->head[b] = -1;
    if (index) {
        t->nv = nv;
        t->byv = xmalloc((size_t)(nv ? nv : 1) * sizeof *t->byv);
        for (int v = 0; v < nv; v++)
            t->byv[v] = -1;
    }
}

void vntab_free(struct vntab *t)
{
    free(t->e); free(t->head); free(t->r); free(t->byv); free(t->ld);
}

/* The result of the entry whose key equals k, or -1. */
int vntab_find(const struct vntab *t, const struct vn *k)
{
    for (int x = t->head[vn_hash(k) & t->mask]; x >= 0; x = t->e[x].next)
        if (vn_eq(&t->e[x].k, k))
            return t->e[x].k.result;
    return -1;
}

static void vntab_file(struct vntab *t, int v, int ent)
{
    if (v < 0 || v >= t->nv)
        return;
    if (t->nr == t->capr) {
        t->capr = t->capr ? t->capr * 2 : 64;
        t->r = xrealloc(t->r, (size_t)t->capr * sizeof *t->r);
    }
    t->r[t->nr].ent = ent;
    t->r[t->nr].next = t->byv[v];
    t->byv[v] = t->nr++;
}

void vntab_add(struct vntab *t, const struct vn *k)
{
    if (t->ne == t->cape) {
        t->cape = t->cape ? t->cape * 2 : 64;
        t->e = xrealloc(t->e, (size_t)t->cape * sizeof *t->e);
    }
    int x = t->ne++;
    struct vnent *e = &t->e[x];
    e->k = *k;
    e->h = vn_hash(k) & t->mask;
    e->prev = -1;
    e->next = t->head[e->h];
    if (e->next >= 0)
        t->e[e->next].prev = x;
    t->head[e->h] = x;
    e->live = 1;
    if (t->byv) {
        vntab_file(t, k->a, x);
        vntab_file(t, k->b, x);
        vntab_file(t, k->result, x);
        if (k->op == IR_LOAD || k->op == IR_LDVAR) {
            if (t->nld == t->capld) {
                t->capld = t->capld ? t->capld * 2 : 64;
                t->ld = xrealloc(t->ld, (size_t)t->capld * sizeof *t->ld);
            }
            t->ld[t->nld++] = x;
        }
    }
}

void vntab_unlink(struct vntab *t, int x)
{
    struct vnent *e = &t->e[x];
    if (!e->live)
        return;
    if (e->prev >= 0) t->e[e->prev].next = e->next;
    else              t->head[e->h] = e->next;
    if (e->next >= 0) t->e[e->next].prev = e->prev;
    e->live = 0;
}

/* Drop every entry that mentions vreg `t`, because something just gave
 * `t` a new value and the entries naming it describe the old one.
 *
 * This is what a block-local table can do that a dominator-scoped one
 * cannot: the pass walks straight-line code, so "has an operand been
 * reassigned since" is answered by having watched. It is also why LVN
 * does not need vn_stable's blanket refusal of multiply-assigned vregs,
 * which would throw away most of a loop body -- `arr[i]` reads `i`, and
 * an induction variable is assigned on every incoming edge. */
static void vn_kill(struct vntab *t, int v)
{
    if (v < 0 || v >= t->nv) {           /* not filed: look at them all */
        for (int x = 0; x < t->ne; x++)
            if (t->e[x].k.a == v || t->e[x].k.b == v || t->e[x].k.result == v)
                vntab_unlink(t, x);
        return;
    }
    for (int r = t->byv[v]; r >= 0; r = t->r[r].next)
        vntab_unlink(t, t->r[r].ent);
    t->byv[v] = -1;
}

/* Forget everything: a block boundary. Only what was filed is undone,
 * so a function of many small blocks pays for its entries, not for its
 * vregs at every label. */
static void vntab_clear(struct vntab *t)
{
    for (int x = 0; x < t->ne; x++) {
        struct vnent *e = &t->e[x];
        t->head[e->h] = -1;
        if (t->byv) {
            if (e->k.a >= 0 && e->k.a < t->nv) t->byv[e->k.a] = -1;
            if (e->k.b >= 0 && e->k.b < t->nv) t->byv[e->k.b] = -1;
            if (e->k.result >= 0 && e->k.result < t->nv)
                t->byv[e->k.result] = -1;
        }
    }
    t->ne = t->nr = t->nld = 0;
}

int pass_lvn(struct ir_func *fn)
{
    int changed = 0, memver = 0;
    struct vntab tb;
    vntab_init(&tb, fn->nins, fn->nvregs, 1);
    /* What a store can reach, so it forgets only those loads
     * (lvn_mem_kill). The definitions are counted once, before the walk
     * turns any instruction into a copy: a copy keeps its destination,
     * so the counts stay true, and the walk to a base follows the copy
     * to the same value. */
    struct defs d;
    compute_defs(fn, &d);
    char *taken = slots_taken(fn);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        enum ir_op op0 = i->op;
        if (op0 == IR_LABEL) { vntab_clear(&tb); continue; } /* block boundary */
        struct vn k;
        if (i->dst >= 0 && vn_key(i, memver, &k)) {
            int hit = vntab_find(&tb, &k);
            if (hit >= 0 && hit != i->dst) {
                vn_to_mov(i, hit);
                changed = 1;
                g_did.lvn++;
                op0 = IR_MOV;      /* what it is NOW, for the kill below */
            } else if (hit < 0) {
                vn_kill(&tb, i->dst);
                k.result = i->dst;
                vntab_add(&tb, &k);
                if (writes_memory(op0) &&
                    !lvn_mem_kill(fn, &d, taken, &tb, i)) {
                    memver++;
                    tb.nld = 0;    /* every load keyed so far is stale */
                }
                continue;
            }
        }
        /* Anything else that assigns a vreg -- a call's result, a store
         * to a slot, an instruction with no key at all -- invalidates
         * what named it. Inline asm and a landing pad write temps that
         * def_target cannot report, so they clear the table outright. */
        if (op0 == IR_ASM || op0 == IR_LANDING) {
            vntab_clear(&tb);
        } else {
            int t = def_target(i);
            if (t >= 0)
                vn_kill(&tb, t);
        }
        /* A store forgets only the reads it could reach; everything
         * else that writes memory forgets them all. */
        if (writes_memory(op0) &&
            !lvn_mem_kill(fn, &d, taken, &tb, i)) {
            memver++;
            tb.nld = 0;
        }
    }
    vntab_free(&tb);
    free(taken);
    free_defs(&d);
    return changed;
}
