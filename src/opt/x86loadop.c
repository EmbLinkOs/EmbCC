/* ---- x86-64: a load moved down to the operation it feeds ---------------
 *
 * x86 can take one operand of an add, sub, and, or or xor straight from
 * memory -- `xor (%r10,%rsi,4), %eax` is the table step of every CRC --
 * but only when the load sits next to its use, and only into a register
 * that already holds the other operand. irgen puts the load where the
 * source reads it, often several instructions earlier, and the register
 * allocator has decided everything by the time codegen could notice.
 *
 * So, before allocation: a load whose value is read once, by such an
 * operation later in the same block, is moved down to just before it,
 * together with the address add only it uses -- when nothing between can
 * write memory (it would change what the load reads) or redefine the
 * address. A commutative operation gets the loaded value as its SECOND
 * operand, which is the one x86 takes from memory, and the allocator's
 * two-address bias then puts the result in the other operand's register.
 * Codegen fuses what it finds adjacent. */

#include "opt_int.h"

struct lop_cnt { int *n; int nv; };
static void lop_cnt_cb(int *p, void *ctx)
{
    struct lop_cnt *c = ctx;
    if (*p >= 0 && *p < c->nv)
        c->n[*p]++;
}
struct lop_find { int v, hit; };
static void lop_find_cb(int *p, void *ctx)
{
    struct lop_find *f = ctx;
    if (*p == f->v)
        f->hit = 1;
}

static int lop_user_ok(const struct ir_ins *u, int v, int size)
{
    if (u->flt || u->imm_b || u->w != size || u->a == u->b)
        return 0;
    switch (u->op) {
    case IR_ADD: case IR_AND: case IR_OR: case IR_XOR:
        return u->a == v || u->b == v;
    case IR_SUB:
        return u->b == v;          /* a - [mem]; [mem] - b gains nothing */
    default:
        return 0;
    }
}

/* A compare takes its memory operand second (`cmp reg, [mem]`), or first
 * against an immediate (`cmp [mem], imm`). */
static int lop_cmp_ok(const struct ir_ins *u, int v, int size)
{
    if (u->op != IR_CMP || u->flt || u->w != size || u->a == u->b)
        return 0;
    return u->imm_b ? u->a == v : u->a == v || u->b == v;
}

int pass_x86_loadop(struct ir_func *fn)
{
    int nv = fn->nvregs, changed = 0;
    if (target_get() != TARGET_X86_64 || fn->nins < 2 || !nv)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *nuse = xcalloc((size_t)nv, sizeof *nuse);
    struct lop_cnt c = { nuse, nv };
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op != IR_LDVAR && fn->ins[n].op != IR_ADDR)
            each_read(&fn->ins[n], lop_cnt_cb, &c);
    for (int p = 0; p < fn->nins; p++) {
        struct ir_ins *L = &fn->ins[p];
        if (L->op != IR_LOAD || L->vol || L->flt || L->memoff ||
            L->dst < fn->nvars || L->dst >= nv || nuse[L->dst] != 1 ||
            d.cnt[L->dst] != 1 || (L->size != 4 && L->size != 8) ||
            L->w != L->size)
            continue;
        int a0 = p;                    /* first instruction that moves */
        if (p > 0) {
            const struct ir_ins *ad = &fn->ins[p - 1];
            if (ad->op == IR_ADD && !ad->flt && ad->dst == L->a &&
                L->a >= fn->nvars && L->a < nv && nuse[L->a] == 1 &&
                d.cnt[L->a] == 1) {
                a0 = p - 1;
                /* ...and the shift scaling its index, which codegen folds
                 * into the address only when it is adjacent too */
                const struct ir_ins *sh = p > 1 ? &fn->ins[p - 2] : NULL;
                if (sh && sh->op == IR_SHL && sh->imm_b && !ad->imm_b &&
                    sh->dst == ad->b && sh->dst >= fn->nvars &&
                    sh->dst < nv && nuse[sh->dst] == 1 &&
                    d.cnt[sh->dst] == 1)
                    a0 = p - 2;
            }
        }
        int q = -1;
        for (int k = p + 1; k < fn->nins; k++) {
            const struct ir_ins *x = &fn->ins[k];
            enum ir_op op = x->op;
            struct lop_find f = { L->dst, 0 };
            if (op != IR_LDVAR && op != IR_ADDR)
                each_read((struct ir_ins *)x, lop_find_cb, &f);
            if (f.hit) {
                if (lop_user_ok(x, L->dst, L->size) ||
                    lop_cmp_ok(x, L->dst, L->size))
                    q = k;
                break;
            }
            if (op == IR_LABEL || op == IR_JMP || op == IR_BRZ ||
                op == IR_BRNZ || op == IR_RET || op == IR_UD2 ||
                op == IR_IGOTO || op == IR_SWITCH || writes_memory(op))
                break;
            int t = def_target(x), clash = t >= 0 && t == L->a;
            for (int m = a0; m < p && t >= 0 && !clash; m++)
                clash = t == fn->ins[m].a ||
                        (!fn->ins[m].imm_b && t == fn->ins[m].b);
            if (clash)
                break;
        }
        if (q < 0)
            continue;
        struct ir_ins *U = &fn->ins[q];
        if (U->a == L->dst && !U->imm_b) {   /* memory goes second */
            int t = U->a; U->a = U->b; U->b = t;
            if (U->op == IR_CMP)
                U->pred = swap_pred(U->pred);
        }
        if (q > p + 1) {
            int nm = p - a0 + 1, N = fn->nins;
            struct ir_ins mv[3];
            for (int k = 0; k < nm; k++)
                mv[k] = fn->ins[a0 + k];
            memmove(&fn->ins[a0], &fn->ins[p + 1],
                    (size_t)(q - p - 1) * sizeof *fn->ins);
            for (int k = 0; k < nm; k++)
                fn->ins[q - nm + k] = mv[k];
            if (fn->var_scope_lo) {
                int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
                for (int k = 0; k <= N; k++)
                    newpos[k] = k < a0 || k >= q ? k
                              : k <= p ? q - 1 - (p - k)
                              : k - nm;
                remap_scopes(fn, newpos, N);
                free(newpos);
            }
        }
        changed = 1;
        p = q;
    }
    free(nuse);
    free_defs(&d);
    return changed;
}
