/* The optimizer's pass manager. opt_run optimizes each function of a
 * unit: the block-local passes -- folding, value numbering, copy
 * propagation, dead code -- iterate to a fixpoint inside each round, and
 * from -O2 the global and loop passes run around them (opt_func). Each
 * pass is a file of its own in this directory (README.md lists them),
 * and what they share is declared in opt_int.h.
 *
 * The local passes are safe by EmbIR's single-assignment temporaries: a
 * value in a vreg with exactly one definition is invariant, so no
 * control-flow analysis is needed to know it is the same everywhere. The
 * global ones stand on the CFG and the dominator tree (cfg.c). */

#include "opt_int.h"

/* What the quiet passes did, counted per function.
 *
 * CSE, dead code, copy propagation and load elimination each run many
 * times inside the fixpoint, so a remark per rewrite would bury the
 * output. One line per function, at the end, answers the question
 * anyone actually asks -- "did anything happen, and what" -- and is
 * comparable between two builds. */
struct opt_did g_did;
int g_opt_size;

/* ---- driver ---- */

/* ---- the passes, by name -------------------------------------------
 *
 * One table so that -f<name>/-fno-<name>, the -O levels and --help all
 * read the same list, and a pass added without a name here is visibly
 * missing rather than silently unnameable. `forced` records that a flag
 * spoke, so the level does not overwrite what it asked for. */
struct passflag { const char *name; int on; int forced; };
static struct passflag g_pass[] = {
    { "mem2reg",   0, 0 },   /* promote locals to SSA before the fixpoint */
    { "gcse",      0, 0 },   /* dominator-scoped global CSE */
    { "load-cse",  0, 0 },   /* global redundant-load elimination */
    { "sccp",      0, 0 },   /* const-branch resolution + dead-block drop */
    { "licm",      0, 0 },   /* loop invariants, rotation, strength reduction */
    { "vectorize", 0, 0 },   /* lane-wise loops, where the backend has them */
    { "inline",    0, 0 },   /* splice a small callee into its caller */
    { "dse",       0, 0 },   /* drop a store a later one overwrites */
    { "div-magic", 0, 0 },   /* divide by a constant without dividing */
    { "if-convert",0, 0 },   /* a two-way choice without a branch */
    { "cfg-clean", 0, 0 },   /* thread jumps, drop unreachable code */
    { "tail-recursion", 0, 0 }, /* a self tail call becomes a loop */
    { "idiom",     0, 0 },   /* a copy or clear loop becomes memcpy/memzero */
    { "sroa",      0, 0 },   /* split a private aggregate into scalars */
    { "unroll",    0, 0 },   /* copies of a body, one test between them */
    { "pre",       0, 0 },   /* compute it on the path that lacked it */
    { "switch-thread", 0, 0 }, /* a known state jumps straight to its case */
    { "licm-mem",  0, 0 },   /* loads out of loops, a location in a register */
};
#define NPASS ((int)(sizeof g_pass / sizeof g_pass[0]))
#define P_MEM2REG 0
#define P_GCSE    1
#define P_LOADCSE 2
#define P_SCCP    3
#define P_LICM    4
#define P_VEC     5
#define P_INLINE  6
#define P_DSE     7
#define P_DIVMAGIC 8
#define P_IFCONV   9
#define P_CFGCLEAN 10
#define P_TAILREC  11
#define P_IDIOM    12
#define P_SROA     13
#define P_UNROLL   14
#define P_PRE      15
#define P_SWTHREAD 16
#define P_LICMMEM  17

int opt_set_pass(const char *name, int on)
{
    for (int i = 0; i < NPASS; i++)
        if (!strcmp(g_pass[i].name, name)) {
            g_pass[i].on = on;
            g_pass[i].forced = 1;
            return 1;
        }
    return 0;
}

int opt_pass_names(const char *const **names)
{
    static const char *n[NPASS];
    for (int i = 0; i < NPASS; i++)
        n[i] = g_pass[i].name;
    *names = n;
    return NPASS;
}

/* Set by the level, unless a flag already spoke for this pass. */
static void pass_default(int idx, int on)
{
    if (!g_pass[idx].forced)
        g_pass[idx].on = on;
}

#define g_mem2reg (g_pass[P_MEM2REG].on)
#define g_gcse    (g_pass[P_GCSE].on)
#define g_loadcse (g_pass[P_LOADCSE].on)
#define g_sccp    (g_pass[P_SCCP].on)
#define g_licm    (g_pass[P_LICM].on)
#define g_vec     (g_pass[P_VEC].on)
#define g_dse     (g_pass[P_DSE].on)
#define g_divmagic (g_pass[P_DIVMAGIC].on)
#define g_ifconv   (g_pass[P_IFCONV].on)
#define g_cfgclean (g_pass[P_CFGCLEAN].on)
#define g_tailrec  (g_pass[P_TAILREC].on)
#define g_idiom    (g_pass[P_IDIOM].on)
#define g_sroa     (g_pass[P_SROA].on)
#define g_unroll   (g_pass[P_UNROLL].on)
#define g_pre      (g_pass[P_PRE].on)
#define g_swthread (g_pass[P_SWTHREAD].on)

#define OPT_MAX_ROUNDS 64   /* rounds of the block-local passes per outer round */
static void opt_no_fixpoint(struct ir_func *fn, int rounds)
{
    if (getenv("EMBCC_VERIFY"))
        diag_fatal(fn->file, fn->line,
                   "internal: the optimizer did not converge on '%s' after %d "
                   "rounds -- two passes are undoing each other's work",
                   fn->name, rounds);
    fprintf(stderr, "embcc: warning: the optimizer stopped after %d rounds on "
                    "'%s' without converging (the code is correct; this is a "
                    "compiler performance bug worth reporting)\n",
            rounds, fn->name);
}

static void opt_func(struct ir_func *fn)
{
    /* ---- functions the CFG cannot be trusted for -------------------
     *
     * Two shapes leave this analysis short of the truth:
     *
     *   Computed goto. An indirect jump can reach any address-taken
     *   label, and build_cfg does not model that, so dominance and
     *   liveness are both wrong.
     *
     *   An exception region. A landing pad is entered from EVERY call
     *   in its region -- edges nothing here records -- so the pad looks
     *   unreachable and a value live only on the exception path looks
     *   dead.
     *
     * Both used to return outright, leaving the whole function at -O0
     * however the build was invoked. For exceptions that is most of a
     * C++ program: across lib/libcxx, 316 of 609 functions got no
     * optimization at all, including every one of the loop passes.
     *
     * They do not need to. The passes that reason about CONTROL FLOW
     * are the ones that cannot be trusted here; the ones that work
     * within a block -- folding, value numbering, copy propagation,
     * store forwarding, dead code -- reason only between labels, and a
     * missing edge cannot make them wrong. A landing pad starts with a
     * label, so it is its own block to all of them. So the function is
     * optimized LOCALLY rather than not at all. */
    /* Computed goto used to bail OUTRIGHT, and the reason was recorded
     * as a condition on when it could stop: "the exclusion stays until
     * the CFG can model the edges rather than until a pass is blamed".
     * build_cfg models them now -- an indirect jump gets an edge to
     * every label whose address is taken, which is the complete set of
     * places it can land, because `&&label` is the only thing that
     * produces such a value.
     *
     * What was actually going wrong is worth naming, because it was not
     * the local passes. Without those edges the blocks those labels
     * open have no predecessor, so they are UNREACHABLE -- and the two
     * passes that delete unreachable code deleted the program. With the
     * edges they are reachable, dominance is right, and the function is
     * optimized like any other. Codegen still keeps it in the memory
     * model (no register allocation), because a value live across an
     * indirect jump is a separate question and that is where it is
     * answered. */
    int cfg_ok = !fn->neh;
    /* What still has to stand aside is every pass that puts an
     * instruction ON AN EDGE. `goto *p` goes to the label and there is
     * no block between them to intercept, so an edge out of one cannot
     * be split -- and a preheader written just after the jump is not on
     * any path at all, which is exactly what happened: LICM hoisted
     * four instructions into one, and the pass that drops unreachable
     * code dropped it, uses and all.
     *
     * So: mem2reg (phi copies go on edges), the loop passes (a
     * preheader is an edge), if-conversion and unrolling (both open new
     * blocks). What is left -- folding, value numbering local and
     * global, copy propagation, store forwarding, load elimination,
     * dead stores, constant branches, dead code, SROA, magic division,
     * CFG cleanup -- reasons about the blocks that are there and needs
     * no new ones. That is most of the optimizer, where before it was
     * none of it.
     *
     * It is also the right answer for the code that uses this: a
     * threaded-code dispatcher keeps its state where every arm can
     * reach it, and tests/exec/computed-goto.c says so in its first
     * paragraph -- a value live across the jump stays in memory. */
    int has_igoto = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_IGOTO) { has_igoto = 1; break; }
    int edge_ok = cfg_ok && !has_igoto;   /* may a pass split an edge? */
    /* Likewise exception regions: a landing pad is entered from every call
     * of its region, edges the passes do not see (and its code, reached by
     * no jump, would look dead).
     *
     * Only while a region EXISTS, though. mark_eh_calls() clears neh when
     * no call in any region can actually throw -- true of every `noexcept`
     * function that calls nothing, and of type_info::name() in our own C++
     * runtime -- and then the pad really is ordinary unreachable code that
     * the passes may optimize or drop. What used to make that unsafe was
     * not the pad but the MODEL: IR_LANDING writes two temps and
     * compute_defs only saw one, so DCE deleted the landing and kept the
     * stores reading what it produced. compute_defs knows both now, so
     * these functions are optimized again instead of being compiled at
     * -O0 however the build was invoked. */
    int verify = getenv("EMBCC_VERIFY") != NULL;
    if (verify) verify_func(fn, "irgen");
    int ins_before = fn->nins;
    memset(&g_did, 0, sizeof g_did);
    /* Before the fixpoint: what it emits -- a multiply, some shifts and
     * an add -- is ordinary arithmetic the other passes then fold,
     * value-number and strength-reduce like any other. */
    /* Before everything: the loop it makes is then an ordinary loop to
     * mem2reg, LICM, rotation and strength reduction, which is most of
     * why it is worth doing at all. */
    if (g_tailrec && edge_ok)
        pass_tailrec(fn);
    if (g_divmagic) {
        pass_divtest(fn);               /* before divmagic takes the % */
        pass_divmagic(fn);
    }
    /* Before mem2reg, and needing no CFG of its own: it only renames
     * memory, and what it renames is what mem2reg then finds. */
    int sroa_twice = g_sroa && g_mem2reg && cfg_ok && !has_igoto;
    if (g_mem2reg && cfg_ok && !has_igoto)
        pass_cxlocal(fn);         /* a private `expected`: by value */
    if (g_sroa) {
        pass_aggcopy(fn);         /* a small struct copied whole: by field */
        pass_sroa(fn, !sroa_twice);
    }
    if (g_mem2reg && cfg_ok && !has_igoto) {
        drop_unreachable(fn);     /* or mem2reg refuses the function */
        pass_mem2reg(fn);         /* global mem2reg (subsumes store-forwarding) */
    }
    /* And a second look at the aggregates, now that mem2reg has run.
     * `int *p = &s.x; ... *p` hides the object behind a pointer
     * VARIABLE, which is memory like any other, so the first look sees
     * the address escape into it and stops. Once the pointer is a temp
     * the chain is plain again and the object turns out to have been
     * private all along -- so whatever that splits, mem2reg is run once
     * more to promote. */
    if (sroa_twice && pass_sroa(fn, 1))
        pass_mem2reg(fn);
    /* Global load CSE is the expensive pass (CFG + an available-expressions
     * dataflow), so it runs ONCE per outer round instead of on every inner
     * iteration. When it exposes copies, the inner fixpoint reconverges and we
     * round again — it settles in one or two rounds. */
    /* The rounds run to a fixpoint, and the guards are how a fixpoint
     * that never comes is noticed rather than waited for. A pair of
     * passes that undo each other -- a constant-copy rule against value
     * numbering, once -- is a compiler that never finishes on a function
     * big enough to make each round slow: an hour on
     * src/arch/x86_64/codegen.c before anyone looked. So the cap is low
     * enough to hit in seconds (a real fixpoint takes a handful of
     * rounds), and hitting it is reported: fatal under EMBCC_VERIFY,
     * where the test suite runs, and a warning otherwise, because every
     * round leaves the function correct and the user's build should not
     * fail for a slow convergence. */
    int outer = 1, oguard = 0;
    while (outer && oguard++ < 100) {
        outer = 0;
        int changed = 1, guard = 0;
        while (changed && guard++ < OPT_MAX_ROUNDS) {
            if (guard == OPT_MAX_ROUNDS)
                opt_no_fixpoint(fn, OPT_MAX_ROUNDS);
            changed = 0;
            changed |= pass_storefwd(fn); /* forward local stores to loads (mem2reg-lite) */
            changed |= pass_roload(fn);   /* a global nothing writes */
            changed |= pass_fold(fn);
            /* ...and the divisors folding has just made constants:
             * `x / (1 << k)`, a const local, an inlined parameter. The
             * run before the rounds sees only literals. */
            if (g_divmagic)
                changed |= pass_divmagic(fn);
            /* After folding, so the constants it just exposed are the
             * ones this moves, and before value numbering, so what it
             * leaves is what CSE sees. */
            changed |= pass_reassoc(fn);
            changed |= pass_lvn(fn);      /* CSE: reuse identical computations */
            /* After value numbering, whose merges are what make an
             * index's use count true. */
            changed |= pass_idxoff(fn);   /* a[i - 1]: the -1 into the address */
            changed |= pass_divmod(fn);   /* a % b from the a / b beside it */
            if (g_divmagic)               /* and `x % C == 0` folding found */
                changed |= pass_divtest(fn);
            if (g_gcse && cfg_ok)
                changed |= pass_gcse(fn); /* CSE across the dominator tree */
            /* SCCP is the one that must stay off without a trustworthy
             * CFG even though it builds its own: it DELETES blocks it
             * finds unreachable, and a landing pad is exactly that. */
            if (g_sccp && cfg_ok)
                changed |= pass_sccp(fn); /* resolve const branches, drop dead blocks */
            changed |= pass_copyprop(fn);
            changed |= pass_copyprop_local(fn);  /* the phi copies the global
                                                  * one cannot touch */
            changed |= pass_dce(fn);
            if (g_cfgclean) {
                changed |= pass_thread(fn);
                changed |= pass_cfgclean(fn);
            }
            if (g_dse && cfg_ok)
                changed |= pass_dse(fn);
            changed |= pass_rangecheck(fn);
        }
        if (pass_punfwd(fn)) {         /* a union's words, without the union */
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        if (g_loadcse && cfg_ok && pass_loadcse(fn)) {   /* reuse loads redundant on every path */
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* PRE after both CSEs have had their go: what it is looking for
         * is what they LEFT -- an expression redundant on some paths and
         * new on the rest. Running it first would have it insert copies
         * for expressions the cheaper passes were about to remove
         * outright. It rebuilds the array, so it belongs out here. */
        if (g_pre && edge_ok && pass_pre(fn)) {
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* Rotation first: it merges the header into the body, so the
         * block-local passes in the next round see one block where they
         * saw two. */
        if (g_licm && edge_ok && pass_rotate(fn)) {
            pass_copyprop_local(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* LICM belongs out here with the other CFG passes: it rebuilds the
         * instruction array, so every block boundary the inner fixpoint
         * might hold is gone. Rounding again matters -- a hoisted
         * expression is a new candidate for folding and CSE in the
         * preheader, and what those leave can expose the next hoist. */
        if (g_licm && edge_ok && pass_licm(fn)) {
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* Vectorizing LAST of the loop passes, because it depends on
         * all of them: rotation for the single-block bottom-tested
         * shape, and LICM for the address base -- until the `gaddr` is
         * hoisted out, every memory reference looks like it is indexed
         * off something that changes, and nothing vectorizes. */
        /* Before vectorizing: a copy or clear loop turned into one
         * operation beats four lanes of the same loop. */
        if (g_idiom && edge_ok && pass_idiom(fn)) {
            pass_dce(fn);
            outer = 1;
        }
        if (g_vec && edge_ok && pass_vectorize(fn)) {
            pass_dce(fn);
            outer = 1;
        }
    }
    /* Strength reduction runs ONCE, after the loop passes have settled,
     * because it destroys the addressing shape the vectorizer matches
     * on: a loop already walking a pointer no longer looks like
     * base + i*scale. Inside the round it would race the vectorizer for
     * the same loops and win, trading four lanes for one add. */
    /* If-conversion after the loop passes: the shape it matches is what
     * phi destruction leaves, and rotation/LICM must have finished
     * moving blocks around before the diamond is the real one. */
    if (g_ifconv && edge_ok && pass_ifconv(fn)) {
        pass_copyprop_local(fn);
        pass_dce(fn);
    }
    if (g_licm && edge_ok && pass_ivsr(fn)) {
        pass_copyprop_local(fn);
        pass_dce(fn);
    }
    /* Unrolling LAST of the loop passes, and for the same reason
     * strength reduction runs late: it leaves copies of the body that no
     * longer look like a loop to anything that wanted to match one.
     * What follows it is the block-local work -- folding, value
     * numbering, copy propagation -- which is most of what the copies
     * are for. */
    /* Switch threading with it: what it copies is a path, not a loop,
     * and the same block-local work cleans up after both. */
    int copied = 0;
    if (g_unroll && edge_ok && pass_unroll(fn))
        copied = 1;
    if (g_swthread && edge_ok && pass_swthread(fn))
        copied = 1;
    if (copied) {
        int changed = 1, g2 = 0;
        while (changed && g2++ < 100) {
            changed = 0;
            changed |= pass_fold(fn);
            /* The copies are a chain of `i+1` on `i+1` on `i+1`, which
             * is the shape this turns into four independent adds. */
            changed |= pass_reassoc(fn);
            changed |= pass_lvn(fn);
            /* ...and an unrolled filter's x[i - 1], x[i - 2], ...: the
             * offsets into the loads, the base shared */
            changed |= pass_idxoff(fn);
            changed |= pass_copyprop(fn);
            changed |= pass_copyprop_local(fn);
            changed |= pass_dce(fn);
            /* The unrolled block opens with a test the loop's own guard
             * has just made -- it has to, because that block is the
             * target of a back edge and the guard runs once -- so on
             * the way IN it is a branch whose answer is already known.
             * This is what notices. */
            if (g_cfgclean)
                changed |= pass_cfgclean(fn);
            /* And a guard that folding has just decided: the copies'
             * value numbering sees `end != base + 2048` with end ==
             * base + 2048 and makes it a constant, which nothing in
             * this loop resolved -- CRC's inner loop entered through
             * `mov $1; test; je` on every outer trip. */
            if (g_sccp && cfg_ok)
                changed |= pass_sccp(fn);
        }
    }
    /* After the fixpoint: fold constant operands into immediates, then DCE the
     * CONSTs that leaves unreferenced. Kept out of the fixpoint so the earlier
     * passes never reason about the imm_b form. */
    /* the 64-bit product of two 32-bit values: one widening multiply */
    if (pass_mulwiden(fn))
        pass_dce(fn);
    if (pass_immfold(fn))
        pass_dce(fn);
    pass_signtest(fn);       /* `if (x >> 63)` is `if (x < 0)` */
    if (pass_storenarrow(fn))
        pass_dce(fn);
    /* ...and only now put each surviving literal where it is wanted.
     *
     * AFTER the fixpoint, not inside it, for the same reason immfold is:
     * this decides where a value is MATERIALISED, not what it is, so
     * every pass that reasons about the instruction order should have
     * finished first. Inside the round it also never settled -- folding
     * and value numbering kept producing literals for it to move and it
     * kept reporting a change, which put format.c at six minutes. */
    /* The joins' copies, before anything decides where literals go: a
     * constant written straight into the merged name is where it was. */
    if (cfg_ok && pass_joincopies(fn) && g_cfgclean)
        pass_cfgclean(fn);
    pass_sinkconst(fn);
    /* Last: the copies it adds must not be propagated away again.
     * EMBCC_NO_SPLITLOOPS=1 turns it off, for bisecting a difference. */
    if (g_licm && edge_ok && !g_opt_size && !getenv("EMBCC_NO_SPLITLOOPS")) {
        int guard = 0;
        while (guard++ < 64 && pass_splitloops(fn))
            ;
    }
    pass_sinkaddr(fn);       /* last: nothing may separate them again */
    /* After every loop pass, at -Os on ARM: a rotated loop entered at its
     * test instead of through a copy of it (pass_guardjump). */
    /* ...and before that, one copy of each repeated block end
     * (pass_tailmerge) */
    if (edge_ok && g_opt_size && !getenv("EMBCC_NO_TAILMERGE")) {
        int guard = 0, any = 0;
        while (guard++ < 256 && pass_tailmerge(fn))
            any = 1;
        /* a block left as only a jump: its branches straight to the copy */
        if (any && g_cfgclean)
            pass_cfgclean(fn);
        if (any)
            tm_sweep(fn);
    }
    if (g_licm && edge_ok && g_opt_size && !getenv("EMBCC_NO_GUARDJUMP")) {
        int guard = 0;
        while (guard++ < 256 && pass_guardjump(fn))
            ;
    }
    if (edge_ok)
        pass_retdup(fn);     /* last: the tail merge would share them */
    if (verify) verify_func(fn, "opt");

    /* What the whole fixpoint came to, for this function. The per-pass
     * decisions above answer "why"; this answers "did anything happen", and
     * it is the number a person compares between two builds. */
    if (remarks_on() && fn->src) {
        remark_add("opt", "optimized", fn->name, "fixpoint-reached",
                   fn->file, fn->line,
                   "%d instructions -> %d", ins_before, fn->nins);
        /* The passes that had been silent. One line, only when they did
         * something, because most functions give every count as zero. */
        if (g_did.lvn || g_did.gcse || g_did.dce || g_did.copy ||
            g_did.loadcse || g_did.dse || g_did.ifconv || g_did.pre)
            remark_add("opt", "rewrote", fn->name, "pass-counts",
                       fn->file, fn->line,
                       "%ld cse, %ld global cse, %ld load reuse, "
                       "%ld copies propagated, %ld dead, %ld dead stores, "
                       "%ld selects, %ld partial redundancies",
                       g_did.lvn, g_did.gcse, g_did.loadcse, g_did.copy,
                       g_did.dce, g_did.dse, g_did.ifconv, g_did.pre);
    }
}

/* -Os is level 2 without vectorization, which is the one pass here that
 * reliably adds code: a vector loop beside the scalar one, plus the
 * remainder test.
 *
 * Inlining is NOT disabled, which is worth writing down because the
 * obvious guess was wrong. Turning it off made the benchmark's object
 * file grow from 3959 bytes to 7340 -- inlining a static function that
 * has one caller lets the original be deleted, so refusing to inline
 * keeps two copies of it -- INLINE_SOLE_CALLEE keeps that case inlined
 * at every level. The body copied into twenty call sites is the other
 * case, and -Os now tells them apart by budget: INLINE_SIZE_CALLEE.
 *
 * Everything else -- folding, value numbering, dead code, loop
 * invariants, strength reduction -- makes code smaller as well as
 * faster, which is why -Os is level 2 and not level 1. */
void opt_run(struct ir_unit *iu, int level)
{
    g_fold_unit = iu;
    int size = level == OPT_SIZE;
    g_opt_size = size;
    if (size)
        level = 2;
    if (level < 1) {
        int any = 0;
        for (int f = 0; f < iu->nfuncs && !any; f++)
            for (int i = 0; i < iu->funcs[f].nins && !any; i++)
                any = iu->funcs[f].ins[i].op == IR_CALL &&
                      iu->funcs[f].ins[i].callee &&
                      iu->funcs[f].ins[i].callee->attr_always_inline;
        if (any) {
            g_inline_always_only = 1;
            g_inline_o1 = 0;
            inline_unit(iu);
            g_inline_always_only = 0;
        }
        return;
    }
    /* -O1 is gcc's -O1: every value that can be is a register, and the
     * passes that only remove work run -- constants, dead stores, loop
     * invariants, branches made selects -- while the ones that trade
     * size or compile time for speed (inlining beyond a sole callee,
     * global CSE, PRE, unrolling, vectorizing) wait for -O2. It used to
     * be folding and copies over values that all lived in memory, and
     * with the sole-callee inlining that made frames LARGER than -O0's:
     * FreeRTOS's timer task overflowed its stack at -O1 alone. */
    pass_default(P_MEM2REG, level >= 1);
    pass_default(P_GCSE,    level >= 2);
    pass_default(P_LICM,    level >= 1);
    /* Vectorization is x86-64 for now: the aarch64 backend refuses the
     * vector opcodes loudly (diag_fatal) rather than emitting something
     * it has not been taught, so the pass must not hand it any. */
    pass_default(P_VEC,     level >= 2 && !size &&
                            target_get() == TARGET_X86_64);
    pass_default(P_LOADCSE, level >= 1);
    pass_default(P_SCCP,    level >= 1);
    pass_default(P_INLINE,  level >= 1);   /* -O1: see g_inline_o1 */
    g_inline_o1 = level == 1;
    pass_default(P_DSE,     level >= 1);
    /* A multiply and two shifts in place of a divide is smaller than
     * the divide's setup on these targets as well as faster, so -Os
     * keeps it. */
    pass_default(P_DIVMAGIC, level >= 1);
    pass_default(P_IFCONV, level >= 1);   /* cmov on x86-64, csel on aarch64 */
    pass_default(P_CFGCLEAN, level >= 1);  /* smaller and simpler at any level */
    pass_default(P_TAILREC, level >= 2);
    pass_default(P_IDIOM, level >= 2);
    /* Splitting a struct into the scalars it is made of makes the code
     * smaller as well as faster -- a field in a register is not loaded --
     * so -Os keeps it too. */
    pass_default(P_SROA, level >= 1);
    /* Unrolling is the one pass besides vectorization that reliably adds
     * code -- U copies of a body, plus the original kept whole for the
     * remainder -- so -Os leaves it off. */
    pass_default(P_UNROLL, level >= 2 && !size);
    pass_default(P_PRE, level >= 2);
    /* It copies the path to a switch once per state, so -Os leaves it
     * off with unrolling. */
    pass_default(P_SWTHREAD, level >= 2 && !size);
    /* The memory half of LICM: a load out of a loop, a location kept in
     * a register across one. Fewer instructions at any level. */
    pass_default(P_LICMMEM, level >= 1);
    g_licm_mem = g_pass[P_LICMMEM].on;
    if (g_pass[P_INLINE].on)      /* inline before the per-function passes clean up */
        inline_unit(iu);
    /* After inlining, so the bodies are the ones that will be compiled,
     * and before the per-function passes, which consult the result at
     * every call site. */
    if (level >= 1)
        infer_attrs(iu);
    /* After inlining too: it is every function's final body that has to
     * leave the global alone. */
    if (level >= 1)
        ro_globals(iu);
    else
        g_nro = 0;
    /* Every function, including one computing with __int128. The folds
     * that are 64-bit refuse a 128-bit width individually now (see
     * pass_fold), which is a great deal narrower than refusing the
     * function: everything else -- value numbering, copy propagation,
     * dead code, and all of the loop passes -- works on it unchanged. */
    for (int f = 0; f < iu->nfuncs; f++) {
        opt_func(&iu->funcs[f]);
        pass_latch_copies(&iu->funcs[f]);
        if (pass_thread_copies(&iu->funcs[f]))
            pass_cfgclean(&iu->funcs[f]);   /* the copy blocks left behind */
        /* After both: they are what put the back-edge copies into the
         * block whose update this moves next to them. */
        if (level >= 1)
            pass_sinkupd(&iu->funcs[f]);
        pass_x86_loadop(&iu->funcs[f]);     /* last: nothing reorders after */
    }
}
