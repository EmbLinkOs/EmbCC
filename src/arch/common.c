/* What every backend's lowering shares: which vregs hold a sixteen-byte
 * value (cg_wide_vregs) or a float (cg_float_vregs), a string site's
 * index turned into its .rodata offset (cg_resolve_strsites), whether a
 * call is resolved here or by the linker (cg_call_local), and the label
 * marks of GNU C's static label data (cg_label_mark, cg_note_labels).
 * backend.h declares them.
 *
 * They were written in src/arch/x86_64/codegen.c, by the first backend,
 * and every later one called them there -- which made x86-64 a part of
 * every compiler. They moved here when a compiler could be built for a
 * subset of the targets (make TARGETS=, src/arch/target.h): one built for
 * Cortex-M alone has no x86-64 code in it, and still has these. */
#include "backend.h"
#include "regalloc.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

static int wide_def(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_LOAD: case IR_LDVAR:
        return i->size == 16;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_NEG:
    case IR_CALL:
        return i->w == 16;          /* (a long double, or an __int128) */
    case IR_I2F: case IR_F2F:
        return i->w == 16;
    /* __int128: its ops at width 16, an extension to it, a conversion */
    case IR_CONST: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_BNOT: case IR_EXT: case IR_F2I:
        return !i->flt && i->w == 16;
    case IR_CAS16:
        return 1;
    /* A vector result is 16 bytes, so it wants the same 16-aligned slot
     * a long double gets and the same exclusion from the integer
     * register allocator. `wide` is already exactly that map. */
    case IR_VLOAD: case IR_VBIN: case IR_VSPLAT: case IR_VWIDEN:
        return 1;
    default:
        return 0;
    }
}

char *cg_wide_vregs(struct ir_func *fn)
{
    int nv = fn->nvregs ? fn->nvregs : 1;
    char *w = xcalloc((size_t)nv, 1);
    int any = 0;
    for (int v = 0; v < fn->nvars; v++)
        if (fn->locals[v].is_ldouble || fn->locals[v].is_int128)
            w[v] = any = 1;
    for (int n = 0; n < fn->nins; n++)
        if (wide_def(&fn->ins[n]) && fn->ins[n].dst >= 0)
            w[fn->ins[n].dst] = any = 1;
    /* A sixteen-byte value carries its width through every COPY, not
     * just IR_MOV: `ldvar`/`stvar` of one are equally a copy, and
     * gen_x87 lowers all three the same way. Missing those left a vreg
     * that a 16-byte copy WRITES looking like an ordinary eight-byte
     * temp -- so the allocator was free to put it in a register and the
     * copy wrote sixteen bytes over whatever slot it had been given
     * instead. lib/libc/src/math/widths.c does exactly that. */
    for (int changed = any; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int a, d;
            switch (i->op) {
            case IR_MOV: case IR_LDVAR: case IR_STVAR:
                a = i->a; d = i->dst; break;
            default: continue;
            }
            if (a < 0 || d < 0 || a >= nv || d >= nv) continue;
            if (w[a] && !w[d]) { w[d] = 1; changed = 1; }
            if (w[d] && !w[a]) { w[a] = 1; changed = 1; }
        }
    }
    if (!any) { free(w); return NULL; }
    return w;
}

/* The vregs that hold a FLOATING-POINT value -- a float or a double, not
 * a long double, which is x87 or a 16-byte slot and is `wide` instead.
 *
 * Float-ness is not written on every instruction that carries one. An
 * op that COMPUTES in floating point says so (ir_ins::flt), and a
 * conversion says which side is which, but a `ldvar` of a double, a
 * `mov` joining the arms of a `?:`, and a `load` through a pointer all
 * look exactly like their integer selves. So this starts from the
 * places float-ness IS stated and closes over the copies, the same
 * fixpoint cg_wide_vregs does and for the same reason.
 *
 * A compare is the one op whose operands and result disagree: it reads
 * two floats and produces a 0/1 integer. */
/* String sites carry the string INDEX while a function is being lowered,
 * because that is what the IR's IR_STRADDR holds; the driver's relocations
 * want the OFFSET in .rodata. Every backend owes this conversion once its
 * unit is done.
 *
 * It lives here, shared, rather than as the same line in each backend.
 * Four of them had that line and the fifth did not, and the symptom was
 * three strings away from the cause: string index N resolved to
 * rodata + N, so `puts_("DONE\n")` printed "cXYZ" out of the middle of
 * the previous literal and a digit-summing loop over "0123456789" got 224.
 * Nothing faulted and every address was inside .rodata. */
void cg_resolve_strsites(struct ir_unit *iu, struct strsite *s, int n)
{
    for (int k = 0; k < n; k++) {
        /* RK_AVR_TEXT_CALL names a label in .text and its str_off is ALREADY
         * an offset -- a jump too far for AVR's 12-bit rjmp, relocated
         * against the section symbol. Everything else here is a string index
         * into the unit's pool. */
        if (s[k].kind == RK_AVR_TEXT_CALL || s[k].kind == RK_MIPS_TEXT26 ||
            s[k].kind == RK_XTENSA_TEXT32)
            continue;
        s[k].str_off = iu->strs[s[k].str_off].off;
    }
}

/* Is a call from `caller` to `callee` resolved here, as a displacement
 * within one section -- or left to the linker, as a relocation? Only a
 * callee defined in this unit AND placed in the same section can be:
 * a function with a section attribute is laid out apart from .text, and
 * the distance between two sections is the linker's to decide. Shared,
 * like cg_resolve_strsites (AVR relocates every call anyway).
 *
 * Never a WEAK one: the definition here is a default the link may
 * replace, and a call bound to it now still runs the default after the
 * program has supplied its own -- libc's bare-metal write() was, and a
 * program's printf went nowhere. */
int cg_call_local(const struct func *caller, const struct func *callee)
{
    if (!callee->has_defn || callee->is_weak)
        return 0;
    const char *a = caller && caller->section ? caller->section : "";
    const char *b = callee->section ? callee->section : "";
    return strcmp(a, b) == 0;
}

int cg_label_mark(const struct ir_ins *i)
{
    return i->op == IR_LABELADDR && i->vol;
}

void cg_note_labels(struct ir_func *fn, const int *label_off)
{
    struct func *f = fn->src;
    if (!f || !f->label_pos)
        return;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (!cg_label_mark(i) || i->imm < 0 || i->imm >= f->nlabel_pos)
            continue;
        if (i->label < 0 || i->label >= fn->nlabels || label_off[i->label] < 0)
            internal_error("%s: a label static data takes the address of "
                           "was never placed", fn->name);
        f->label_pos[i->imm] =
            (long)label_off[i->label] - (f->code_off + f->code_entry);
    }
}

/* Can this load, store, ldvar or stvar move its value as a float or a
 * double? (cg_float_vregs) */
static int flt_width(const struct ir_ins *i)
{
    if (i->size != 4 && i->size != 8)
        return 0;
    if (i->op == IR_LOAD || i->op == IR_LDVAR)
        return !(i->sign && i->size < i->w);
    return 1;
}

struct flt_bad { char *bad; int *soft; int nv; };
static void flt_bad_cb(int v, void *ctx)
{
    struct flt_bad *b = ctx;
    if (v >= 0 && v < b->nv) {
        if (b->soft) b->soft[v]++;
        else b->bad[v] = 1;
    }
}

static int flt_find(int *uf, int x)
{
    while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; }
    return x;
}

static char *float_vregs(struct ir_func *fn, int by_cost);

char *cg_float_vregs(struct ir_func *fn)
{
    return float_vregs(fn, 0);
}

/* The same classes, decided by cost where a value is touched both ways:
 * for a backend whose integer lowering can also reach a value at home in
 * an FP register (one fmov) -- AArch64. An integer use that can only be
 * made from a general register (an address, a narrow access, a narrow
 * result) still decides for the integer class; one that an fmov serves
 * is a vote, and the floating-point uses are the other votes. fdlibm's
 * `x` is read by a dozen float operations and by the one shift that
 * takes its high word: it belongs in a d register, with one fmov out. */
char *cg_float_vregs_by_cost(struct ir_func *fn)
{
    return float_vregs(fn, 1);
}

static char *float_vregs(struct ir_func *fn, int by_cost)
{
    int nv = fn->nvregs ? fn->nvregs : 1;
    char *w = xcalloc((size_t)nv, 1);
    char *wide = cg_wide_vregs(fn);
    int any = 0;
    /* by_cost: each value's floating-point uses, the votes for its class */
    int *fcnt = by_cost ? xcalloc((size_t)nv, sizeof *fcnt) : NULL;
#define MARK(v) do { int _v=(v); if (_v>=0 && _v<nv) { if (fcnt) fcnt[_v]++; \
                     if (!w[_v]) { w[_v]=1; any=1; } } } while (0)
    for (int v = 0; v < fn->nvars; v++)
        if (fn->locals[v].is_scalar_float) MARK(v);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt) {
            if (i->op == IR_CMP) { MARK(i->a); MARK(i->b); }
            else { MARK(i->dst); MARK(i->a); MARK(i->b); }
        }
        switch (i->op) {
        case IR_I2F:  MARK(i->dst); break;
        case IR_F2I:  MARK(i->a);   break;
        /* A bitcast has a float on exactly one side: `sign` says
         * which, so only that side joins the float class. */
        case IR_BITCAST:
            if (i->sign) MARK(i->a); else MARK(i->dst);
            break;
        case IR_F2F:  MARK(i->dst); MARK(i->a); break;
        case IR_SQRT: MARK(i->dst); MARK(i->a); break;
        default: break;
        }
    }
    for (int changed = any; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int a = -1, d = -1;
            switch (i->op) {
            case IR_MOV:   a = i->a; d = i->dst; break;
            case IR_LDVAR: a = i->a; d = i->dst; break;
            case IR_STVAR: a = i->a; d = i->dst; break;
            default: break;
            }
            if (a < 0 || d < 0 || a >= nv || d >= nv) continue;
            /* a copy carries float-ness both ways */
            if (w[a] && !w[d]) { w[d] = 1; changed = 1; }
            if (w[d] && !w[a]) { w[a] = 1; changed = 1; }
        }
    }
    /* ---- and now take back everything an INTEGER op touches --------
     *
     * Float-ness is not a property of a value here, it is a property of
     * each USE, and the two disagree more often than the arithmetic
     * suggests. A floating-point constant is an ordinary `const` of its
     * bit pattern -- `const.8s 4602678819172646912` is 0.5 -- and
     * negating a double is an integer XOR of the sign bit, not `fneg`.
     * A value reaching either of those has to be in a general register
     * when it gets there.
     *
     * So the mark above is only a candidate, and this is the whitelist:
     * a value stays in this class only if EVERY instruction that touches
     * it treats it as floating point. Anything else takes it back, and
     * the taking-back travels through the copies for the same reason the
     * marking did -- both ends of a `mov` are one value and cannot be in
     * two classes. */
    char *bad = xcalloc((size_t)nv, 1);
    /* by_cost: the integer uses an fmov can serve, counted, not decided */
    int *soft = by_cost ? xcalloc((size_t)nv, sizeof *soft) : NULL;
    struct flt_bad fb = { bad, soft, nv };
    void *bad_ctx = &fb;
#define BAD(v) do { int _v=(v); if (_v>=0 && _v<nv) bad[_v]=1; } while (0)
#define SOFT(v) do { int _v=(v); if (_v>=0 && _v<nv) { \
                     if (soft) soft[_v]++; else bad[_v]=1; } } while (0)
    /* A parameter arrives where its TYPE puts it: an integer one in a
     * general register, whatever its later uses. `double bits(long x) {
     * union { long l; double d; } u; u.l = x; return u.d; }` folds to
     * returning x as a double, which marked x float -- and an integer
     * parameter in the float class had no home the prologue could fill:
     * an internal error at -O1. In the integer class its float uses
     * move it with movq (x86_fld). */
    if (fn->src)
        for (int p = 0; p < fn->nparams && p < nv; p++) {
            const struct type *pt = fn->src->param_tys[p];
            if (pt && !ty_is_float(pt))
                BAD(p);
        }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt) {
            if (i->op == IR_CMP) BAD(i->dst);     /* a 0/1 integer */
            continue;                             /* operands are float */
        }
        switch (i->op) {
        case IR_CONST:
            /* A constant is whatever its consumer needs it to be. The
             * bits of 0.5 are `const.8s 4602678819172646912` and nothing
             * says float, so the default below took it out of the class
             * -- and the taking-back travels through the copies, so one
             * literal put every value reachable from it in memory, in
             * BOTH classes. An op that wants it as an integer still
             * BADs it by its own rule, so the whitelist stays sound. */
            break;
        case IR_I2F:  SOFT(i->a);   break;        /* integer in */
        case IR_F2I:                              /* integer out */
            if (i->w == 8) SOFT(i->dst); else BAD(i->dst);
            break;
        case IR_BITCAST:
            if (i->sign) SOFT(i->dst); else SOFT(i->a);
            break;
        case IR_F2F:
            /* A conversion with a LONG DOUBLE on either side goes
             * through the x87 unit, which loads and stores memory and
             * nothing else -- so the float/double side of it has to be
             * in a slot for x87 to reach. tests/exec/long-double.c:
             * `(double)(one + tiny)`. */
            if (i->w == 16 || i->size == 16) { BAD(i->dst); BAD(i->a); }
            break;
        case IR_SQRT: break;
        case IR_MOV: break;                       /* a copy */
        /* A memory access moves a float or a double as movss/movsd (ldr
         * s/d): four or eight bytes. One of another width is an INTEGER
         * access -- `r->h = 0` where the same `const 0` is also the 0.0f
         * of `f * 0.0f` -- and from an FP register it stored eight bytes
         * into a two-byte field. A signed widening read is integer too:
         * movss fills the high half with zeros, not the sign. */
        case IR_LDVAR: case IR_STVAR:             /* copies, unless narrow */
            if (!flt_width(i)) { BAD(i->a); BAD(i->dst); }
            break;
        case IR_LOAD:
            BAD(i->a);                            /* the address */
            if (!flt_width(i)) BAD(i->dst);
            break;
        case IR_STORE:
            BAD(i->a);                            /* the address */
            if (!flt_width(i)) BAD(i->b);
            break;
        /* A floating-point return is `flt` and never reaches here; this
         * one hands its value back in rax (x0), so the value has to be
         * in a general register. */
        case IR_RET:   SOFT(i->a); break;
        case IR_CALL:
            if (i->indirect) BAD(i->a);
            for (int k = 0; k < i->nargs; k++)
                if (i->argv[k].cls[0] != CLASS_SSE) SOFT(i->argv[k].vreg);
            if (!i->flt) {
                if (i->w == 8 && !i->retsize) SOFT(i->dst); else BAD(i->dst);
            }
            break;
        default:
            /* every other op is integer in and integer out -- in the
             * operands it has, which ra_each_use knows. The fields
             * themselves do not say: a two-operand op's `c` is 0, not
             * -1, so `and.4 %x, #1` or an `ext` anywhere in a function
             * took vreg 0 -- the first parameter -- out of the float
             * class, and a double argument went through memory. */
            /* a result narrower than eight bytes is not something an
             * fmov puts in a d register whole */
            if (i->w == 8) SOFT(i->dst); else BAD(i->dst);
            ra_each_use(i, flt_bad_cb, bad_ctx);
            break;
        }
    }
#undef BAD
#undef SOFT
    if (by_cost) {
        /* A copy's two ends are one value: the votes are pooled over the
         * copies, and a hard integer use anywhere decides for all. */
        int *uf = xmalloc((size_t)nv * sizeof *uf);
        for (int v = 0; v < nv; v++) uf[v] = v;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if ((i->op == IR_MOV || i->op == IR_LDVAR || i->op == IR_STVAR) &&
                i->a >= 0 && i->a < nv && i->dst >= 0 && i->dst < nv) {
                int x = flt_find(uf, i->a), y = flt_find(uf, i->dst);
                if (x != y) uf[x] = y;
            }
        }
        long *fs = xcalloc((size_t)nv, sizeof *fs);
        long *is = xcalloc((size_t)nv, sizeof *is);
        char *hard = xcalloc((size_t)nv, 1);
        for (int v = 0; v < nv; v++) {
            int r = flt_find(uf, v);
            fs[r] += fcnt[v];
            is[r] += soft[v];
            hard[r] |= bad[v];
        }
        for (int v = 0; v < nv; v++) {
            int r = flt_find(uf, v);
            bad[v] = hard[r] || (is[r] > 0 && fs[r] < is[r]);
        }
        free(uf); free(fs); free(is); free(hard);
        free(fcnt); free(soft);
    }
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int a, d;
            switch (i->op) {
            case IR_MOV: case IR_LDVAR: case IR_STVAR:
                a = i->a; d = i->dst; break;
            default: continue;
            }
            if (a < 0 || d < 0 || a >= nv || d >= nv) continue;
            if (bad[a] && !bad[d]) { bad[d] = 1; changed = 1; }
            if (bad[d] && !bad[a]) { bad[a] = 1; changed = 1; }
        }
    }
    any = 0;
    for (int v = 0; v < nv; v++) {
        if (bad[v]) w[v] = 0;
        if (w[v]) any = 1;
    }
    free(bad);

    /* A long double is not this class: it is sixteen bytes and has its
     * own slot. Whatever marked one, unmark it. */
    if (wide)
        for (int v = 0; v < nv; v++)
            if (wide[v]) w[v] = 0;
    free(wide);
#undef MARK
    if (!any) { free(w); return NULL; }
    return w;
}
