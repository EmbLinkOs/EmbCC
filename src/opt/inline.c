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
/* A call site the profile says is hot (-fprofile-use: IR_HEAT_HOT) is
 * where a copy pays: four times the budget. A cold one, or a call of a
 * cold function, is not inlined at all, unless it is the callee's only
 * call (which costs nothing) or always_inline. In a cold caller the
 * budget is -Os's. */
#define INLINE_HOT_CALLEE 96
static int g_site_hot, g_site_size;

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
/* ...and a function DECLARED inline gets more at -Os, counted as it will
 * be once copied (inl_size: no parameter slots, moves or constants), as
 * GCC honours the keyword there. An RTOS's list helpers -- four loads and
 * stores, `static inline` in a header -- were 24 raw instructions, so
 * every call stayed a call, and on Xtensa's windowed ABI each one an
 * entry, a retw and a call8 (#184: GCC inlines them all). Measured over
 * lib/libc at -Os against the plain rule (2026-11-08), budgets 10 to 20:
 * 12 is the smallest on every target tried -- AVR -0.21%, Cortex-M4
 * -0.16%, Xtensa -0.10%, RV32 -0.06% -- and admits the list unlink (12
 * as optimized); 13 grows RV32, Cortex-M4 and Xtensa, 20 all four.
 * Raising the plain budget instead grew lib/libc 1-3% everywhere. */
#define INLINE_SIZE_DECLARED 12

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
/* inline_unit's index of the unit's functions: sorted by their source
 * node, with how many direct calls name each and whether its address is
 * taken -- kept current as calls are inlined (inline_call). Without it
 * each call site scanned the unit twice, which a whole program under
 * -flto (thousands of functions) made the whole compile. */
struct fent { struct func *src; int k; };
static struct fent *g_fidx;
static int g_nfidx;
static int *g_ncalls;
static char *g_addr_taken;

static int fent_cmp(const void *a, const void *b)
{
    unsigned long x = (unsigned long)((const struct fent *)a)->src;
    unsigned long y = (unsigned long)((const struct fent *)b)->src;
    return x < y ? -1 : x > y;
}

/* the index of callee's body in the unit, or -1 */
static int func_k(struct func *callee)
{
    int lo = 0, hi = g_nfidx - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fidx[mid].src == callee)
            return g_fidx[mid].k;
        if ((unsigned long)g_fidx[mid].src < (unsigned long)callee)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void count_calls(const struct ir_func *fn, int sign)
{
    for (int i = 0; i < fn->nins; i++) {
        const struct ir_ins *in = &fn->ins[i];
        int k;
        if (in->op == IR_FADDR && (k = func_k(in->callee)) >= 0)
            g_addr_taken[k] = 1;
        else if (in->op == IR_CALL && !in->indirect &&
                 (k = func_k(in->callee)) >= 0)
            g_ncalls[k] += sign;
    }
}

static struct ir_func *func_ir(struct ir_unit *iu, struct func *callee)
{
    int k = func_k(callee);
    return k >= 0 ? &iu->funcs[k] : NULL;
}

/* Conservative eligibility: a real, small body; scalar-integer params and
 * return only (no varargs / struct / float); no inline asm, va_start, or a
 * struct-returning call in the body. */
/* Each refusal has its OWN reason code (R2). This function used to return 0
 * from a dozen places and every one of them meant something different; by
 * the time anyone asked why a function was not inlined, the twelve answers
 * had collapsed into one. `*why` is the stable code, `*detail` the fact. */
/* -O3 measures a callee by what it will be, not by what irgen wrote.
 *
 * The callee is weighed before any pass has run on it (inline_unit runs
 * ahead of the per-function passes), so its count is mostly the frame:
 * `static unsigned short cls(int c) { return ... ? t[c + 1] : 0; }` is 34
 * instructions as written and five once mem2reg and folding are done
 * with it, and 24 refuses it. Across lib/libc, 700 calls that -O2 leaves
 * are to a function of 16 instructions or fewer once optimized. So at -O3
 * the count leaves out what the passes will take away: labels, copies
 * and constants, and the loads and stores of a local mem2reg promotes (a
 * scalar whose address is never taken). The budgets are -O2's, applied
 * to that count; -O2 and -Os keep the plain count they were measured
 * with. */
static int inl_size(const struct ir_func *cf)
{
    if (!g_opt_max)
        return cf->nins;
    int nv = cf->nvars;
    char *addr = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int i = 0; i < cf->nins; i++)
        if (cf->ins[i].op == IR_ADDR && cf->ins[i].a >= 0 && cf->ins[i].a < nv)
            addr[cf->ins[i].a] = 1;
    int n = 0;
    for (int i = 0; i < cf->nins; i++) {
        const struct ir_ins *in = &cf->ins[i];
        int v = in->op == IR_LDVAR ? in->a : in->op == IR_STVAR ? in->dst : -1;
        if (in->op == IR_LABEL || in->op == IR_MOV || in->op == IR_CONST)
            continue;
        if (v >= 0 && v < nv && !addr[v] && cf->locals && !in->vol &&
            !cf->locals[v].is_volatile &&
            (cf->locals[v].is_scalar_int_or_ptr || cf->locals[v].is_scalar_float))
            continue;
        n++;
    }
    free(addr);
    return n;
}

static int inlinable(struct ir_func *cf, int force, int sole, const char **why,
                     char *detail, size_t dcap)
{
    struct func *c = cf->src;
    *why = NULL;
    if (detail && dcap)
        detail[0] = 0;

    if (c->is_varargs)       { *why = "callee-is-varargs";  return 0; }
    /* main stays a frame of its own, as GCC keeps it: called once, from a
     * reset handler or crt0, it gains nothing inlined, and inlined it is
     * an inline frame of its caller -- gdb's backtrace then runs past the
     * entry function into 0x0, and `break main` stops in reset's code */
    if (cf->name && !strcmp(cf->name, "main")) { *why = "callee-is-main"; return 0; }
    if (cf->nins == 0)       { *why = "callee-not-defined-here"; return 0; }
    /* __attribute__((always_inline)) overrides the SIZE budget and
     * nothing else. Every other test below is a thing this inliner
     * cannot do rather than a thing it decided against -- forcing one
     * would not inline the call, it would emit a wrong one. The remark
     * still names whichever test refused, so a function marked
     * always_inline that was not inlined says why. */
    int small = g_opt_size || g_site_size;
    int budget = small ? INLINE_SIZE_CALLEE
               : g_site_hot ? INLINE_HOT_CALLEE : INLINE_MAX_CALLEE;
    int size = small ? cf->nins : inl_size(cf);
    if (small && c->def_inline) {
        int keep = g_opt_max;
        g_opt_max = 1;                  /* inl_size's optimized count */
        size = inl_size(cf);
        g_opt_max = keep;
        budget = INLINE_SIZE_DECLARED;
    }
    if (!force && size > budget &&
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
                     size, budget, g_opt_size ? " (-Os)"
                                 : size != cf->nins ? " (-O3: as optimized)" : "");
        return 0;
    }
    if (cf->neh)             { *why = "callee-has-exception-regions"; return 0; }
    /* A struct (or _Complex) result used to be refused here, and an
     * RTOS wrote its time constructors as macros for it. It comes back
     * as the address of the value (IR_RET's `a`, `size` bytes), and the
     * call's result is the address of a buffer -- so inlined, the value
     * is copied into a local of the caller's, or, where every return
     * names the same local, that local IS the result (inline_call). */
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
    /* A struct or _Complex parameter is bound by copying from the address
     * its argument vreg holds (inline_call; args_match checks the call
     * passes it so). */
    for (int k = 0; k < cf->nparams && k < cf->nvars; k++) {
        struct type *pt = c->var_tys[k];
        if (pt && pt->kind == TY_STRUCT && pt->complete)
            continue;
        if (!pt || ty_size(pt) > 8 || ty_is_complex(pt) ||
            !(ty_is_integer(pt) || pt->kind == TY_PTR || ty_is_float(pt))) {
            *why = "parameter-is-not-a-simple-scalar";
            return 0;
        }
    }
    if (c->ret_ty->kind == TY_LDOUBLE || c->ret_ty->kind == TY_INT128) {
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
        /* A struct-returning call in the body comes across with its
         * result buffer moved past the caller's (inline_call). */
    }
    return 1;
}

/* Does this call pass each parameter as the callee binds it: a struct or
 * _Complex as the address of the value, anything else as the value? */
static int args_match(const struct ir_ins *call, const struct ir_func *cf)
{
    if (call->nargs < cf->nparams || call->ret_x87 || call->ret_ldref)
        return 0;
    for (int k = 0; k < cf->nparams && k < cf->nvars; k++) {
        const struct type *pt = cf->src->var_tys[k];
        /* a one-member struct passed as the integer it holds (sema's
         * param_scalar) is bound as a scalar is */
        int agg = pt && pt->kind == TY_STRUCT &&
                  !(k < MAX_PARAMS && cf->src->param_scalar[k]);
        if (agg != !!call->argv[k].is_struct)
            return 0;
    }
    return 1;
}

/* The local every `return` of a struct-returning callee names, when
 * there is one: `T t = {...}; return t;` -- each IR_RET's `a` made by
 * `addr` of the same local, and nothing else defines it. Then the
 * result needs no copy: it is that local, in the caller. -1 if not. */
static int ret_local(const struct ir_func *cf)
{
    int k = -1;
    for (int i = 0; i < cf->nins; i++) {
        const struct ir_ins *r = &cf->ins[i];
        if (r->op != IR_RET)
            continue;
        if (r->a < cf->nvars || r->a >= cf->nvregs)
            return -1;
        int def = -1, ndef = 0;
        for (int j = 0; j < cf->nins; j++)
            if (def_target(&cf->ins[j]) == r->a) {
                def = j;
                ndef++;
            }
        if (ndef != 1 || cf->ins[def].op != IR_ADDR ||
            (k >= 0 && cf->ins[def].a != k))
            return -1;
        k = cf->ins[def].a;
    }
    return k;
}

/* Splice the body of cf in place of the call at fn->ins[ci]. */
static void inline_call(struct ir_func *fn, int ci, struct ir_func *cf)
{
    int V = fn->nvars, N = fn->nvregs, L = fn->nlabels;
    int v = cf->nvars, n = cf->nvregs, nparams = cf->nparams;
    /* A struct result: the local the callee returns (ret_local), else a
     * new local of the caller's -- `res`, one past the callee's -- that
     * each return copies into. The call's result is its address. */
    /* (A one-member struct the ABI returns as the integer it holds --
     * ret_scalar -- is a scalar result to both sides, and takes the
     * ordinary path.) */
    int sret = cf->src->ret_ty->kind == TY_STRUCT && !cf->src->ret_scalar;
    int direct = sret ? ret_local(cf) : -1;
    int xl = sret && direct < 0 ? 1 : 0;        /* the extra local, res */
    int res = V + v;

    /* 1. Open room: shift the caller's temps up by v (locals stay put). */
    struct rmp shift = { 0, V, v + xl, 0, 0 };
    for (int i = 0; i < fn->nins; i++)
        remap_ins(&fn->ins[i], &shift);

    struct ir_ins call = fn->ins[ci];    /* the (now-shifted) call */
    int dst = call.dst;
    int after = L + cf->nlabels;
    int X = N + xl + n;                  /* temps this splice invents */
    /* the callee's struct-returning calls' buffers, past the caller's */
    int sbase = (fn->scratch_bytes + 15) & ~15;

    /* 2. Build param stores + the remapped body + one exit label. */
    struct ir_ins *buf = xmalloc((size_t)(2 * nparams + 4 * cf->nins + 4) *
                                 sizeof *buf);
    int m = 0;
    /* Every instruction the inliner INVENTS still has a source location:
     * the call it replaces (R3). A pass that builds an instruction from
     * scratch and leaves line 0 puts a hole in the line table, and nothing
     * downstream notices -- which is exactly what the verifier's location
     * check now catches, and what it caught here. */
    for (int k = 0; k < nparams; k++) {
        if (call.argv[k].is_struct) {    /* the value at that address */
            struct ir_ins *ad = &buf[m++];
            ins_blank(ad);
            ad->op = IR_ADDR; ad->dst = X++; ad->a = V + k;
            ad->line = call.line; ad->col = call.col;
            struct ir_ins *cp = &buf[m++];
            ins_blank(cp);
            cp->op = IR_MEMCPY; cp->a = ad->dst; cp->b = call.argv[k].vreg;
            cp->size = cf->locals[k].size;
            cp->line = call.line; cp->col = call.col;
            continue;
        }
        struct ir_ins *s = &buf[m++];
        memset(s, 0, sizeof *s);
        s->op = IR_STVAR;
        s->dst = V + k;                  /* callee param -> caller local */
        s->a = call.argv[k].vreg;
        s->size = cf->locals[k].size;
        s->line = call.line;             /* the argument was written there */
        s->col = call.col;
    }
    struct rmp cm = { 1, V, N + xl, v, L };
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
        if (in.op == IR_CALL) {          /* its buffers, past the caller's */
            if (in.retsize || in.ret_ldref)
                in.scratch += sbase;
            for (int k = 0; k < in.nargs; k++)
                if (in.argv[k].byref && target_win64_abi())
                    in.argv[k].copy_off += sbase;
        }
        remap_ins(&in, &cm);
        if (in.op == IR_SWITCH)          /* its table, renumbered like its default */
            in.jt = ir_jt_clone(fn, cf, in.jt, L);
        if (in.op == IR_RET && sret) {
            if (in.a >= 0 && dst >= 0 && direct < 0) {
                struct ir_ins *ad = &buf[m++];
                ins_blank(ad);
                ad->op = IR_ADDR; ad->dst = X++; ad->a = res;
                ad->line = in.line; ad->col = in.col;
                struct ir_ins *cp = &buf[m++];
                ins_blank(cp);
                cp->op = IR_MEMCPY; cp->a = ad->dst; cp->b = in.a;
                cp->size = in.size;
                cp->natural = in.natural;
                cp->line = in.line; cp->col = in.col;
            }
            if (i != cf->nins - 1) {
                struct ir_ins *jp = &buf[m++];
                ins_blank(jp);
                jp->op = IR_JMP; jp->label = after;
                jp->line = in.line;
                jp->col = in.col;
            }
        } else if (in.op == IR_RET) {
            if (in.a >= 0 && dst >= 0) {
                struct ir_ins *mv = &buf[m++];
                ins_blank(mv);
                mv->op = IR_MOV; mv->dst = dst; mv->a = in.a;
                /* the value's width and class are the call's -- what the
                 * caller read the result as. Without them the move was
                 * width 0, which AArch64 copied between stack slots as a
                 * single (ldur/stur s): four bytes of a double, so a
                 * static inline function's double result was garbage in
                 * its top half (msun's lgamma of a negative argument
                 * returned infinity at -O1). */
                mv->w = call.w;
                mv->flt = call.flt;
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
    /* A callee that can fall off its closing brace returns nothing on
     * that path: irgen writes no `ret` for a float result's, nor for any
     * whose brace only a noreturn call stands before (`long long
     * llabs(long long b) { abort(); }`, GCC's torture execute/
     * 20021127-1.c). Spliced in, that path reached the caller's read of
     * the result with nothing ever written to it -- a temp with no
     * definition, which the AVR backend, giving it no slot, stopped on
     * ("a frame access in a function whose frame pointer was not set
     * up"). So the path writes 0, as irgen's own falling-off return of
     * an integer does: the result has a definition wherever it is read,
     * and the value is the same every time. */
    if (dst >= 0 && cf->nins > 0 && !sret) {
        enum ir_op last = cf->ins[cf->nins - 1].op;
        if (last != IR_RET && last != IR_JMP && last != IR_UD2 &&
            last != IR_SWITCH && last != IR_IGOTO) {
            struct ir_ins *z = &buf[m++];
            ins_blank(z);
            z->op = IR_CONST; z->dst = dst; z->imm = 0;
            z->w = call.w ? call.w : 4;
            z->line = call.line;     /* the call, whose value this is */
            z->col = call.col;
        }
    }
    struct ir_ins *lb = &buf[m++];
    memset(lb, 0, sizeof *lb);
    lb->op = IR_LABEL; lb->label = after;
    /* The join label belongs to no source construct: it exists only because
     * the body was spliced in. This is the §9.1 exception, marked rather
     * than left as an absent location the verifier cannot tell from a bug. */
    lb->synth = 1;
    if (sret && dst >= 0) {              /* the result: where the value is */
        struct ir_ins *ad = &buf[m++];
        ins_blank(ad);
        ad->op = IR_ADDR; ad->dst = dst;
        ad->a = direct >= 0 ? V + direct : res;
        ad->line = call.line; ad->col = call.col;
    }

    /* -g: the callee's code is an inlined call, a scope of the caller's
     * inside the call's, and the callee's own scopes inside that one --
     * its blocks, and the calls inlined into it before -- renumbered from
     * `base`. The DWARF writer makes it a DW_TAG_inlined_subroutine. */
    int base = -1;
    if (fn->scope_up && cf->scope_up && cf->nscopes > 0) {
        base = fn->nscopes;
        int ns = base + cf->nscopes;
        int had = fn->scope_fn != NULL;
        fn->scope_up = xrealloc(fn->scope_up, (size_t)ns * sizeof *fn->scope_up);
        fn->scope_cap = ns;
        fn->scope_fn = xrealloc(fn->scope_fn, (size_t)ns * sizeof *fn->scope_fn);
        fn->scope_cline = xrealloc(fn->scope_cline, (size_t)ns * sizeof *fn->scope_cline);
        fn->scope_ccol = xrealloc(fn->scope_ccol, (size_t)ns * sizeof *fn->scope_ccol);
        if (!had)
            for (int t = 0; t < base; t++) {
                fn->scope_fn[t] = NULL;
                fn->scope_cline[t] = fn->scope_ccol[t] = 0;
            }
        for (int t = 0; t < cf->nscopes; t++) {
            int id = base + t;
            if (t == 0) {
                fn->scope_up[id] = call.scope;
                fn->scope_fn[id] = cf->src;
                fn->scope_cline[id] = call.line;
                fn->scope_ccol[id] = call.col;
            } else {
                fn->scope_up[id] = base + cf->scope_up[t];
                fn->scope_fn[id] = cf->scope_fn ? cf->scope_fn[t] : NULL;
                fn->scope_cline[id] = cf->scope_cline ? cf->scope_cline[t] : 0;
                fn->scope_ccol[id] = cf->scope_ccol ? cf->scope_ccol[t] : 0;
            }
        }
        fn->nscopes = ns;
    }
    for (int k = 0; k < m; k++)
        buf[k].scope = base < 0 ? call.scope
                     : base + (buf[k].scope > 0 && buf[k].scope < cf->nscopes
                               ? buf[k].scope : 0);

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
    fn->nvregs = X;
    fn->nlabels = L + cf->nlabels + 1;
    /* ...and the stack-argument area: the callee's calls are the
     * caller's now. x86-64 sizes the frame from it, and a caller that
     * passed nothing on the stack itself had none -- so a printf of four
     * long doubles, inlined into main, wrote its arguments over main's
     * own slots (embedded-libc.c at -O1: the fourth printed as the
     * first). */
    if (cf->outgoing_bytes > fn->outgoing_bytes)
        fn->outgoing_bytes = cf->outgoing_bytes;
    /* The callee's scratch -- its struct-returning calls' buffers, a
     * Win64 byref argument's copy -- is moved past the caller's (above):
     * shared, a struct the caller still held could be overwritten by an
     * inlined call's result. */
    if (cf->scratch_bytes)
        fn->scratch_bytes = sbase + cf->scratch_bytes;
    int nv = V + v + xl;
    struct type **vt = xmalloc((size_t)(nv ? nv : 1) * sizeof *vt);
    int *va = xmalloc((size_t)(nv ? nv : 1) * sizeof *va);
    for (int k = 0; k < V; k++) { vt[k] = fn->src->var_tys[k]; va[k] = fn->src->var_aligns[k]; }
    for (int k = 0; k < v; k++) { vt[V + k] = cf->src->var_tys[k]; va[V + k] = cf->src->var_aligns[k]; }
    if (xl) {
        vt[res] = cf->src->ret_ty;
        va[res] = ty_align(cf->src->ret_ty);
    }
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
        for (int k = 0; k < v + xl; k++) { lo[V + k] = ci; hi[V + k] = ci + m; }
        /* the result's local is read through the call's result after the
         * spliced region: it lives to the end of the function */
        if (sret) {
            int r = direct >= 0 ? V + direct : res;
            hi[r] = newn;
        }
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = lo; fn->var_scope_hi = hi;
    }
    fn->nvars = nv;

    /* -g: the callee's parameters and locals are the caller's slots V + k
     * now, so the caller's own mem2reg follows them (vreg_var) and they get
     * location lists like any local; each in its inlined scope, its origin
     * the callee's variable (or, inlined into the callee before, that
     * one's own). */
    if (base >= 0)
        for (int d = 0; d < cf->ndbgvars; d++) {
            struct ir_dbgvar x = cf->dbgvars[d];
            if (x.vreg < 0 || x.vreg >= v)
                continue;
            x.vreg = V + x.vreg;
            x.scope = base + (x.scope > 0 && x.scope < cf->nscopes ? x.scope : 0);
            if (!x.origin) {
                x.origin = cf->src;
                x.origin_idx = d;
            }
            if (fn->ndbgvars == fn->dbgvarcap) {
                fn->dbgvarcap = fn->dbgvarcap ? fn->dbgvarcap * 2 : 8;
                fn->dbgvars = xrealloc(fn->dbgvars,
                                       (size_t)fn->dbgvarcap * sizeof *fn->dbgvars);
            }
            fn->dbgvars[fn->ndbgvars++] = x;
        }
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
    (void)iu;
    if (!c || !c->is_static)
        return 0;
    int k = func_k(c);
    if (k < 0 || g_addr_taken[k])
        return 0;                         /* the address escapes */
    int calls = g_ncalls[k];
    return calls >= 1 && calls <= 2 ? calls : 0;
                       /* 1: the sole caller; 2: one of two; 0: neither */
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
static void inline_unit_(struct ir_unit *iu);

void inline_unit(struct ir_unit *iu)
{
    g_nfidx = iu->nfuncs;
    g_fidx = xmalloc((size_t)(g_nfidx ? g_nfidx : 1) * sizeof *g_fidx);
    for (int k = 0; k < g_nfidx; k++) {
        g_fidx[k].src = iu->funcs[k].src;
        g_fidx[k].k = k;
    }
    qsort(g_fidx, (size_t)g_nfidx, sizeof *g_fidx, fent_cmp);
    g_ncalls = xcalloc((size_t)(g_nfidx ? g_nfidx : 1), sizeof *g_ncalls);
    g_addr_taken = xcalloc((size_t)(g_nfidx ? g_nfidx : 1), 1);
    for (int k = 0; k < g_nfidx; k++)
        count_calls(&iu->funcs[k], 1);
    inline_unit_(iu);
    free(g_fidx); free(g_ncalls); free(g_addr_taken);
    g_fidx = NULL; g_ncalls = NULL; g_addr_taken = NULL; g_nfidx = 0;
}

static void inline_unit_(struct ir_unit *iu)
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
                if (in->op != IR_CALL || in->indirect || !in->callee)
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
                else if (!args_match(in, c))
                    why = "argument-passed-another-way";
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
                    int fcold = fn->src && (fn->src->attr_cold ||
                                            fn->src->prof_heat == IR_HEAT_COLD);
                    int ccold = in->callee->attr_cold ||
                                in->callee->prof_heat == IR_HEAT_COLD;
                    if (g_inline_o1 && !in->callee->attr_always_inline &&
                        sole != 1)
                        why = "not-a-sole-callee-at-O1";
                    else if ((in->heat == IR_HEAT_COLD || ccold) &&
                             !in->callee->attr_always_inline && sole != 1)
                        why = in->heat == IR_HEAT_COLD ? "cold-call-site"
                                                       : "callee-is-cold";
                    else {
                        g_site_hot = in->heat == IR_HEAT_HOT && !fcold;
                        g_site_size = fcold;
                        ok = inlinable(c, in->callee->attr_always_inline,
                                       sole, &why, detail, sizeof detail);
                        g_site_hot = g_site_size = 0;
                    }
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
                int sz = g_opt_size ? cf->nins : inl_size(cf);
                if (g_opt_size && cf->src && cf->src->def_inline) {
                    int keep = g_opt_max;
                    g_opt_max = 1;
                    sz = inl_size(cf);
                    g_opt_max = keep;
                    bud = INLINE_SIZE_DECLARED;
                }
                if (sz > bud && csole == 1)
                    bud = INLINE_SOLE_CALLEE;
                else if (sz > bud && csole == 2 && !g_opt_size)
                    bud = INLINE_TWO_CALLEE;
                if (sz > bud)
                    sz = cf->nins;            /* admitted by a larger budget */
                remark_add("inline", "inlined", cf->name,
                           fn->ins[ci].callee->attr_always_inline
                               ? "always_inline" : "small-enough",
                           fn->src ? fn->file : NULL, fn->ins[ci].line,
                           "%d instructions%s into %s, budget %d", sz,
                           sz != cf->nins ? " as optimized" : "",
                           fn->src ? fn->name : "?", bud);
            }
            /* the counts: the caller's calls before and after */
            count_calls(fn, -1);
            inline_call(fn, ci, cf);
            count_calls(fn, 1);
            done++;
        }
    }
}
