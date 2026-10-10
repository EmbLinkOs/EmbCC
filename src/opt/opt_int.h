/* opt_int.h: what the optimizer's files share -- the IR walkers, the
 * CFG and dominators, alias analysis, the instruction buffer, and the
 * passes the pass manager (opt.c) runs. Private to src/opt; the rest of
 * the compiler includes opt.h. */
#ifndef EMBCC_OPT_OPT_INT_H
#define EMBCC_OPT_OPT_INT_H

#include "opt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "../arch/target.h"
#include "../arch/regalloc.h"

/* The width of a POINTER as an IR operation's `w`. Strength reduction
 * rewrites an indexed access into a walking pointer and increments it
 * every iteration; those increments are pointer arithmetic, and saying 8
 * made them 64-bit operations on a machine with 32-bit registers. The
 * other 8s in this file are 16-byte vector lanes and mean something
 * else. */
#define PTRW (target_ptr_size())
#include "../driver/remark.h"
#include "../driver/util.h"
#include "../sema/ldfloat.h"

/* What the quiet passes did, counted per function (opt.c). */
struct opt_did { long lvn, gcse, dce, copy, loadcse, dse, divmagic,
                ifconv, cfgclean, tailrec, idiom, sroa, pre, latch; };
extern struct opt_did g_did;

/* Whether opt_run was asked for size. A global in the style of g_pass: the
 * inliner's decision is three call levels below opt_run. */
extern int g_opt_size;

struct retarget { int from, to; };

/* Counting uses is the one visitor that WRITES through an operand
 * index, so it is the one that turns a bad index into corruption rather
 * than a wrong answer. It carries the array's length and checks.
 * each_read's contract is that every index it reports is a real vreg;
 * this is the backstop for when that stops being true, which it did. */
struct ucount { int *use, n; };

/* The vregs an instruction reads AS VALUES. LDVAR and ADDR are left out
 * on purpose: their `a` is a frame slot (see each_read), which is not a
 * value operand at all -- no instruction computes it, so asking where it
 * was defined is the wrong question. `over` marks an instruction with
 * more operands than fit, and every caller treats that as "do not
 * reason about this one". */
struct opnds { int v[4]; int n, over; };

struct defs {
    int *cnt;    /* number of definitions of each vreg */
    int *ins;    /* index of the sole defining instruction when cnt == 1 */
    /* Every definition of each vreg, when defs_lists has built them:
     * first[v] is the first instruction defining v and next[n] the one
     * after instruction n, -1 at the end. NULL until asked for. */
    int *first, *next;
};

struct lkconst { int *gen_of; long *val; int *w; int gen, nv; };

/* One value-number entry: the discriminating fields of a computation plus the
 * temp that first produced it. Two instructions with equal keys in the same
 * block compute the same value. Loads carry `memver`, which a write that
 * names no bytes (a call, asm, a fence, any volatile store) bumps so that
 * every load before it has a different key; a plain store instead removes
 * the entries it may overlap (lvn_mem_kill). */
struct vn {
    enum ir_op op;
    int a, b, w, sign, size;
    int flt;                   /* a float or double operation */
    enum binop pred;
    long imm;
    void *ptr;                 /* GADDR glob / FADDR callee */
    int label;                 /* STRADDR string index */
    int memver;                /* LDVAR / LOAD only */
    /* An operand that is a LITERAL, keyed by its value rather than by
     * the temp holding it (gcse_key_consts). `has` distinguishes "the
     * constant -1" from "no operand", both of which leave a/b at -1. */
    long ca, cb;
    char has_ca, has_cb;
    int result;                /* the temp holding this value */
};

/* The value table, hashed.
 *
 * Both value-numbering passes looked an instruction's key up by walking
 * every entry, and LVN also forgot what a definition replaced by walking
 * every entry again: on one block of 8000 plain statements that was
 * O(n^2) twice over, and vn_eq and vn_kill were the second and third
 * hottest functions of the -O2 compile. Now a key is found through its
 * hash, and the entries that name a vreg (as an operand or as the result)
 * are filed under it, so a definition reaches exactly the entries it
 * makes stale.
 *
 * What makes this give the same answers is that a key is in the table
 * at most once: an entry is only ever added after a lookup of its key
 * found nothing. So "the first matching entry", which is what the walk
 * returned, is "the matching entry", and the order of the entries --
 * which the hash does not keep -- never mattered. */
struct vnent {
    struct vn k;
    unsigned h;                /* its bucket */
    int prev, next;            /* the bucket's chain; -1 ends it */
    char live;
};
struct vnref { int ent, next; };
struct vntab {
    struct vnent *e;
    int ne, cape;
    int *head;
    unsigned mask;
    /* LVN only (byv != NULL): per vreg, the entries naming it, for
     * vn_kill; and the loads keyed at the current memory version, the
     * only entries a store can make stale (lvn_mem_kill). */
    struct vnref *r;
    int nr, capr;
    int *byv, nv;
    int *ld;
    int nld, capld;
};

struct bb {
    int start, end;                 /* instruction range [start, end) */
    /* Successors. Two would do for every branch this IR has -- except
     * the indirect one: `goto *p` reaches any label whose address was
     * taken, which is as many as the function has. */
    int *succ, nsucc, capsucc;
    int *pred, npred;
    int idom, rpo;                  /* immediate dominator; reverse-postorder # */
    int dpre, dpost;                /* dominator-tree DFS numbers (bb_dominates);
                                     * -1 off the tree */
    int *phi_local, *phi_res, nphi; /* phi(local) -> result temp, per block */
    int **phi_inc;                  /* phi_inc[p][k] = value on edge from pred p */
};

/* Growable instruction buffer, for rebuilding fn->ins out of SSA. */
struct ibuf { struct ir_ins *p; int n, cap; };

enum mem_kind { MEM_UNKNOWN, MEM_GLOBAL, MEM_SLOT };

struct memref {
    enum mem_kind kind;
    int id;                  /* MEM_GLOBAL: the symbol; MEM_SLOT: the var */
};

enum { BASE_NONE, BASE_VREG, BASE_GLOBAL, BASE_SLOT };

struct maccess {
    struct memref obj;       /* the object, for may_alias */
    int bkind, bid;          /* the base value; BASE_NONE: not known */
    unsigned long off;       /* bytes past the base, modulo addr_mask */
    int size;                /* bytes accessed; 0: not known */
};

struct lcopy { const int *cp; int nv, n; };

/* One loop's worth of what the recognizer found. */
struct vecloop {
    int iv;            /* the induction variable's phi temp */
    int step_ins;      /* `next = iv + 1` */
    int copy_ins;      /* `iv = mov next` */
    int cmp_ins;       /* `c = cmp lt iv, #bound` */
    long bound;        /* the trip count, when bound_reg < 0 */
    int bound_reg;     /* or the temp holding it, for a runtime count */
    int esize, vf;
    /* A sum reduction (`s += a[i]`), or red_acc == -1. The accumulator
     * becomes VF partial sums, folded to one after the loop. */
    int red_acc;       /* the accumulator's phi temp */
    int red_next;      /* what the loop body adds into it */
    int red_add;       /* `next = acc + <a vector>` */
    int red_copy;      /* `acc = mov next`, the phi copy at the latch */
    int red_vec;       /* the vector operand being summed */
    int red_ext;       /* a widening sum: the IR_EXT between load and add,
                        * and then red_vec is the NARROW vector it reads */
    int lo, hi;        /* the loop's instruction range, [lo, hi) */
    int header;        /* its first block */
};

#define VDBG(...) do { if (getenv("EMBCC_VECDEBUG")) \
        fprintf(stderr, "vec: " __VA_ARGS__); } while (0)

/* Vreg remap over one instruction. kind 0 = caller shift (a temp >= p1 moves up
 * by p2); kind 1 = callee map (a callee local < p3 -> p1+x, a temp -> p2+x). */
struct rmp { int kind, p1, p2, p3, lbase; };

/* util.c */
int writes_temp(enum ir_op op);
int is_pure(enum ir_op op);
int def_target(const struct ir_ins *i);
void each_label(struct ir_func *fn, struct ir_ins *i,
                void (*cb)(int *, void *), void *ctx);
void retarget_cb(int *p, void *ctx);
void each_read(struct ir_ins *i, void (*cb)(int *, void *), void *ctx);
void opnd_cb(int *p, void *ctx);
void value_opnds(struct ir_ins *i, struct opnds *o);
int ins_reads(struct ir_ins *i, int v);
void ins_own_args(struct ir_ins *q);
void compute_defs(struct ir_func *fn, struct defs *d);
void free_defs(struct defs *d);
void defs_lists(struct ir_func *fn, struct defs *d);
int const_b(struct ir_func *fn, struct defs *d, struct ir_ins *i,
            long *out);
int get_const(struct ir_func *fn, struct defs *d, int v, long *out);
/* The value an instruction makes, as a copy of it must carry it: its `w`
 * and `flt`, except a comparison's, whose `w` and `flt` describe its
 * OPERANDS -- a compare of two doubles makes a four-byte integer 0 or 1. */
int ir_result_w(const struct ir_ins *i);
int ir_result_flt(const struct ir_ins *i);
void ins_blank(struct ir_ins *i);
struct ir_ins *ib_push(struct ibuf *b);

/* fold.c */
long norm(long r, int w);
void to_const(struct ir_ins *i, long val);
void to_mov(struct ir_ins *i, int src);
int fold_bin(enum ir_op op, long A, long B, int w, int sign,
             enum binop pred, long *out);
long fold_ext(long A, int size, int sign, int w);
void lk_note(struct lkconst *k, const struct ir_ins *i);
int lk_get(const struct lkconst *k, int v, int w, long *out);
extern struct ir_unit *g_fold_unit;
int pass_fold(struct ir_func *fn);

/* lvn.c */
int writes_memory(enum ir_op op);
int vn_eq(const struct vn *x, const struct vn *y);
int vn_key(struct ir_ins *i, int memver, struct vn *k);
void vn_to_mov(struct ir_ins *i, int src);
int vn_stable(struct ir_func *fn, const struct defs *d, struct ir_ins *i);
void vntab_init(struct vntab *t, int nins, int nv, int index);
void vntab_free(struct vntab *t);
int vntab_find(const struct vntab *t, const struct vn *k);
void vntab_add(struct vntab *t, const struct vn *k);
void vntab_unlink(struct vntab *t, int x);
int pass_lvn(struct ir_func *fn);

/* copyprop.c */
int pass_copyprop(struct ir_func *fn);
void lcopy_cb(int *p, void *ctx);
int pass_copyprop_local(struct ir_func *fn);

/* dce.c */
void count_cb(int *p, void *ctx);
int pass_dce(struct ir_func *fn);

/* mem2reg.c */
int m2r_w(int size);
const struct ir_dbgvar *local_var(const struct ir_func *fn, int L);
int pass_mem2reg(struct ir_func *fn);

/* reassoc.c */
int as_op_const(struct ir_func *fn, struct defs *d, struct ir_ins *i,
                int *x, long *c);
int pass_reassoc(struct ir_func *fn);
int pass_idxoff(struct ir_func *fn);

/* cfg.c */
void free_cfg(struct bb *bb, int nbb);
struct bb *build_cfg(struct ir_func *fn, int *nbb_out, int **l2b_out);
void compute_rpo(struct bb *bb, int nbb, int *order, int *norder);
void compute_idom(struct bb *bb, int *order, int norder);
void compute_df(struct bb *bb, int nbb, int **df, int *ndf);
int bb_pred_index(const struct bb *b, int p);
int bb_dominates(struct bb *bb, int s, int b);
void loop_body(struct bb *bb, int nbb, int h, int tail, char *in);

/* gcse.c */
int gcse_numberable(enum ir_op op);
void gcse_key_consts(struct ir_func *fn, struct defs *d, struct vn *k);
int pass_gcse(struct ir_func *fn);

/* alias.c */
int one_value(const struct ir_func *fn, const struct defs *d, int v);
struct maccess mem_access(struct ir_func *fn, struct defs *d,
                          int addr, int size);
struct maccess slot_access(int var);
struct maccess obj_access(struct ir_func *fn, struct defs *d, int addr);
int same_base(struct maccess a, struct maccess b);
int acc_overlap(struct maccess a, struct maccess b, const char *taken,
                int nvars);
int acc_covers(struct maccess later, struct maccess early);
char *slots_taken(struct ir_func *fn);
int lvn_mem_kill(struct ir_func *fn, struct defs *d, const char *taken,
                 struct vntab *tb, const struct ir_ins *ins);

/* divmagic.c */
int pass_divmagic(struct ir_func *fn);
int pass_mulwiden(struct ir_func *fn);
int pass_divtest(struct ir_func *fn);
int pass_divmod(struct ir_func *fn);

/* ifconv.c */
int target_cheap_select(void);
int pass_ifconv(struct ir_func *fn);

/* cfgclean.c */
void remap_scopes(struct ir_func *fn, const int *newpos, int oldn);
int pass_thread(struct ir_func *fn);
int pass_cfgclean(struct ir_func *fn);

/* tailrec.c */
int pass_tailrec(struct ir_func *fn);

/* attrs.c */
void infer_attrs(struct ir_unit *iu);

/* dse.c */
int pass_dse(struct ir_func *fn);

/* loadcse.c */
int pass_loadcse(struct ir_func *fn);

/* licm.c */
extern int g_licm_mem;
int pass_licm(struct ir_func *fn);

/* pre.c */
int pass_pre(struct ir_func *fn);

/* rotate.c */
int pass_rotate(struct ir_func *fn);

/* guardjump.c */
int gj_same_fields(const struct ir_ins *g, const struct ir_ins *c);
int gj_in_loop(const struct ir_func *fn, int n, const int *labpos);
int pass_guardjump(struct ir_func *fn);

/* tailmerge.c */
void tm_sweep(struct ir_func *fn);
int pass_tailmerge(struct ir_func *fn);
int pass_retdup(struct ir_func *fn);

/* vectorize.c */
int addr_of_iv(struct ir_func *fn, struct defs *d, struct vecloop *L,
               int p, int scale);
int pass_vectorize(struct ir_func *fn);

/* idiom.c */
int pass_idiom(struct ir_func *fn);

/* ivsr.c */
int pass_ivsr(struct ir_func *fn);

/* unroll.c */
int pass_unroll(struct ir_func *fn);

/* swthread.c */
int pass_swthread(struct ir_func *fn);

/* sccp.c */
int pass_sccp(struct ir_func *fn);
int drop_unreachable(struct ir_func *fn);

/* memfwd.c */
int pass_storefwd(struct ir_func *fn);
extern int g_nro;
void ro_globals(struct ir_unit *iu);
int pass_roload(struct ir_func *fn);
int pass_punfwd(struct ir_func *fn);

/* immfold.c */
int pass_sinkconst(struct ir_func *fn);
enum binop swap_pred(enum binop p);
int pass_signtest(struct ir_func *fn);
int pass_storenarrow(struct ir_func *fn);
int pass_immfold(struct ir_func *fn);

/* inline.c */
void remap_ins(struct ir_ins *in, struct rmp *r);
extern int g_inline_o1;
extern int g_inline_always_only;
void inline_unit(struct ir_unit *iu);

/* sroa.c */
int pass_cxlocal(struct ir_func *fn);
int pass_aggcopy(struct ir_func *fn);
int pass_sroa(struct ir_func *fn, int report_refusals);

/* verify.c */
void verify_func(struct ir_func *fn, const char *tag);

/* splitloops.c */
int pass_splitloops(struct ir_func *fn);

/* rangecheck.c */
int pass_rangecheck(struct ir_func *fn);

/* joincopies.c */
int pass_joincopies(struct ir_func *fn);

/* sink.c */
int pass_sinkupd(struct ir_func *fn);
int pass_sinkaddr(struct ir_func *fn);

/* latch.c */
int pass_latch_copies(struct ir_func *fn);
int pass_thread_copies(struct ir_func *fn);

/* x86loadop.c */
int pass_x86_loadop(struct ir_func *fn);

#endif
