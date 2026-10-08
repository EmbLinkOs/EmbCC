/* The Thumb backend's internal interface: what src/arch/thumb/codegen.c --
 * the ARMv7-M and ARMv8-M instruction selection, and the AAPCS32, frame and
 * register-allocator code every level shares -- exports to v6m.c, the
 * ARMv6-M instruction selection (docs/internals/armv6m-plan.md).
 *
 * Not a public header: nothing outside src/arch/thumb includes it.
 */
#ifndef EMBCC_ARCH_THUMB_CG_H
#define EMBCC_ARCH_THUMB_CG_H

#include "emit.h"
#include "../backend.h"
#include "../regalloc.h"

struct v6_lit;
struct v6_lsite;

struct t_sites {
    struct { int patch_off; struct func *target; int tail; } *call;
    int ncall, capcall;
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};


/* Per-function state. A struct rather than a pile of globals so that the
 * one thing a reader has to know about a function's lowering — where its
 * slots are and what is still unpatched — is in one place. */
struct t_fn {
    struct ir_func *fn;
    /* Comparison/branch fusion: how many times each vreg is READ, so a
     * comparison whose only reader is the branch after it can become
     * one `cmp` and one conditional branch instead of materialising 0
     * or 1 and testing that. `skip_next` tells the dispatch loop the
     * branch has already been emitted. The x86-64 backend has done
     * this from the start (usecnt there); ARMv7-M paid seven
     * instructions for every `if` without it. */
    int *usecnt;
    /* Per vreg: 1 for a constant whose one reader is an IT-block select
     * that moves it as an immediate (t_select_imms); IR_CONST emits
     * nothing for it. */
    char *selimm;
    long *selimm_v;
    int skip_next;
    /* A `tst` already made for the branch at instruction tst_br - 1
     * (0: none), with copies between the two: the branch only jumps. */
    int tst_br;
    int want_debug;
    /* Per vreg: 1 when it holds a 64-bit integer, which on a 32-bit
     * machine is an eight-byte slot and a REGISTER PAIR. Built from the
     * width of each value's DEFINING instruction, which is not the same
     * as i->w everywhere -- a compare of two 64-bit values has w == 8
     * and produces a one-or-zero that is four bytes wide. */
    char *wide;
    char *nshr;          /* per vreg: a narrow high-word shift
                          * (ra_narrow_hishift) */
    struct code *t;
    struct t_sites *st;
    long *slot;          /* per-vreg byte offset from sp, -1 for none */
    long frame;          /* total bytes sp moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    /* A variadic function's REGISTER SAVE AREA: where the prologue
     * spilled r0-r3 so that one pointer walks from them into the
     * caller's stack arguments. -1 when the function is not variadic. */
    long va_regsave;
    long entry_off;          /* the stack pointer at entry, from the frame
                              * base: __builtin_frame_address(0), with the
                              * pushed lr just below it */
    long va_first;       /* ... and the offset of the first UNNAMED one */
    int *label_off;      /* per label id, or -1 while unseen */
    /* cond >= T_CBZ is a cbz (T_CBZ) or cbnz (T_CBZ + 1). cz_at is where
     * a zero test's `cmp` began when it could become one (r0-r7), else -1:
     * what the first pass measures cbz's reach from. */
    struct { int at; int label; int cond; int sz; int cz_at; int ins; } *fix;
    /* Per IR instruction: a switch whose `tbh` table could not reach a
     * case on the first pass, and so takes the word table. */
    char *no_tbh;
    /* A function with a conditional branch past B<c>.W's +-1 MB: every
     * conditional jump to a label is then `b<!c> .+n; b label`, and none
     * is merged with the jump after it -- so the passes still make the
     * same branches in the same order. */
    int far_mode;
    /* Branch relaxation: per branch, in emission order, whether the
     * first pass found its target within the 16-bit form's reach. NULL
     * on the first pass, which emits every branch 32-bit. */
    const char *shortb;
    int nshortb;
    int nfix, capfix;
    /* Per vreg, when its one definition puts a frame address in it:
     * fvar the local whose `addr` it is, or fscr the call-scratch offset
     * a struct-returning call answered with; else both -1. Such a value
     * is frame base + a constant: when it has no register it is
     * recomputed where it is read and addressed through directly,
     * never stored to a slot and loaded back. */
    int *fvar;
    long *fscr;
    /* Per vreg: the register the allocator gave it, or -1 for one that
     * stays in memory. NULL when it did not run (-O0/-O1). */
    int *loc;
    /* Per vreg: the S register (s16-s31) a single-precision float lives
     * in, or -1. NULL without an FPU or without the allocator. A value
     * has at most one of loc and floc, and one with an floc has NO slot:
     * rd/wr/vfp_load/vfp_store cross to it with vmov, and every other
     * reader of a slot goes through slot_of(), which refuses it. */
    int *floc;
    /* The register every frame slot is addressed from: sp, except in a
     * function with a variable-length array, where sp moves at run time
     * and r7 -- the Thumb frame pointer -- holds the frame base. */
    int fb;
    long out_bytes;      /* the outgoing-argument area, at the live sp */
    int nfsave;          /* s16.. that the prologue vpushes (even) */
    /* The last conditional branch: where its code ended and its fixup,
     * or bc_end -1 once a label has been placed since. See
     * invert_last_bcond. */
    int bc_end, bc_fix;
    /* The last compare of a register with an immediate that went
     * straight into a branch: where that branch's code ended, the
     * register and the immediate. A compare of the same two, with
     * nothing emitted since and no label placed, has those flags
     * already -- a switch's decision tree asks `== k` and then `> k` of
     * one value. fl_end is -1 when there is none. */
    int fl_end, fl_reg;
    long fl_imm;
    /* The last store of a register to a frame slot (wr): where its code
     * ended, the register, the slot's offset and the base. A read of the
     * same slot with nothing emitted since and no label placed is the
     * register already (rd). ls_end is -1 when there is none. */
    int ls_end, ls_reg, ls_fb;
    long ls_off;
    /* Which of the scratch registers r9-r11 the prologue saves: all of
     * them until a pass has shown which the body uses. */
    unsigned scr_save;
    /* A leaf that saves nothing at all: no push, no pop, `bx lr`. Decided
     * per pass, once scr_save is known (see the pass loop). */
    int leaf, nopush;
    /* A function that cannot return -- no IR_RET, no tail call: an RTOS
     * task's for (;;), a scheduler's start, a reset handler. No caller
     * is ever resumed, so nothing it would restore is saved: no push, no
     * vpush, no epilogue -- only the frame. Not under -g, where a
     * debugger's backtrace reads the saved lr, and not for a variadic
     * function, whose register save area is a push. */
    int noret;
    /* Per instruction: an IR_CALL made as a TAIL call (t_tail_ok). NULL
     * when there are none. */
    char *tail;
    int used_callee[RA_MAXPOOL];
    int nsave;           /* how many of those it took */
    long save_at;        /* where the prologue spilled them */
    /* LOW SCRATCH (lo_free): which of r0-r7 the instruction being
     * emitted may use in place of a high scratch -- bit r for rr, 0 when
     * none may -- and, per instruction, the registers holding a value
     * live into or out of it, which it is computed from (lo_busy_map). */
    unsigned lofree;
    unsigned *lv_busy;

    /* ---- the 64-bit constant pool (t_lit64 in codegen.c) --------------- */
    /* Per instruction: 1 for an IR_CONST that loads from the pool, 0 for
     * one built with movw/movt -- not a candidate, or out of the pool's
     * reach on a first pass (then the pass is made again without it).
     * NULL when the function has no candidate. */
    char *lp_use;
    unsigned long long *lp_val;   /* this pass's pool, in order */
    int lp_n, lp_cap;
    /* Once a first pass found a load out of reach: the pool's order,
     * fixed (t_lit64_fit), which later passes keep. */
    int lp_planned;
    struct t_lsite { int at, ins, idx, rt, rt2; } *lp_site;
    int lp_nsite, lp_capsite;

    /* ---- ARMv6-M only (v6m.c); zero at the other levels ---------------- */
    /* The literal pool being collected, and the LDRs waiting for it. */
    struct v6_lit *lit;
    int nlit, caplit;
    struct v6_lsite *lsite;
    int nlsite, caplsite;
    /* Per &&label this pass, in order: its label and the `add rD, pc`
     * its pool word is relative to (lrel[2k], lrel[2k + 1]). */
    int *lrel;
    int nlrel, caplrel;
    /* Per pool point (pool_point in v6m.c, in emission order): an island
     * there -- 1 with a branch around it, 2 where the code before ends in
     * an unconditional transfer. The first pass of each layout decides;
     * later passes replay. */
    char *isl;
    int isl_replay;
    int npoint, capisl;  /* the pool points so far this pass (pool_point) */
    int barrier;         /* the code so far ends in an unconditional jump */
    int spb;             /* bytes pushed below the frame for temporaries */
    unsigned tbusy;      /* scratch registers this instruction is holding */
    unsigned ins_mask;   /* the registers this instruction's operands live in */
    int ntmp, tmp_reg[6], tmp_pushed[6];
    /* Where this pass put an alignment pad (a pool, a table): a later pass
     * may grow each by two bytes, which branch measurement allows for. */
    int *pads;
    int npads, cappads;
    unsigned v6_used;    /* which of r6/r7 (the low scratch) a pass used */
};

struct argplace { int reg, nreg, nstk; long stk; int vfp, nvfp, vdbl; };
struct abi_walk { int ncrn; long stk; unsigned vfree; int vfp; };

/* ---- exported by codegen.c --------------------------------------------- */

char *tcg_wide64_map(struct ir_func *fn);
void tcg_frame_addr_map(struct t_fn *F);
void tcg_layout(struct t_fn *F);
void tcg_walk_init(struct abi_walk *w, int sret, int varargs, int pcs);
void tcg_place_one(struct abi_walk *w, const struct ir_arg *a,
                   struct argplace *p);
int tcg_call_sret_bytes(const struct ir_ins *i);
int tcg_fn_sret_bytes(const struct ir_func *fn);
long tcg_slot_of(const struct t_fn *F, int v);
int tcg_faddr(const struct t_fn *F, int v, long *off);
/* The exclusion map for the allocator with a frame address read once
 * added (codegen.c's t_faddr_excl); NULL for `base` as it is. */
char *tcg_faddr_excl(const struct t_fn *F, const char *base);
void tcg_want_label(struct t_fn *F, int at, int label, int cond);
void tcg_note_call(struct t_sites *st, int at, struct func *target);
void tcg_note_ext(struct t_sites *st, int at, struct func *callee);
void tcg_note_str(struct t_sites *st, int at, int idx, enum reloc_kind k);
void tcg_note_glob(struct t_sites *st, int at, struct global *g,
                   enum reloc_kind k);
void tcg_note_fn(struct t_sites *st, int at, struct func *target,
                 enum reloc_kind k);
void tcg_call_helper(struct t_fn *F, const char *name);
const char *tcg_fp_binop_name(enum ir_op op, int w);
const char *tcg_fp_cmp_name(enum binop pred, int w);
int tcg_cond_for(enum binop pred, int sign);
/* The register allocator as the ARMv7-M lowering runs it: the pair pass
 * (NULL when it does not run) and the integer pass. */
int *tcg_pair_alloc(struct ir_func *fn, const char *wide, const char *excl,
                    int *used, int *nused);
const struct ra_target *tcg_ra(void);
int tcg_regalloc(void);
/* r6/r7 in the ARMv6-M pool too, this attempt (gen_func_best), and how
 * v6m.c says an instruction found no scratch register free: the attempt
 * is thrown away. */
int tcg_ext(void);
void tcg_role_fail(void);
/* Per instruction, the registers holding a value live into or out of it
 * (lo_busy_map), and those over n..n+span with its operands (t_busy); and
 * whether the instruction's lowering names no low register of its own
 * (lo_op_ok). */
unsigned *tcg_lo_busy_map(const struct t_fn *F);
unsigned tcg_busy(const struct t_fn *F, int n, int span);
int tcg_lo_op_ok(const struct t_fn *F, const struct ir_ins *i);
/* -O0: the allocator runs for the temporaries, every source variable kept
 * in its slot (codegen.c's g_t_o0). */
int tcg_o0(void);
int tcg_pairs(void);
void tcg_reset_taken(void);

/* CMSE (-mcmse), for both lowerings: the refusals, and how many core
 * registers an entry function's result occupies. */
void tcg_cmse_fail(const struct ir_func *fn, int line, const char *what);
int tcg_cmse_ret_regs(const struct ir_func *fn);
void tcg_cmse_check_entry(const struct ir_func *fn);
void tcg_cmse_check_call(const struct ir_func *fn, const struct ir_ins *i,
                         const struct abi_walk *w, long sret);

/* ---- exported by v6m.c ------------------------------------------------- */

/* One function, ARMv6-M: the counterpart of codegen.c's gen_func. */
void v6_gen_func(struct ir_func *fn, struct code *t, struct t_sites *st,
                 int want_debug);
/* Does this instruction become a call on ARMv6-M where it is not one on
 * ARMv7-M (a divide, a 64-bit multiply, a block copy, an atomic)? */
int v6_op_calls_helper(const struct ir_ins *i);

#endif
