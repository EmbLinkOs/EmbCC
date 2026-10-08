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
        else
            byte = (unsigned char)(((unsigned long)g->init >>
                                    (8 * (target_big_endian() ? gs - 1 - at
                                                              : at)))
                                   & 0xff);
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
 * Not on AVR, whose 64-bit shift is a byte loop and whose double is four
 * bytes anyway. */
struct pun_rec { int off, width, val, flt; };

int pass_punfwd(struct ir_func *fn)
{
    int nv = fn->nvregs, nvars = fn->nvars, changed = 0;
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
            int k = 0;
            for (int j = 0; j < nrec[L]; j++) {      /* drop what it overlaps */
                struct pun_rec *r = &rec[L][j];
                if (r->off + r->width <= off || off + wid <= r->off)
                    rec[L][k++] = *r;
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
    free(plan); free(pr);
    free(d.cnt); free(d.ins);
    return changed;
}
