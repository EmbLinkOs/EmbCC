/* ---- function inlining (inter-procedural, -O2) --------------------------- *
 *
 * A call to a small, defined function is replaced by the function's body. The
 * callee's params and locals become caller LOCALS (the memory model EmbIR uses
 * for addressable, possibly-reassigned variables — treating them as temps would
 * break codegen's single-assignment / no-alias assumptions), so the caller's
 * temps renumber UP to open a contiguous local range for them, and its
 * var_tys/var_aligns/scope metadata extend to match. Params are initialised by
 * an STVAR of each argument; every RET becomes `MOV result` + a jump to one
 * shared label after the inlined body. Gated to -O2, so -O0/-O1 are untouched. */

#include "opt_int.h"

/* ---- the budget is flat, and that was measured -----------------------
 *
 * A flat instruction budget looks like the wrong question. What inlining
 * trades is the callee's body against the CALL -- the argument moves,
 * the call, the return -- and that is not a constant, so a cost model
 * ought to beat a number. Three were tried, each a fact already in the
 * IR rather than a guess, and all three made the code BIGGER:
 *
 *   Credit for the call removed (2 + one per argument), and credit for
 *   a CONSTANT argument the callee will fold around (4 each):
 *     lib/libc + lib/libcxx objects, x86-64  472912 -> 474096
 *                                   aarch64  661208 -> 664816
 *   The constant credit alone changed x86-64 by nothing at all and
 *   aarch64 by +384: an argument that is a literal rarely decides a
 *   budget, because the functions it would let in are not near it.
 *
 *   A budget of 200 for the SOLE caller of a static function nothing
 *   else can reach -- where the body is moved rather than copied, so
 *   dead-function elimination takes the original and the unit should
 *   get smaller however big it was:
 *     objects, x86-64  472912 -> 474864
 *              aarch64 661208 -> 667824
 *   and tests/bench/kernels.c memory_stream 0.22 -> 0.287, because that
 *   kernel IS a static function with one caller: its loop nest moved
 *   into main, and a bigger function allocates worse. The size the
 *   original would have freed is smaller than what the caller then
 *   spends on spills.
 *
 * Re-measured 2026-10-02, after the allocator had changed under it: a
 * sole-caller budget of 2000 made lib/libc SMALLER at -O2 and -Os on all
 * five targets (x86-64 -84 bytes, aarch64 -156, RV32 -58, Thumb -22, AVR
 * -54), left tests/bench/kernels.c unchanged on four boards, memory_stream
 * included, and made the workload's state machine 4-11% faster (its
 * per-token function, 316 instructions, now moves into its one caller).
 * So the sole-caller number is 2000; the copied-callee one stays at 24 --
 * at 64 the hash table's probe and key builder were copied into their
 * callers and ran 1-4% SLOWER on every board.
 *
 * One count between the two did pay, measured 2026-10-04: a static
 * function called from exactly TWO places, its address never taken, may
 * be 200 instructions. Inlining both calls removes the original, so the
 * unit pays one extra copy, not one per caller. That was the hash
 * table's probe, key builder and insert again, but after the allocator
 * stopped spilling a loop's counters around calls (ra-callee-k): against
 * 24, the text kernel ran 5.9% (RV32), 8.6% (x86-64) and 10.0% (aarch64)
 * fewer instructions, M4's +0.4%, and hash 9.6-20.9% fewer on all four.
 * -O2 code over lib/libc and the workload grew 1.0-1.6%; -Os is
 * unchanged. The insert is 167 instructions, so a two-caller budget of
 * 64 or 100 bought text and a fraction of hash (0.6-5.4%) for 0.2-1.1%.
 * A flat budget of 64 bought text's speed for 6-9% more code.
 *
 * So the number stays, and this is what it is doing there. A cost model
 * that beats it wants something these three did not have -- how HOT the
 * call is (section 4's profile work), or a real estimate of what the
 * caller's register pressure will do -- not more arithmetic on facts
 * already available here. */
#define INLINE_MAX_CALLEE 24     /* instruction budget for an inline candidate */
#define INLINE_SOLE_CALLEE 2000  /* ...and for a body that MOVES (sole_static_caller) */
#define INLINE_TWO_CALLEE  200   /* ...and for one of two calls to a static (-O2) */
#define INLINE_MAX_CALLER 800    /* stop expanding a caller past this many ins */
#define INLINE_MAX_PER_FUNC 64   /* and cap inlines per caller, for termination */

/* -Os: a much smaller budget, and WHY it needed one.
 *
 * The comment above opt_run used to say that inlining everything -O2 inlines
 * is the smaller answer at -Os too, because a callee with one caller is
 * DELETED after it moves, so refusing to inline it keeps two copies. That
 * half is still true, and INLINE_SOLE_CALLEE below is how it stays true.
 *
 * The other half was wrong, and it was wrong because 24 is a count of IR
 * instructions being used as a proxy for BYTES. An IR op costs one or two
 * instructions on x86-64 and six to ten on AVR, where an int add is four and
 * every value lives in a frame slot. So the same budget that admits a genuine
 * one-liner on a 64-bit machine admits a 450-byte function on an 8-bit one,
 * and lib/rt/avrfp.c is what that looks like from the outside: -Os produced
 * 41392 bytes of text against -O1's 32856, on a part with 32768 of flash.
 * `pack` and `mul24` had been copied into every caller and deleted, and
 * addsub grew 6600 -> 9850 for it.
 *
 * So at -Os a body is inlined only when it MOVES -- the sole-caller case,
 * where there is no duplication to pay for -- or when it is small enough that
 * the copy is plausibly smaller than the call sequence it replaces, on the
 * most expansive target rather than the least. Nothing changes at -O2, so no
 * speed measurement moves; -Os is the level that asked for size.
 */
#define INLINE_SIZE_CALLEE 6

static int vmap(const struct rmp *r, int x)
{
    if (x < 0) return x;
    if (r->kind == 0) return x < r->p1 ? x : x + r->p2;
    return x < r->p3 ? r->p1 + x : r->p2 + x;
}
static void rmp_cb(int *p, void *ctx) { *p = vmap(ctx, *p); }

void remap_ins(struct ir_ins *in, struct rmp *r)
{
    each_read(in, rmp_cb, r);
    /* IR_LANDING's SECOND destination, for the same reason compute_defs
     * has to name it: def_target reports one, and a landing pad left with
     * the callee's numbering for its selector reads a vreg that does not
     * exist in the caller. Inlining a `noexcept` function is enough to
     * reach this -- its pad comes along with it. */
    if (in->op == IR_LANDING) {
        in->dst = vmap(r, in->dst);
        in->b = vmap(r, in->b);
    } else if (def_target(in) >= 0) {
        in->dst = vmap(r, in->dst);
    }
    if (in->op == IR_JMP || in->op == IR_BRZ || in->op == IR_BRNZ ||
        in->op == IR_LABEL || in->op == IR_SWITCH)
        in->label += r->lbase;      /* a switch's table: see inline_call */
}

/* The ir_func for a callee, or NULL if not defined in this unit. */
static struct ir_func *func_ir(struct ir_unit *iu, struct func *callee)
{
    for (int i = 0; i < iu->nfuncs; i++)
        if (iu->funcs[i].src == callee)
            return &iu->funcs[i];
    return NULL;
}

/* Conservative eligibility: a real, small body; scalar-integer params and
 * return only (no varargs / struct / float); no inline asm, va_start, or a
 * struct-returning call in the body. */
/* Each refusal has its OWN reason code (R2). This function used to return 0
 * from a dozen places and every one of them meant something different; by
 * the time anyone asked why a function was not inlined, the twelve answers
 * had collapsed into one. `*why` is the stable code, `*detail` the fact. */
static int inlinable(struct ir_func *cf, int force, int sole, const char **why,
                     char *detail, size_t dcap)
{
    struct func *c = cf->src;
    *why = NULL;
    if (detail && dcap)
        detail[0] = 0;

    if (c->is_varargs)       { *why = "callee-is-varargs";  return 0; }
    if (cf->nins == 0)       { *why = "callee-not-defined-here"; return 0; }
    /* __attribute__((always_inline)) overrides the SIZE budget and
     * nothing else. Every other test below is a thing this inliner
     * cannot do rather than a thing it decided against -- forcing one
     * would not inline the call, it would emit a wrong one. The remark
     * still names whichever test refused, so a function marked
     * always_inline that was not inlined says why. */
    int budget = g_opt_size ? INLINE_SIZE_CALLEE : INLINE_MAX_CALLEE;
    if (!force && cf->nins > budget &&
        !(sole == 1 && cf->nins <= INLINE_SOLE_CALLEE) &&
        !(sole == 2 && !g_opt_size && cf->nins <= INLINE_TWO_CALLEE)) {
        *why = "callee-too-large";
        /* Name the budget that applied: a sole or one-of-two caller's
         * is larger than the copied one. */
        if (sole == 1)
            budget = INLINE_SOLE_CALLEE;
        else if (sole == 2 && !g_opt_size)
            budget = INLINE_TWO_CALLEE;
        if (detail)
            snprintf(detail, dcap, "%d instructions, budget %d%s",
                     cf->nins, budget, g_opt_size ? " (-Os)" : "");
        return 0;
    }
    if (cf->neh)             { *why = "callee-has-exception-regions"; return 0; }
    if (c->ret_ty->kind == TY_STRUCT) { *why = "returns-a-struct"; return 0; }
    /* FLOATS used to be refused here too -- a float return and a float
     * local, 82 and 117 call sites across lib/libc and lib/libcxx
     * against the 218 that were inlined. The reason was that a float
     * had no register to live in, so the inliner's parameter STVARs and
     * its result MOV were memory traffic the call had not been paying;
     * with the float register class and a float pool those are ordinary
     * copies that mem2reg and the allocator see through like any other.
     *
     * lib/rt/complex.c is what this is for: __muldc3 called is_nan_d,
     * is_inf_d and copysign_d thirty-four times, and every product was
     * live across one, so every product was in memory. gcc's __muldc3
     * touches the stack not once. */
    /* A PARAMETER is bound by `stvar local, argvreg`, which is only the
     * value when the argument vreg IS the value. For anything an ABI
     * passes some other way -- a struct or a _Complex (the vreg is its
     * ADDRESS), a long double (the x87 stack), an __int128 (a register
     * pair) -- that store copies the wrong bytes from the wrong place.
     * `clog` is what says so: its callee took a `_Complex double`, the
     * caller had `%3 = addr v0`, and the splice produced
     * `stvar:16 v1, %3` -- sixteen bytes read out of an eight-byte
     * pointer's slot.
     *
     * So a parameter has to be a scalar that fits one vreg. A non-
     * parameter local has no such constraint: the metadata copy below
     * carries its type and alignment, ir_locals_fill rebuilds the
     * descriptors, and SROA looks at it in the caller exactly as it
     * would have in the callee -- which is what lets is_inf_d's
     * `union { double d; u64 u; }` come across. */
    for (int k = 0; k < cf->nparams && k < cf->nvars; k++) {
        struct type *pt = c->var_tys[k];
        if (!pt || ty_size(pt) > 8 || ty_is_complex(pt) ||
            !(ty_is_integer(pt) || pt->kind == TY_PTR || ty_is_float(pt))) {
            *why = "parameter-is-not-a-simple-scalar";
            return 0;
        }
    }
    if (ty_is_complex(c->ret_ty) || c->ret_ty->kind == TY_LDOUBLE ||
        c->ret_ty->kind == TY_INT128) {
        *why = "returns-a-value-wider-than-a-vreg";
        return 0;
    }
    for (int i = 0; i < cf->nins; i++) {
        const struct ir_ins *in = &cf->ins[i];
        /* Inline asm used to be refused here, which left every
         * always_inline CMSIS intrinsic and RTOS critical section a call.
         * An asm comes across with its operands renamed (inline_call);
         * its registers were chosen when it was assembled, and the
         * caller's allocator treats it exactly as the callee's did. */
        if (in->op == IR_VA_START) { *why = "callee-uses-va_start"; return 0; }
        /* `in->flt` used to refuse the callee outright. Floating-point
         * arithmetic is ordinary arithmetic to this pass -- it renames
         * vregs and splices instructions -- and the values it produces
         * now have a register class and a pool to live in. */
        /* a VLA's allocation is released by the callee's own epilogue;
         * inlined into a loop it would never be */
        if (in->op == IR_ALLOCA)   { *why = "callee-has-a-vla"; return 0; }
        /* Computed goto: a label address / indirect jump can't be inlined —
         * the callee's label ids would need remapping into the caller, and the
         * caller then can't be optimized either (opt_func bails on it). */
        if (in->op == IR_LABELADDR || in->op == IR_IGOTO) {
            *why = "callee-uses-a-computed-goto";
            return 0;
        }
        if (in->op == IR_CALL && in->retsize) {
            *why = "callee-calls-a-struct-returning-function";
            return 0;
        }
    }
    return 1;
}

/* Splice the body of cf in place of the call at fn->ins[ci]. */
static void inline_call(struct ir_func *fn, int ci, struct ir_func *cf)
{
    int V = fn->nvars, N = fn->nvregs, L = fn->nlabels;
    int v = cf->nvars, n = cf->nvregs, nparams = cf->nparams;

    /* 1. Open room: shift the caller's temps up by v (locals stay put). */
    struct rmp shift = { 0, V, v, 0, 0 };
    for (int i = 0; i < fn->nins; i++)
        remap_ins(&fn->ins[i], &shift);

    struct ir_ins call = fn->ins[ci];    /* the (now-shifted) call */
    int dst = call.dst;
    int after = L + cf->nlabels;

    /* 2. Build param stores + the remapped body + one exit label. */
    struct ir_ins *buf = xmalloc((size_t)(nparams + 2 * cf->nins + 1) * sizeof *buf);
    int m = 0;
    /* Every instruction the inliner INVENTS still has a source location:
     * the call it replaces (R3). A pass that builds an instruction from
     * scratch and leaves line 0 puts a hole in the line table, and nothing
     * downstream notices -- which is exactly what the verifier's location
     * check now catches, and what it caught here. */
    for (int k = 0; k < nparams; k++) {
        struct ir_ins *s = &buf[m++];
        memset(s, 0, sizeof *s);
        s->op = IR_STVAR;
        s->dst = V + k;                  /* callee param -> caller local */
        s->a = call.argv[k].vreg;
        s->size = cf->locals[k].size;
        s->line = call.line;             /* the argument was written there */
        s->col = call.col;
    }
    struct rmp cm = { 1, V, N, v, L };
    for (int i = 0; i < cf->nins; i++) {
        struct ir_ins in = cf->ins[i];
        /* An asm's operands are renamed in place through each_read, and
         * the callee keeps its own: the copy gets operand arrays of its
         * own (the assembled bytes are never written, and are shared). */
        if (in.op == IR_ASM && in.asm_ir) {
            struct ir_asm *ca = xmalloc(sizeof *ca);
            *ca = *in.asm_ir;
            ca->in = xmalloc((size_t)(ca->nin ? ca->nin : 1) * sizeof *ca->in);
            ca->out = xmalloc((size_t)(ca->nout ? ca->nout : 1) *
                              sizeof *ca->out);
            memcpy(ca->in, in.asm_ir->in, (size_t)ca->nin * sizeof *ca->in);
            memcpy(ca->out, in.asm_ir->out, (size_t)ca->nout * sizeof *ca->out);
            in.asm_ir = ca;
        }
        /* ...and so does a call's argument array, which a rename of the
         * copy would otherwise rewrite in the callee too */
        if (in.op == IR_CALL && in.argv) {
            in.argv = ir_args_copy(in.argv, in.nargs);
        }
        remap_ins(&in, &cm);
        if (in.op == IR_SWITCH)          /* its table, renumbered like its default */
            in.jt = ir_jt_clone(fn, cf, in.jt, L);
        if (in.op == IR_RET) {
            if (in.a >= 0 && dst >= 0) {
                struct ir_ins *mv = &buf[m++];
                ins_blank(mv);
                mv->op = IR_MOV; mv->dst = dst; mv->a = in.a;
                mv->line = in.line;      /* the callee's `return` */
                mv->col = in.col;
            }
            if (i != cf->nins - 1) {     /* the last RET falls into `after` */
                struct ir_ins *jp = &buf[m++];
                ins_blank(jp);
                jp->op = IR_JMP; jp->label = after;
                jp->line = in.line;
                jp->col = in.col;
            }
        } else {
            buf[m++] = in;
        }
    }
    struct ir_ins *lb = &buf[m++];
    memset(lb, 0, sizeof *lb);
    lb->op = IR_LABEL; lb->label = after;
    /* The join label belongs to no source construct: it exists only because
     * the body was spliced in. This is the §9.1 exception, marked rather
     * than left as an absent location the verifier cannot tell from a bug. */
    lb->synth = 1;

    /* 3. Splice buf over the call. */
    int newn = fn->nins - 1 + m;
    struct ir_ins *ni = xmalloc((size_t)newn * sizeof *ni);
    memcpy(ni, fn->ins, (size_t)ci * sizeof *ni);
    memcpy(ni + ci, buf, (size_t)m * sizeof *ni);
    memcpy(ni + ci + m, fn->ins + ci + 1,
           (size_t)(fn->nins - ci - 1) * sizeof *ni);
    free(fn->ins); free(buf);
    fn->ins = ni; fn->nins = newn; fn->cap = newn;

    /* 4. Grow vreg/label space and the caller's var metadata. */
    fn->nvregs = N + n;
    fn->nlabels = L + cf->nlabels + 1;
    /* ...and the stack-argument area: the callee's calls are the
     * caller's now. x86-64 sizes the frame from it, and a caller that
     * passed nothing on the stack itself had none -- so a printf of four
     * long doubles, inlined into main, wrote its arguments over main's
     * own slots (embedded-libc.c at -O1: the fourth printed as the
     * first). */
    if (cf->outgoing_bytes > fn->outgoing_bytes)
        fn->outgoing_bytes = cf->outgoing_bytes;
    if (cf->scratch_bytes > fn->scratch_bytes)
        fn->scratch_bytes = cf->scratch_bytes;
    int nv = V + v;
    struct type **vt = xmalloc((size_t)(nv ? nv : 1) * sizeof *vt);
    int *va = xmalloc((size_t)(nv ? nv : 1) * sizeof *va);
    for (int k = 0; k < V; k++) { vt[k] = fn->src->var_tys[k]; va[k] = fn->src->var_aligns[k]; }
    for (int k = 0; k < v; k++) { vt[V + k] = cf->src->var_tys[k]; va[V + k] = cf->src->var_aligns[k]; }
    fn->src->var_tys = vt;
    fn->src->var_aligns = va;
    /* Both representations grow together: the IR's per-slot descriptors are
     * rebuilt from the types the inliner just extended. Letting them drift
     * is exactly the split brain that miscompiled same-scope. The new count
     * is `nv`; fn->nvars is not updated until the end of this function, and
     * fn->src->nvars is stale by design. */
    ir_locals_fill(fn, fn->src, nv);

    /* Scope ranges are instruction indices; the splice inserted (m-1) net at ci.
     * Shift every existing endpoint past ci, and scope the new callee locals to
     * the inlined region. Kept precise so local-slot coalescing still works. */
    if (fn->var_scope_lo) {
        int *lo = xmalloc((size_t)nv * sizeof *lo);
        int *hi = xmalloc((size_t)nv * sizeof *hi);
        int d = m - 1;
        for (int k = 0; k < V; k++) {
            int a = fn->var_scope_lo[k], b = fn->var_scope_hi[k];
            lo[k] = a <= ci ? a : a + d;
            hi[k] = b <= ci ? b : b + d;
        }
        for (int k = 0; k < v; k++) { lo[V + k] = ci; hi[V + k] = ci + m; }
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = lo; fn->var_scope_hi = hi;
    }
    fn->nvars = nv;
}

/* How many calls in the whole unit name this function, and does its
 * address escape? A STATIC function nothing takes the address of and
 * exactly one call names is a body that will be MOVED rather than
 * copied: once the call is gone, dead-function elimination takes the
 * original, so the unit cannot grow by more than the call it removed.
 * The size budget is the wrong question for that one. Exactly two calls
 * is the next case (INLINE_TWO_CALLEE): both inlined, one copy extra.
 * Returns the count, 1 or 2, and 0 for any other count, a non-static
 * function, or one whose address is taken. */
static int sole_static_caller(struct ir_unit *iu, struct func *c)
{
    if (!c || !c->is_static)
        return 0;
    int calls = 0;
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        for (int i = 0; i < fn->nins; i++) {
            struct ir_ins *in = &fn->ins[i];
            if (in->op == IR_FADDR && in->callee == c)
                return 0;                 /* the address escapes */
            if (in->op == IR_CALL && !in->indirect && in->callee == c &&
                ++calls > 2)
                return 0;
        }
    }
    return calls;      /* 1: the sole caller; 2: one of two; 0: neither */
}

/* -O1: inline only what makes the code smaller or was asked for -- an
 * always_inline callee, and a static function with a single caller,
 * whose original is then deleted. gcc's -O1 does the same (always_inline
 * at every level, -finline-functions-called-once). */
int g_inline_o1;

/* -fno-inline-functions: only a function its author declared `inline`
 * (or always_inline) is a candidate, at any level -- the sole-caller and
 * small-function cases included, which GCC's flag leaves to two others.
 * One flag that means "what I did not mark stays a call" is the one a
 * build can rely on: for a breakpoint, a stack-usage figure, a symbol
 * in the map file. */
static int g_inline_declared_only;
void opt_set_inline_declared_only(int on) { g_inline_declared_only = on; }

/* -O0: only an always_inline callee, which GCC and clang inline at every
 * level -- CMSIS's __STATIC_FORCEINLINE register accessors are written
 * for it, and a header may count on the body being in the caller (one
 * reading its caller's frame or return address). Nothing else changes:
 * no pass after it runs, so -O0's code is otherwise the same. */
int g_inline_always_only;

/* Inline eligible calls across the unit (a bounded fixpoint per caller). */
void inline_unit(struct ir_unit *iu)
{
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        int done = 0;
        for (;;) {
            if (done >= INLINE_MAX_PER_FUNC || fn->nins > INLINE_MAX_CALLER ||
                fn->neh || fn->has_i128)
                break;
            int ci = -1, csole = 0;
            struct ir_func *cf = NULL;
            for (int i = 0; i < fn->nins; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op != IR_CALL || in->indirect || !in->callee ||
                    in->retsize)
                    continue;
                struct ir_func *c = func_ir(iu, in->callee);
                const char *why = NULL;
                char detail[160] = "";
                int ok = 0, sole = 0;
                if (!c)
                    why = "callee-not-defined-here";
                else if (c == fn)
                    why = "would-be-recursive";
                else if (c->has_i128)
                    why = "callee-computes-in-__int128";
                else if (in->callee->attr_noinline)
                    why = "callee-is-noinline";
                else if (g_inline_always_only &&
                         !in->callee->attr_always_inline)
                    why = "not-always_inline-at-O0";
                else if (in->callee->is_weak)
                    why = "callee-is-weak";   /* the link may replace it */
                else if (g_inline_declared_only &&
                         !in->callee->attr_always_inline &&
                         !in->callee->any_inline)
                    why = "not-declared-inline";  /* -fno-inline-functions */
                else {
                    sole = sole_static_caller(iu, in->callee);
                    if (g_inline_o1 && !in->callee->attr_always_inline &&
                        sole != 1)
                        why = "not-a-sole-callee-at-O1";
                    else
                        ok = inlinable(c, in->callee->attr_always_inline,
                                       sole, &why, detail, sizeof detail);
                }
                if (!ok) {
                    remark_add("inline", "not-inlined", in->callee->name, why,
                               fn->src ? fn->file : NULL, in->line,
                               detail[0] ? "%s" : NULL, detail);
                    continue;
                }
                ci = i;
                cf = c;
                csole = sole;
                break;
            }
            if (ci < 0)
                break;
            {
                /* The budget that admitted it, not always the copied
                 * one: a sole caller's or one of two calls' is larger,
                 * and -Os's is smaller. */
                int bud = g_opt_size ? INLINE_SIZE_CALLEE : INLINE_MAX_CALLEE;
                if (cf->nins > bud && csole == 1)
                    bud = INLINE_SOLE_CALLEE;
                else if (cf->nins > bud && csole == 2 && !g_opt_size)
                    bud = INLINE_TWO_CALLEE;
                remark_add("inline", "inlined", cf->name,
                           fn->ins[ci].callee->attr_always_inline
                               ? "always_inline" : "small-enough",
                           fn->src ? fn->file : NULL, fn->ins[ci].line,
                           "%d instructions into %s, budget %d", cf->nins,
                           fn->src ? fn->name : "?", bud);
            }
            inline_call(fn, ci, cf);
            done++;
        }
    }
}
