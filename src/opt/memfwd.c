/* ---- local store-forwarding (a lightweight mem2reg) ----
 *
 * EmbIR keeps locals in memory (STVAR/LDVAR). Within an extended basic block a
 * store `STVAR L, v` makes every later `LDVAR L` yield v — until the next store
 * to L or a control-flow join. Forwarding v turns the reload into a copy that
 * copyprop/DCE then erase. Sound only for a local that is never address-taken
 * (so no aliased write can change it) and a full-width plain load (size 4 or 8,
 * no narrowing/extension between the store and the load). This is what lets an
 * inlined body's parameter plumbing (STVAR param, arg; LDVAR param) collapse to
 * the argument, so folding flows through the inline. */

#include "opt_int.h"

static int sf_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

int pass_storefwd(struct ir_func *fn)
{
    int nvars = fn->nvars;
    if (nvars == 0)
        return 0;
    char *taken = xcalloc((size_t)fn->nvregs, 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
            fn->ins[i].a < fn->nvregs)
            taken[fn->ins[i].a] = 1;
    int *cur = xmalloc((size_t)nvars * sizeof *cur);
    for (int v = 0; v < nvars; v++) cur[v] = -1;
    int changed = 0;
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if (in->op == IR_LABEL) {                 /* a join: values may differ */
            for (int v = 0; v < nvars; v++) cur[v] = -1;
        } else if (in->op == IR_STVAR) {
            int L = in->dst;
            /* Never a volatile local: each read of one must happen, and
             * read what is there. `volatile int v = 5; return v + v;`
             * returned 10 with no load at all. */
            /* (big-endian: only an access at the local's own size, as
             * mem2reg asks -- a narrower one is the stored value's HIGH
             * end there, not the low bits the MOV would carry) */
            if (L >= 0 && L < nvars)
                cur[L] = (!taken[L] && !in->vol &&
                          !(fn->locals && fn->locals[L].is_volatile) &&
                          sf_plain(in->size, 0, in->size) &&
                          !(target_big_endian() && fn->locals &&
                            in->size != fn->locals[L].size))
                             ? in->a : -1;
        } else if (in->op == IR_LDVAR) {
            int L = in->a;
            if (L >= 0 && L < nvars && !taken[L] && cur[L] >= 0 && !in->vol &&
                sf_plain(in->size, in->sign, in->w) &&
                !(target_big_endian() && fn->locals &&
                  in->size != fn->locals[L].size)) {
                in->op = IR_MOV;                  /* LDVAR L -> MOV of the stored temp */
                in->a = cur[L];
                in->b = -1;
                changed = 1;
            }
        }
    }
    free(taken); free(cur);
    return changed;
}

/* ---- reading a global nothing writes ------------------------------------
 *
 * `static const double pi = 3.14159...;` and fdlibm's two hundred other
 * constants were each an address and two loads at every use, where a
 * constant is two instructions. The type system here records no `const`,
 * and it would not be enough anyway (a cast can write through it), so
 * this PROVES it instead: a static global of this unit is read-only when
 * every use of its address, in every function, is a load address --
 * directly or at a constant offset -- no initializer anywhere points at
 * it, and no inline or top-level assembly could touch it. Its bytes are
 * then its initializer's, forever, and a load of them is that constant.
 * A float's constant is its bit pattern, which is what every backend
 * takes a float constant to be. Bytes a relocation fills (a pointer in
 * an initializer) are left alone. */
static const struct global **g_ro;
int g_nro;

static const struct global *ro_find(const struct global *g)
{
    for (int k = 0; g && k < g_nro; k++)
        if (g_ro[k] == g || (g_ro[k]->name && g->name &&
                             strcmp(g_ro[k]->name, g->name) == 0))
            return g_ro[k];
    return NULL;
}

void ro_globals(struct ir_unit *iu)
{
    struct unit *u = iu->src;
    g_nro = 0;
    free(g_ro);
    g_ro = NULL;
    if (!u || u->topasm)
        return;
    int cap = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (!g->is_static || !g->defined || g->absorbed || g->is_tls ||
            g->is_weak || g->section || !g->ty || !g->ty->kind ||
            g->ty->is_volatile)
            continue;
        if (g_nro == cap) {
            cap = cap ? cap * 2 : 16;
            g_ro = xrealloc(g_ro, (size_t)cap * sizeof *g_ro);
        }
        g_ro[g_nro++] = g;
    }
    /* withdraw every one that something could write or see */
    char *out = xcalloc((size_t)(g_nro ? g_nro : 1), 1);
    for (struct global *g = u->globals; g; g = g->next)
        for (int r = 0; r < g->nrelocs; r++)
            for (int k = 0; k < g_nro; k++)
                if (g->relocs[r].gtarget &&
                    (g->relocs[r].gtarget == g_ro[k] ||
                     (g->relocs[r].gtarget->name &&
                      strcmp(g->relocs[r].gtarget->name, g_ro[k]->name) == 0)))
                    out[k] = 1;
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        int nv = fn->nvregs;
        if (!nv) continue;
        int *gof = xmalloc((size_t)nv * sizeof *gof);   /* vreg -> ro index */
        struct defs d;
        compute_defs(fn, &d);
        for (int v = 0; v < nv; v++) gof[v] = -1;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_ASM) {
                memset(out, 1, (size_t)(g_nro ? g_nro : 1));
                break;
            }
            if (i->op == IR_GADDR && i->glob) {
                const struct global *r = ro_find(i->glob);
                if (!r) continue;
                int k;
                for (k = 0; k < g_nro && g_ro[k] != r; k++) ;
                if (i->dst >= 0 && i->dst < nv && d.cnt[i->dst] == 1)
                    gof[i->dst] = k;
                else
                    out[k] = 1;
            } else if (i->op == IR_ADD && i->a >= 0 && i->a < nv &&
                       gof[i->a] >= 0 && i->dst >= 0 && i->dst < nv &&
                       d.cnt[i->dst] == 1) {
                /* Any offset, constant or not: this runs on the IR as
                 * irgen left it, where `table[1]` is still `mul 1, 4`,
                 * and a read at a computed offset is still only a read.
                 * pass_roload folds only the constant ones. */
                gof[i->dst] = gof[i->a];
            }
        }
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int *ops[3], nops = 0;
            if (i->a >= 0 && i->a < nv && gof[i->a] >= 0) ops[nops++] = &i->a;
            if (!i->imm_b && i->b >= 0 && i->b < nv && gof[i->b] >= 0)
                ops[nops++] = &i->b;
            if (i->c >= 0 && i->c < nv && gof[i->c] >= 0) ops[nops++] = &i->c;
            for (int k = 0; k < nops; k++) {
                int ok = ops[k] == &i->a &&
                         ((i->op == IR_LOAD && !i->vol) ||
                          (i->op == IR_ADD && i->dst >= 0 && gof[i->dst] >= 0));
                if (!ok) out[gof[*ops[k]]] = 1;
            }
            if (i->op == IR_CALL)
                for (int k = 0; k < i->nargs; k++) {
                    int v = i->argv[k].vreg;
                    if (v >= 0 && v < nv && gof[v] >= 0) out[gof[v]] = 1;
                }
            if (i->op == IR_RET && i->a >= 0 && i->a < nv && gof[i->a] >= 0)
                out[gof[i->a]] = 1;
        }
        free(gof); free(d.cnt); free(d.ins);
    }
    int k = 0;
    for (int j = 0; j < g_nro; j++)
        if (!out[j]) g_ro[k++] = g_ro[j];
    g_nro = k;
    free(out);
}

/* The `size` bytes at `off` of read-only global g, or 0 if a relocation
 * fills any of them or they are outside it. */
static int ro_bytes(const struct global *g, long off, int size,
                    unsigned long *out)
{
    int gs = global_size(g);
    unsigned long v = 0;
    if (off < 0 || off + size > gs || size < 1 || size > 8)
        return 0;
    for (int r = 0; r < g->nrelocs; r++) {
        long ro = g->relocs[r].off;
        if (ro < off + size && off < ro + 8)
            return 0;
    }
    /* The bytes are the object's image, in the target's order: the most
     * significant first is the LAST of them little-endian, the first
     * big-endian. A scalar without an image is its value, stored the same
     * way. */
    for (int k = 0; k < size; k++) {
        int b = target_big_endian() ? k : size - 1 - k;
        long at = off + b;
        unsigned char byte;
        if (g->init_bytes)
            byte = at < g->init_len ? (unsigned char)g->init_bytes[at] : 0;
        else {
            /* zero past the value's eight bytes, as target_put_uint writes
             * the image (a zero-initialized aggregate's is all of it) */
            long sig = target_big_endian() ? gs - 1 - at : at;
            byte = sig < 8 ? (unsigned char)((unsigned long)g->init >> (8 * sig))
                           : 0;
        }
        v = v << 8 | byte;
    }
    *out = v;
    return 1;
}

int pass_roload(struct ir_func *fn)
{
    int nv = fn->nvregs, changed = 0;
    if (!g_nro || !nv)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    const struct global **gv = xcalloc((size_t)nv, sizeof *gv);
    long *goff = xcalloc((size_t)nv, sizeof *goff);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long c;
        if (i->dst < 0 || i->dst >= nv || d.cnt[i->dst] != 1)
            continue;
        if (i->op == IR_GADDR && i->glob)
            gv[i->dst] = ro_find(i->glob);
        else if (i->op == IR_ADD && i->a >= 0 && i->a < nv && gv[i->a] &&
                 (i->imm_b ? (c = i->imm, 1)
                           : get_const(fn, &d, i->b, &c))) {
            gv[i->dst] = gv[i->a];
            goff[i->dst] = goff[i->a] + c;
        }
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        unsigned long v;
        if (i->op != IR_LOAD || i->vol || i->a < 0 || i->a >= nv ||
            !gv[i->a] || i->w > 8)
            continue;
        if (!ro_bytes(gv[i->a], goff[i->a], i->size, &v))
            continue;
        to_const(i, fold_ext((long)v, i->size, i->sign, i->w));
        i->flt = 0;
        changed = 1;
    }
    free(gv); free(goff); free(d.cnt); free(d.ins);
    return changed;
}

/* ---- forwarding through a punning union --------------------------------
 *
 * fdlibm reads a double's words through a union -- EXTRACT_WORDS,
 * GET_HIGH_WORD, GET_LOW_WORD -- and every such union stayed a stack
 * object: the double stored, one word loaded back, in nearly every
 * function of the math library. mem2reg cannot promote a union (it is
 * not a scalar) and storefwd will not touch a local whose address is
 * taken.
 *
 * Here a local is PRIVATE when every use of its address is a load or
 * store address, or a memzero's (`union { ... } v = { x }` zeroes v before
 * storing x), directly or at a constant offset -- nothing can see it but
 * these accesses. In a block, a load that one earlier store covers
 * becomes that store's value: a copy or a bit-reinterpret at the same
 * width and offset, and at half the width the low or high word
 * (reinterpret, shift by 32, truncate). A private local left with no load
 * loses its stores, and DCE the addresses.
 *
 * And the other way: SET_HIGH_WORD stores the double, then one word over
 * its high half, then loads the double back. When that word is the same
 * half changed by a constant (pun_word_foldable) the store does not end
 * the 8-byte value -- it makes a new one, the old value with that half
 * replaced (mask, zero-extend, shift, or), which a later load forwards
 * like any other and fold_word_insert then makes one operation. fabs
 * and copysign were a store, a store and a load through the frame on
 * every target but AVR; they are now an and, and an and and an or, of
 * registers. Any other word stays a store: the general replacement is
 * more code than the memory round trip it saves.
 *
 * Not on AVR, whose 64-bit shift is a byte loop and whose double is four
 * bytes anyway. */
struct pun_rec { int off, width, val, flt; };

/* plan[n] == 2: the 4-byte store at n puts its word into the 8-byte
 * value pm[n].old holds, making pm[n].newv; bits, when not -1, is that
 * value's bits as an integer already (what the word was read from) */
struct pun_merge { struct pun_rec old; int newv, bits; };

/* Is the word a store puts into the high half of `old` that same half
 * changed by a constant -- SET_HIGH_WORD(x, GET_HIGH_WORD(x) & c), and
 * |, ^, + and - -- which fold_word_insert makes one 64-bit operation of
 * x? Only then is the merge a gain: in general a mask, an extend, a
 * shift and an or of 64-bit values (pairs, on a 32-bit machine) are more
 * code than the store, the store and the load they replace -- libm grew
 * 88 bytes on Cortex-M and 534 on x86-64 doing it every time. Returns
 * the vreg the high half was read from (old's value, or its bits), or
 * -1. */
static int pun_word_foldable(const struct ir_func *fn, const struct defs *d,
                             const struct pun_rec *old, int h)
{
    if (h < 0 || h >= fn->nvregs || d->cnt[h] != 1 || d->ins[h] < 0)
        return -1;
    const struct ir_ins *Y = &fn->ins[d->ins[h]];
    long c;
    int s;
    if (Y->w != 4 || Y->flt ||
        !(Y->op == IR_AND || Y->op == IR_OR || Y->op == IR_XOR ||
          Y->op == IR_ADD || Y->op == IR_SUB))
        return -1;
    if (Y->imm_b || get_const((struct ir_func *)fn, (struct defs *)d,
                              Y->b, &c))
        s = Y->a;
    else if (Y->op != IR_SUB &&
             get_const((struct ir_func *)fn, (struct defs *)d, Y->a, &c))
        s = Y->b;
    else
        return -1;
    if (s < 0 || s >= fn->nvregs || d->cnt[s] != 1 || d->ins[s] < 0)
        return -1;
    const struct ir_ins *S = &fn->ins[d->ins[s]];
    if (S->op != IR_SHR || S->w != 8 ||
        !(S->imm_b ? S->imm == 32
                   : get_const((struct ir_func *)fn, (struct defs *)d,
                               S->b, &c) && c == 32))
        return -1;
    int x = S->a;
    if (x == old->val)
        return x;
    /* the read side reinterpreted the double first: the same bits */
    if (old->flt && x >= 0 && x < fn->nvregs && d->cnt[x] == 1 &&
        d->ins[x] >= 0 && fn->ins[d->ins[x]].op == IR_BITCAST &&
        fn->ins[d->ins[x]].a == old->val)
        return x;
    return -1;
}

int pass_punfwd(struct ir_func *fn)
{
    int nv = fn->nvregs, nvars = fn->nvars, changed = 0;
    /* EMBCC_NO_PUNMERGE=1: a word store ends the 8-byte value again, as
     * before the merge, for bisecting a difference to it */
    int g_no_punmerge = getenv("EMBCC_NO_PUNMERGE") != NULL;
    if (!nvars || !nv || target_get() == TARGET_AVR)
        return 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ASM)
            return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *base = xmalloc((size_t)nv * sizeof *base);
    long *boff = xcalloc((size_t)nv, sizeof *boff);
    char *bad = xcalloc((size_t)nvars, 1);
    for (int v = 0; v < nv; v++) base[v] = -1;
    /* the address of a local, and constant offsets from it */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long c;
        if (i->dst < 0 || i->dst >= nv || d.cnt[i->dst] != 1)
            continue;
        if (i->op == IR_ADDR && i->a >= 0 && i->a < nvars) {
            base[i->dst] = i->a;
            boff[i->dst] = 0;
        } else if (i->op == IR_ADD && i->a >= 0 && i->a < nv &&
                   base[i->a] >= 0 &&
                   (i->imm_b ? (c = i->imm, 1)
                             : get_const(fn, &d, i->b, &c))) {
            base[i->dst] = base[i->a];
            boff[i->dst] = boff[i->a] + c;
        }
    }
    /* private: every read of such an address is a load/store address or
     * another offset; and no whole-local access */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if ((i->op == IR_LDVAR && i->a >= 0 && i->a < nvars) ||
            (i->op == IR_STVAR && i->dst >= 0 && i->dst < nvars)) {
            bad[i->op == IR_LDVAR ? i->a : i->dst] = 1;
            /* ...and an address STORED in a variable has escaped: it can
             * be read back past a join, where nothing here follows it,
             * and the local was declared unread and its stores dropped
             * (`p = &s; if (c) ...; return p->x;` read garbage at -O1). */
            if (i->op == IR_STVAR && i->a >= 0 && i->a < nv &&
                base[i->a] >= 0)
                bad[base[i->a]] = 1;
            continue;
        }
        int *ops[4], nops = 0;
        if (i->a >= 0 && i->a < nv && base[i->a] >= 0) ops[nops++] = &i->a;
        if (!i->imm_b && i->b >= 0 && i->b < nv && base[i->b] >= 0)
            ops[nops++] = &i->b;
        if (i->c >= 0 && i->c < nv && base[i->c] >= 0) ops[nops++] = &i->c;
        for (int k = 0; k < nops; k++) {
            int ok = ops[k] == &i->a &&
                     (i->op == IR_LOAD || i->op == IR_STORE ||
                      i->op == IR_MEMZERO ||
                      (i->op == IR_ADD && i->dst >= 0 && base[i->dst] >= 0));
            if (!ok)
                bad[base[*ops[k]]] = 1;
        }
        if (i->op == IR_CALL)
            for (int k = 0; k < i->nargs; k++) {
                int v = i->argv[k].vreg;
                if (v >= 0 && v < nv && base[v] >= 0) bad[base[v]] = 1;
            }
        if (i->op == IR_RET && i->a >= 0 && i->a < nv && base[i->a] >= 0)
            bad[base[i->a]] = 1;
    }
    /* forward, a block at a time */
    struct pun_rec (*rec)[4] = xcalloc((size_t)nvars, sizeof *rec);
    int *nrec = xcalloc((size_t)nvars, sizeof *nrec);
    int *plan = xmalloc((size_t)fn->nins * sizeof *plan);  /* rec index */
    struct pun_rec *pr = xmalloc((size_t)fn->nins * sizeof *pr);
    struct pun_merge *pm = xmalloc((size_t)fn->nins * sizeof *pm);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        plan[n] = 0;
        if (i->op == IR_LABEL || i->op == IR_CALL) {
            /* a join; and a call cannot touch a private local, but a
             * longjmp-style return into the middle is not worth reasoning
             * about -- start again either way */
            memset(nrec, 0, (size_t)nvars * sizeof *nrec);
            continue;
        }
        if (i->op == IR_MEMZERO && i->a >= 0 && i->a < nv &&
            base[i->a] >= 0 && !bad[base[i->a]]) {
            /* zeroes: whatever it overlaps no longer holds a value */
            int L = base[i->a], off = (int)boff[i->a], wid = i->size, k = 0;
            for (int j = 0; j < nrec[L]; j++) {
                struct pun_rec *r = &rec[L][j];
                if (r->off + r->width <= off || off + wid <= r->off)
                    rec[L][k++] = *r;
            }
            nrec[L] = k;
            continue;
        }
        if ((i->op != IR_LOAD && i->op != IR_STORE) || i->a < 0 ||
            i->a >= nv || base[i->a] < 0 || bad[base[i->a]] || i->vol)
            continue;
        int L = base[i->a], off = (int)boff[i->a], wid = i->size;
        if (i->op == IR_STORE) {
            int k = 0, merge = 0;
            struct pun_rec old;
            for (int j = 0; j < nrec[L]; j++) {      /* drop what it overlaps */
                struct pun_rec *r = &rec[L][j];
                if (r->off + r->width <= off || off + wid <= r->off)
                    rec[L][k++] = *r;
                else if (!merge && r->width == 8 && wid == 4 && i->b >= 0 &&
                         (off == r->off || off == r->off + 4)) {
                    old = *r;                    /* ...or write a word of */
                    merge = 1;
                }
            }
            int fb = -1;
            if (merge && k < 4 && !g_no_punmerge &&
                ((off != old.off) != target_big_endian()) &&
                (fb = pun_word_foldable(fn, &d, &old, i->b)) >= 0 &&
                !i->flt) {
                pm[n].old = old;
                pm[n].bits = fb != old.val ? fb : -1;
                pm[n].newv = fn->nvregs++;
                plan[n] = 2;
                any = 1;
                rec[L][k].off = old.off; rec[L][k].width = 8;
                rec[L][k].val = pm[n].newv; rec[L][k].flt = 0;
                k++;
            }
            nrec[L] = k;
            if (k < 4 && (wid == 4 || wid == 8) && i->b >= 0) {
                rec[L][k].off = off; rec[L][k].width = wid;
                rec[L][k].val = i->b; rec[L][k].flt = i->flt;
                nrec[L] = k + 1;
            }
            continue;
        }
        for (int j = 0; j < nrec[L]; j++) {          /* a LOAD */
            struct pun_rec *r = &rec[L][j];
            if (i->flt && r->width != wid)
                continue;      /* half a double as a float: not handled */
            if ((r->width == wid && r->off == off) ||
                (r->width == 8 && wid == 4 &&
                 (off == r->off || off == r->off + 4))) {
                plan[n] = 1;
                pr[n] = *r;
                any = 1;
                break;
            }
        }
    }
    if (any) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins in = fn->ins[n];
            newpos[n] = nb.n;
            if (!plan[n]) {
                *ib_push(&nb) = in;
                continue;
            }
            if (plan[n] == 2) {
                /* the store stays (a later load past a join may read the
                 * memory), and after it the value with its word put in */
                *ib_push(&nb) = in;
                const struct pun_merge *g = &pm[n];
                int hi = ((int)boff[in.a] != g->old.off) != target_big_endian();
                int x = g->old.val, h = in.b;
                struct ir_ins t;
                memset(&t, 0, sizeof t);
                t.a = t.b = t.c = -1;
                t.line = in.line; t.col = in.col;
                if (g->bits >= 0)                /* read as bits already */
                    x = g->bits;
                else if (g->old.flt) {           /* the double's bits */
                    struct ir_ins *b = ib_push(&nb);
                    *b = t;
                    b->op = IR_BITCAST; b->a = x; b->w = 8; b->size = 8;
                    b->sign = 1;
                    b->dst = x = fn->nvregs++;
                }
                if (in.flt) {                    /* the float's bits */
                    struct ir_ins *b = ib_push(&nb);
                    *b = t;
                    b->op = IR_BITCAST; b->a = h; b->w = 4; b->size = 4;
                    b->sign = 1;
                    b->dst = h = fn->nvregs++;
                }
                struct ir_ins *k = ib_push(&nb);
                *k = t;
                k->op = IR_CONST; k->w = 8;
                k->imm = hi ? 0xffffffffL : (long)(~0xffffffffUL);
                int km = k->dst = fn->nvregs++;
                struct ir_ins *an = ib_push(&nb);
                *an = t;
                an->op = IR_AND; an->a = x; an->b = km; an->w = 8;
                int kept = an->dst = fn->nvregs++;
                struct ir_ins *z = ib_push(&nb);
                *z = t;
                z->op = IR_EXT; z->a = h; z->size = 4; z->sign = 0; z->w = 8;
                int word = z->dst = fn->nvregs++;
                if (hi) {
                    struct ir_ins *c = ib_push(&nb);
                    *c = t;
                    c->op = IR_CONST; c->imm = 32; c->w = 8;
                    int k32 = c->dst = fn->nvregs++;
                    struct ir_ins *sh = ib_push(&nb);
                    *sh = t;
                    sh->op = IR_SHL; sh->a = word; sh->b = k32; sh->w = 8;
                    word = sh->dst = fn->nvregs++;
                }
                struct ir_ins *o = ib_push(&nb);
                *o = t;
                o->op = IR_OR; o->a = kept; o->b = word; o->w = 8;
                o->dst = g->newv;
                changed = 1;
                continue;
            }
            const struct pun_rec *r = &pr[n];
            /* the word at +4 of an 8-byte store is its high half
             * little-endian, its low half big-endian */
            int x = r->val,
                hi = ((int)boff[in.a] != r->off) != target_big_endian();
            if (r->width == in.size) {
                struct ir_ins *m = ib_push(&nb);
                *m = in;
                m->op = r->flt == in.flt ? IR_MOV : IR_BITCAST;
                m->a = x; m->b = -1; m->c = -1;
                m->w = in.size; m->size = in.size;
                m->sign = in.flt ? 0 : 1;
                m->vol = 0; m->natural = 0;
                changed = 1;
                continue;
            }
            if (r->flt) {                   /* the double's bits */
                struct ir_ins *b = ib_push(&nb);
                *b = in;
                b->op = IR_BITCAST; b->a = x; b->b = -1; b->c = -1;
                b->w = 8; b->size = 8; b->sign = 1; b->flt = 0;
                b->vol = 0; b->natural = 0;
                b->dst = x = fn->nvregs++;
            }
            if (hi) {
                struct ir_ins *k = ib_push(&nb);
                memset(k, 0, sizeof *k);
                k->op = IR_CONST; k->imm = 32; k->w = 8;
                k->a = k->b = k->c = -1;
                k->line = in.line; k->col = in.col;
                int kdst = k->dst = fn->nvregs++;   /* (k dangles after the
                                                       * next ib_push) */
                struct ir_ins *sh = ib_push(&nb);
                memset(sh, 0, sizeof *sh);
                sh->op = IR_SHR; sh->a = x; sh->b = kdst; sh->w = 8;
                sh->sign = 0; sh->c = -1;
                sh->line = in.line; sh->col = in.col;
                sh->dst = x = fn->nvregs++;
            }
            struct ir_ins *e = ib_push(&nb);
            *e = in;
            e->op = IR_EXT; e->a = x; e->b = -1; e->c = -1;
            e->size = 4; e->flt = 0; e->vol = 0; e->natural = 0;
            changed = 1;
        }
        newpos[fn->nins] = nb.n;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    /* a private local nothing loads from any more: its stores are dead */
    {
        char *loaded = xcalloc((size_t)nvars, 1);
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if ((i->op == IR_LOAD || i->op == IR_MEMCPY) && i->a >= 0 &&
                i->a < nv && base[i->a] >= 0)
                loaded[base[i->a]] = 1;
            if (i->op == IR_MEMCPY && i->b >= 0 && i->b < nv &&
                base[i->b] >= 0)
                loaded[base[i->b]] = 1;
        }
        int k = 0;
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            newpos[n] = k;
            if ((i->op == IR_STORE || i->op == IR_MEMZERO) && i->a >= 0 &&
                i->a < nv && base[i->a] >= 0 && !bad[base[i->a]] &&
                !loaded[base[i->a]] && !i->vol) {
                changed = 1;
                continue;
            }
            fn->ins[k++] = fn->ins[n];
        }
        newpos[fn->nins] = k;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        fn->nins = k;
        free(loaded);
    }
    free(base); free(boff); free(bad); free(rec); free(nrec);
    free(plan); free(pr); free(pm);
    free(d.cnt); free(d.ins);
    return changed;
}

/* ==== constants stored to a global, forwarded across blocks (-O2) =========
 *
 * A filter that fills its coefficient table at the top of the function --
 * `for (i = 0; i < 16; i++) h[i] = ...;`, unrolled into sixteen stores of
 * constants -- and reads it in the loop after had a load of h[k] at every
 * tap, where clang multiplies by the constant. storefwd stops at a join
 * and at a global. This is the global, across blocks: a forward
 * must-available dataflow whose facts are "bytes [off, off+size) of
 * global g hold the constant c", gen'd by a store of a constant to g at a
 * constant offset and killed by
 *
 *   - a store to g at another or an unknown offset (its overlapping facts,
 *     or all of g's), a memcpy or memzero into g, a vector store into g;
 *   - a store through an address whose object is not known: everything;
 *   - a call that may write memory, inline asm, an atomic: everything.
 *
 * An address's object is followed through its definitions -- gaddr, addr,
 * a copy, an add of a value that is not itself an address -- and a name
 * defined more than once (a walking pointer) has one only when every
 * definition agrees. A load the facts cover exactly becomes the constant,
 * truncated and extended as the load reads it. EMBCC_NO_GSTOREFWD turns
 * it off. */
struct gsf { const struct global *g; long off, val; int size; };
/* Offsets in program order within a block: a name the block has defined
 * by gaddr, a copy, or an add of a constant to an address it knows has
 * that offset from here on -- a fully unrolled pointer walk reuses one
 * name for every copy, which no single-definition map can follow. */
struct gloc { long *off; int *st; int stamp; };
struct gsfs { struct gsf *f; int n; };

#define GSF_MAX 96
#define GB_UNK  ((const struct global *)-1)    /* any object at all */
#define GB_LOC  ((const struct global *)-2)    /* a frame object */

static int gsf_find(const struct gsfs *s, const struct global *g, long off,
                    int size)
{
    for (int k = 0; k < s->n; k++)
        if (s->f[k].g == g && s->f[k].off == off && s->f[k].size == size)
            return k;
    return -1;
}

static void gsf_kill(struct gsfs *s, const struct global *g, long off,
                     int size, int all)
{
    int j = 0;
    for (int k = 0; k < s->n; k++) {
        struct gsf *f = &s->f[k];
        int hit = g == GB_UNK || (f->g == g &&
                  (all || (f->off < off + size && off < f->off + f->size)));
        if (!hit)
            s->f[j++] = *f;
    }
    s->n = j;
}

/* One instruction's effect on the facts; with `rw`, a load they cover
 * becomes its constant. Returns 1 when it rewrote. */
static int gsf_off(const struct gloc *lc, const long *go, const char *gk,
                   int v, long *off)
{
    if (lc->st[v] == lc->stamp) { *off = lc->off[v]; return 1; }
    if (lc->st[v] == -lc->stamp) return 0;       /* redefined unknown here */
    if (gk[v]) { *off = go[v]; return 1; }
    return 0;
}

static void gsf_def(struct ir_func *fn, const struct defs *d,
                    struct gloc *lc, const long *go, const char *gk,
                    const struct ir_ins *i)
{
    int nv = fn->nvregs, t = def_target(i);
    if (t < 0 || t >= nv)
        return;
    long o, c = 0;
    if (i->op == IR_GADDR) { lc->off[t] = 0; lc->st[t] = lc->stamp; return; }
    if (i->op == IR_MOV && i->a >= 0 && i->a < nv &&
        gsf_off(lc, go, gk, i->a, &o)) {
        lc->off[t] = o; lc->st[t] = lc->stamp; return;
    }
    if (i->op == IR_ADD && i->a >= 0 && i->a < nv &&
        gsf_off(lc, go, gk, i->a, &o)) {
        int have = 0;
        if (i->imm_b) { c = i->imm; have = 1; }
        else if (i->b >= 0 && i->b < nv && d->cnt[i->b] == 1 &&
                 d->ins[i->b] >= 0 && fn->ins[d->ins[i->b]].op == IR_CONST) {
            c = fn->ins[d->ins[i->b]].imm; have = 1;
        }
        if (have) { lc->off[t] = o + c; lc->st[t] = lc->stamp; return; }
    }
    lc->st[t] = -lc->stamp;
}

static int gsf_step(struct ir_func *fn, const struct defs *d,
                    const struct global **gb, const long *go, const char *gk,
                    struct gloc *lc, struct gsfs *s, struct ir_ins *i, int rw)
{
    int nv = fn->nvregs;
#define BASE(v) ((v) >= 0 && (v) < nv ? gb[v] : GB_UNK)
    switch (i->op) {
    case IR_STORE: {
        const struct global *g = BASE(i->a);
        if (i->vol) { gsf_kill(s, GB_UNK, 0, 0, 1); return 0; }
        if (g == GB_LOC) return 0;
        if (!g || g == GB_UNK) { gsf_kill(s, GB_UNK, 0, 0, 1); return 0; }
        long off;
        if (!gsf_off(lc, go, gk, i->a, &off)) {
            gsf_kill(s, g, 0, 0, 1);
            return 0;
        }
        off += i->memoff > 0 ? i->memoff : 0;
        gsf_kill(s, g, off, i->size, 0);
        long c;
        if (i->b >= 0 && i->b < nv && d->cnt[i->b] == 1 && d->ins[i->b] >= 0 &&
            fn->ins[d->ins[i->b]].op == IR_CONST && s->n < GSF_MAX &&
            i->size >= 1 && i->size <= 8) {
            c = fn->ins[d->ins[i->b]].imm;
            if (i->size < 8)
                c = (long)((unsigned long)c & ((1UL << (8 * i->size)) - 1));
            s->f[s->n].g = g; s->f[s->n].off = off; s->f[s->n].size = i->size;
            s->f[s->n].val = c; s->n++;
        }
        return 0;
    }
    case IR_MEMCPY: case IR_MEMZERO: case IR_VSTORE: {
        const struct global *g = BASE(i->a);
        if (g == GB_LOC) return 0;
        if (!g || g == GB_UNK || i->vol) gsf_kill(s, GB_UNK, 0, 0, 1);
        else gsf_kill(s, g, 0, 0, 1);
        return 0;
    }
    case IR_CALL:
        if (!i->indirect && i->callee && i->callee->inf_no_write)
            return 0;
        gsf_kill(s, GB_UNK, 0, 0, 1);
        return 0;
    case IR_ASM: case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS:
    case IR_CAS16: case IR_VA_START: case IR_LANDING:
        gsf_kill(s, GB_UNK, 0, 0, 1);
        return 0;
    case IR_LOAD: {
        if (!rw || i->vol || i->flash || i->dst < 0 || i->size < 1 ||
            i->size > 8 || i->w > 8)
            return 0;
        const struct global *g = BASE(i->a);
        long off;
        if (!g || g == GB_UNK || g == GB_LOC || !gsf_off(lc, go, gk, i->a, &off))
            return 0;
        off += i->memoff > 0 ? i->memoff : 0;
        int k = gsf_find(s, g, off, i->size);
        if (k < 0)
            return 0;
        unsigned long v = (unsigned long)s->f[k].val;
        if (i->size < 8 && i->sign && (v >> (8 * i->size - 1)) & 1)
            v |= ~0UL << (8 * i->size);          /* sign-extended */
        if (i->w == 4)
            v &= 0xffffffffUL;
        int dst = i->dst, w = i->w, line = i->line, col = i->col;
        memset(i, 0, sizeof *i);
        i->op = IR_CONST; i->dst = dst; i->w = w ? w : 4; i->imm = (long)v;
        i->a = i->b = -1; i->line = line; i->col = col;
        return 1;
    }
    default:
        return 0;
    }
#undef BASE
}

int pass_gstorefwd(struct ir_func *fn)
{
    if (fn->nins == 0 || getenv("EMBCC_NO_GSTOREFWD"))
        return 0;
    int nv = fn->nvregs;
    struct defs d;
    compute_defs(fn, &d);
    /* the object each address-valued vreg points into, and its constant
     * offset when one definition chain fixes it */
    const struct global **gb = xcalloc((size_t)(nv ? nv : 1), sizeof *gb);
    long *go = xcalloc((size_t)(nv ? nv : 1), sizeof *go);
    char *gk = xcalloc((size_t)(nv ? nv : 1), 1);
    char *any = xcalloc((size_t)(nv ? nv : 1), 1);
    int pw = target_ptr_size(), have = 0;
    for (int round = 0, grew = 1; grew && round < 16; round++) {
        grew = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int t = def_target(i);
            if (t < 0 || t >= nv || gb[t] == GB_UNK)
                continue;
            /* NULL: not an address as far as this knows (a constant, an
             * index -- or a loaded pointer, which a store through then
             * treats as unknown) */
            const struct global *b = NULL;
            if (i->op == IR_GADDR) {
                b = i->glob && !i->glob->is_tls && i->glob->ty &&
                    !i->glob->ty->is_volatile ? i->glob : GB_UNK;
            } else if (i->op == IR_ADDR)
                b = GB_LOC;
            else if (i->op == IR_MOV && i->a >= 0 && i->a < nv)
                b = any[i->a] ? gb[i->a] : NULL;
            else if (i->op == IR_ADD && i->w == pw && !i->flt) {
                const struct global *ba = i->a >= 0 && i->a < nv && any[i->a]
                                          ? gb[i->a] : NULL;
                const struct global *bb = !i->imm_b && i->b >= 0 &&
                                          i->b < nv && any[i->b]
                                          ? gb[i->b] : NULL;
                if (ba && !bb) b = ba;
                else if (bb && !ba) b = bb;
                else if (ba && bb) b = GB_UNK;
            }
            if (b == NULL)
                continue;                /* no address here (yet) */
            if (!any[t]) {
                any[t] = 1; gb[t] = b; grew = 1; have = 1;
            } else if (gb[t] != b) {
                gb[t] = GB_UNK; grew = 1;
            }
        }
    }
    /* Validation, to a fixed point: each definition of a vreg given an
     * object must derive that same object from the final map -- a copy of
     * an undecided value (a loaded pointer) does not. And a value computed
     * from an address by anything but a copy or an add of a non-address
     * is TAINTED: `&g + (x - (long)&g)` points wherever x does, so adding
     * a tainted value gives no object. */
    char *taint = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int round = 0, ch = 1; ch && round < 32; round++) {
        ch = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int t = def_target(i);
            if (t < 0 || t >= nv)
                continue;
            int addr_in = 0, taint_in = 0;
            struct opnds o;
            value_opnds(i, &o);
            for (int q = 0; q < o.n; q++) {
                int v = o.v[q];
                if (v < 0 || v >= nv) continue;
                if (any[v] && gb[v]) addr_in = 1;
                if (taint[v]) taint_in = 1;
            }
            int keeps = i->op == IR_MOV || (i->op == IR_ADD && i->w == pw);
            if (!keeps && (addr_in || taint_in) && !taint[t] &&
                i->op != IR_LOAD) {
                taint[t] = 1; ch = 1;
            }
            if (!any[t] || gb[t] == GB_UNK)
                continue;
            const struct global *bd = GB_UNK;
            if (i->op == IR_GADDR && i->glob && !i->glob->is_tls &&
                i->glob->ty && !i->glob->ty->is_volatile)
                bd = i->glob;
            else if (i->op == IR_ADDR)
                bd = GB_LOC;
            else if (i->op == IR_MOV && i->a >= 0 && i->a < nv && any[i->a])
                bd = gb[i->a];
            else if (i->op == IR_ADD && i->w == pw && !i->flt) {
                int ia = i->a >= 0 && i->a < nv && any[i->a];
                int ib = !i->imm_b && i->b >= 0 && i->b < nv && any[i->b];
                int ta = i->a >= 0 && i->a < nv && taint[i->a];
                int tb = !i->imm_b && i->b >= 0 && i->b < nv && taint[i->b];
                if (ia && !ib && !tb) bd = gb[i->a];
                else if (ib && !ia && !ta) bd = gb[i->b];
            }
            if (bd != gb[t]) {
                gb[t] = GB_UNK; ch = 1;
            }
        }
    }
    free(taint);
    /* any address-valued vreg nothing decided is unknown */
    for (int v = 0; v < nv; v++)
        if (!any[v])
            gb[v] = NULL;
    for (int v = 0; v < nv; v++) {
        if (!gb[v] || gb[v] == GB_UNK || gb[v] == GB_LOC || d.cnt[v] != 1 ||
            d.ins[v] < 0)
            continue;
        const struct ir_ins *i = &fn->ins[d.ins[v]];
        if (i->op == IR_GADDR) { go[v] = 0; gk[v] = 1; }
    }
    for (int round = 0, grew = 1; grew && round < 8; round++) {
        grew = 0;
        for (int v = 0; v < nv; v++) {
            if (gk[v] || !gb[v] || gb[v] == GB_UNK || gb[v] == GB_LOC ||
                d.cnt[v] != 1 || d.ins[v] < 0)
                continue;
            const struct ir_ins *i = &fn->ins[d.ins[v]];
            long c;
            if (i->op == IR_MOV && i->a >= 0 && i->a < nv && gk[i->a]) {
                go[v] = go[i->a]; gk[v] = 1; grew = 1;
            } else if (i->op == IR_ADD && i->a >= 0 && i->a < nv && gk[i->a]) {
                if (i->imm_b) c = i->imm;
                else if (i->b >= 0 && i->b < nv && d.cnt[i->b] == 1 &&
                         d.ins[i->b] >= 0 && fn->ins[d.ins[i->b]].op == IR_CONST)
                    c = fn->ins[d.ins[i->b]].imm;
                else
                    continue;
                go[v] = go[i->a] + c; gk[v] = 1; grew = 1;
            }
        }
    }
    int changed = 0;
    if (!have) {
        free(gb); free(go); free(gk); free(any); free_defs(&d);
        return 0;
    }
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    struct gsfs *out = xcalloc((size_t)nbb, sizeof *out);
    char *done = xcalloc((size_t)nbb, 1);
    struct gsf *buf = xmalloc(GSF_MAX * sizeof *buf);
    for (int b = 0; b < nbb; b++)
        out[b].f = xmalloc(GSF_MAX * sizeof *out[b].f);
    struct gloc lc = { xcalloc((size_t)(nv ? nv : 1), sizeof(long)),
                       xcalloc((size_t)(nv ? nv : 1), sizeof(int)), 0 };
    for (int iter = 0, again = 1; again && iter < 32; iter++) {
        again = 0;
        for (int oi = 0; oi < norder; oi++) {
            int b = order[oi];
            struct gsfs s = { buf, 0 };
            int first = 1;
            if (b != 0)
                for (int q = 0; q < bb[b].npred; q++) {
                    int p = bb[b].pred[q];
                    if (!done[p])
                        continue;             /* optimistic: not seen yet */
                    if (first) {
                        memcpy(s.f, out[p].f, (size_t)out[p].n * sizeof *s.f);
                        s.n = out[p].n;
                        first = 0;
                        continue;
                    }
                    int j = 0;
                    for (int k = 0; k < s.n; k++) {
                        int h = gsf_find(&out[p], s.f[k].g, s.f[k].off,
                                         s.f[k].size);
                        if (h >= 0 && out[p].f[h].val == s.f[k].val)
                            s.f[j++] = s.f[k];
                    }
                    s.n = j;
                }
            lc.stamp++;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                gsf_step(fn, &d, gb, go, gk, &lc, &s, &fn->ins[n], 0);
                gsf_def(fn, &d, &lc, go, gk, &fn->ins[n]);
            }
            int same = done[b] && out[b].n == s.n;
            for (int k = 0; same && k < s.n; k++)
                same = out[b].f[k].g == s.f[k].g && out[b].f[k].off == s.f[k].off &&
                       out[b].f[k].size == s.f[k].size &&
                       out[b].f[k].val == s.f[k].val;
            if (!same) {
                memcpy(out[b].f, s.f, (size_t)s.n * sizeof *s.f);
                out[b].n = s.n;
                done[b] = 1;
                again = 1;
            }
        }
    }
    /* the rewrite, from each block's entry facts */
    for (int oi = 0; oi < norder; oi++) {
        int b = order[oi];
        struct gsfs s = { buf, 0 };
        int first = 1;
        if (b != 0)
            for (int q = 0; q < bb[b].npred; q++) {
                int p = bb[b].pred[q];
                if (!done[p]) { s.n = 0; first = 0; break; }
                if (first) {
                    memcpy(s.f, out[p].f, (size_t)out[p].n * sizeof *s.f);
                    s.n = out[p].n;
                    first = 0;
                    continue;
                }
                int j = 0;
                for (int k = 0; k < s.n; k++) {
                    int h = gsf_find(&out[p], s.f[k].g, s.f[k].off, s.f[k].size);
                    if (h >= 0 && out[p].f[h].val == s.f[k].val)
                        s.f[j++] = s.f[k];
                }
                s.n = j;
            }
        lc.stamp++;
        for (int n = bb[b].start; n < bb[b].end; n++) {
            changed |= gsf_step(fn, &d, gb, go, gk, &lc, &s, &fn->ins[n], 1);
            gsf_def(fn, &d, &lc, go, gk, &fn->ins[n]);
        }
    }
    for (int b = 0; b < nbb; b++)
        free(out[b].f);
    free(out); free(done); free(buf); free(order); free(l2b);
    free(lc.off); free(lc.st);
    free_cfg(bb, nbb);
    free(gb); free(go); free(gk); free(any); free_defs(&d);
    return changed;
}
