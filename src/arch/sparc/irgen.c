/* SPARC V8's share of IR generation: va_arg, and which constants the
 * optimizer may fold into an instruction.
 *
 * The SPARC ABI passes a variadic argument exactly where a named one
 * would go -- word after word, the first six in %o0-%o5 -- and a variadic
 * callee stores %i0-%i5 into the home area its caller reserved at %fp+68,
 * so the whole argument list is one block of words and a va_list is a
 * bare pointer walking it (codegen.c's prologue). There is no alignment
 * padding anywhere: a double or long long may start at any word. A
 * `float` arrives as a `double`. Every aggregate -- a structure, a union,
 * a _Complex -- and a long double is passed BY REFERENCE: its word is the
 * address of the caller's copy.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"
#include "asm.h"
#include "emit.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Is an argument of this type passed as the address of a copy? */
static int by_reference(const struct type *t)
{
    return t->kind == TY_STRUCT || ty_is_xldouble(t);
}

int irg_va_arg_sparc(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt) && !ty_is_xldouble(rt);
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    int byref = by_reference(rt);

    /* Every access here is natural: the va_list is a pointer object, as
     * aligned as its lvalue is, and every argument word is a word. An
     * eight-byte value at a word that is not 8-aligned is read as two
     * words by the backend, which never uses ldd for it. */
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (flt && rt->kind == TY_FLOAT)            /* promoted to double */
        size = 8;
    {
        int addr = new_temp(fn);
        long step = byref ? 4 : (size + 3) & ~3L;
        emit_mov(fn, addr, cur);
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (byref) {
            /* the word is the copy's address */
            int p = emit_load(fn, addr, ptr);
            fn->ins[fn->nins - 1].natural = 1;
            if (sdst >= 0) {
                irg_va_copy(fn, sdst, 0, p, ty_size(rt));
                return sdst;
            }
            {
                int v = emit_load(fn, p, rt);
                fn->ins[fn->nins - 1].natural = 1;
                return v;
            }
        }
        if (flt) {
            int v = emit_load(fn, addr, ty_base(TY_DOUBLE, 0));
            fn->ins[fn->nins - 1].natural = 1;
            if (rt->kind == TY_FLOAT) {
                struct ir_ins *cv = emit(fn);
                cv->op = IR_F2F;
                cv->a = v;
                cv->size = 8;
                cv->w = 4;
                cv->dst = new_temp(fn);
                return cv->dst;
            }
            return v;
        }
        {
            int v = emit_load(fn, addr, rt);
            fn->ins[fn->nins - 1].natural = 1;
            return v;
        }
    }
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE, into register names, and
 * the result handed to src/arch/sparc/asm.c; the backend then only loads
 * the inputs, splices the bytes and stores the outputs -- Xtensa's split
 * (xtensa/irgen.c irg_asm_xtensa), operand for operand: a register
 * operand substitutes to `%o0`, an immediate to its value, and an "m"
 * operand -- a register holding the ADDRESS -- to `[%o0]`, as GCC prints
 * a SPARC memory operand.
 *
 * What the register windows change:
 *
 *   * %sp (%o6), %fp (%i6) and %i7 (the return address) are this frame's;
 *     an operand never goes there and a clobber list may not name them
 *     (EmbCC saves nothing around an asm). %g5-%g7 are the system's
 *     (the ABI reserves them) and %g1-%g4, %l6, %l7 and %o7 the code
 *     generator's scratch; no operand goes in any of them either.
 *   * The locals and the ins are this window's own, so a template may use
 *     any register freely as long as it says so: a value live across the
 *     asm keeps out of every register the asm changes (ir_asm.clob) --
 *     its operands', its clobbers' and every one the template names.
 *   * A call in the template (`call`, or a jmpl that links through %o7)
 *     changes the outs and %g1-%g4: those are added to what the asm
 *     changes, and no operand is put in them.
 */

/* The registers an operand may be given: the outs first (nothing lives
 * across a call in them), then the window's own locals and ins. Not
 * %o6/%o7, %l6/%l7, %i6/%i7 or a global. */
static const int sp_asm_pool[] = {
    SP_O0, SP_O1, SP_O2, SP_O3, SP_O4, SP_O5,
    SP_L0, SP_L1, SP_L2, SP_L3, SP_L4, SP_L5,
    SP_I0, SP_I1, SP_I2, SP_I3, SP_I4, SP_I5
};
#define SP_NPOOL ((int)(sizeof sp_asm_pool / sizeof sp_asm_pool[0]))

/* The registers the template names outright (`%o3`, or `%%o3` in an
 * extended asm's text): off limits to allocation, and changed as far as
 * anything outside knows. */
static void sp_mark_template_regs(const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n = 0, r;
        if (*p != '%')
            continue;
        if (p[1] == '%')
            p++;
        while (isalnum((unsigned char)p[1 + n]))
            n++;
        r = spasm_gpr(p + 1, n);
        if (r >= 0)
            used[r] = 1;
        p += n;
    }
}

static char *sp_subst(const char *file, int line, const char *tmpl,
                      const int *regs, const long *imms, const int *isimm,
                      const int *ismem, const char *const *names, int nops)
{
    size_t cap = strlen(tmpl) * 2 + 64, len = 0;
    char *out = xmalloc(cap);
    for (const char *p = tmpl; *p; ) {
        int mod = 0, k = -1;
        if (len + 40 >= cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        if (*p != '%') {
            out[len++] = *p++;
            continue;
        }
        p++;
        if (*p == '%') {
            out[len++] = '%';
            p++;
            continue;
        }
        /* %c0: a constant, bare; %r0: %g0 for a constant 0 (GCC's) */
        if ((*p == 'c' || *p == 'r') &&
            (isdigit((unsigned char)p[1]) || p[1] == '[')) {
            mod = *p;
            p++;
        }
        if (*p == '[') {
            const char *e = strchr(p, ']');
            if (!e)
                diag_fatal(file, line, "unterminated %%[name] in asm template");
            for (int i = 0; i < nops; i++)
                if (names[i] && (size_t)(e - p - 1) == strlen(names[i]) &&
                    strncmp(p + 1, names[i], (size_t)(e - p - 1)) == 0)
                    k = i;
            if (k < 0)
                diag_fatal(file, line, "asm template names an unknown operand "
                                       "'%.*s'", (int)(e - p - 1), p + 1);
            p = e + 1;
        } else if (isdigit((unsigned char)*p)) {
            k = 0;
            while (isdigit((unsigned char)*p))
                k = k * 10 + (*p++ - '0');
            if (k >= nops)
                diag_fatal(file, line, "asm template refers to operand %%%d, "
                                       "but there are only %d", k, nops);
        } else if (isalpha((unsigned char)*p) && (isdigit((unsigned char)p[1]) ||
                                                  p[1] == '[')) {
            diag_fatal(file, line, "asm template modifier '%%%c' is not "
                                   "supported for SPARC", *p);
        } else {
            /* `%o0`, `%hi(` and every other register or operator: the
             * text, as GCC passes it on */
            out[len++] = '%';
            continue;
        }
        if (mod == 'c' && !isimm[k])
            diag_fatal(file, line, "asm template modifier '%%c' needs a "
                                   "constant operand");
        if (isimm[k] && mod == 'r' && imms[k] == 0)
            len += (size_t)snprintf(out + len, cap - len, "%%g0");
        else if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else if (ismem[k])
            len += (size_t)snprintf(out + len, cap - len, "[%%%s]",
                                    sparc_reg_name(regs[k]));
        else
            len += (size_t)snprintf(out + len, cap - len, "%%%s",
                                    sparc_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_sparc(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    int ismem[2 * MAX_PARAMS], tie[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[32] = { 0 };
    int calls;

    /* Operands are numbered outputs first, then inputs, as gcc does. */
    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        const char *c = op->constraint;
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "SPARC", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        while (*c == '=' || *c == '+' || *c == '&')
            c++;
        tie[i] = -1;
        if (i >= a->nout && isdigit((unsigned char)*c)) {
            tie[i] = atoi(c);
            if (tie[i] >= a->nout || a->out[tie[i]].reg == ASM_REG_IMM ||
                asm_constraint_mem_only(a->out[tie[i]].constraint))
                diag_fatal(file, s->line, "asm input %d is tied to \"%s\", "
                           "which is not a register output", i - a->nout, c);
        }
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        ismem[i] = asm_constraint_mem_only(op->constraint);
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        if (!isimm[i] && !ismem[i] && sizes[i] > 4)
            diag_fatal(file, s->line, "SPARC asm operand %d is %d bytes; an "
                                      "operand is one 32-bit register (pass a "
                                      "64-bit value as two)", i, sizes[i]);
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        const char *c = a->clob[i];
        int r = spasm_gpr(c, (int)strlen(c));
        if (r == SP_SP || r == SP_FP || r == SP_I7)
            diag_fatal(file, s->line, "SPARC asm clobbers '%s', which holds "
                                      "this function's %s; EmbCC does not "
                                      "save it around an asm", c,
                       r == SP_SP ? "stack pointer"
                       : r == SP_FP ? "frame pointer" : "return address");
        if (r >= 0) {
            used[r] = 1;
            continue;
        }
        if (strcmp(c, "memory") && strcmp(c, "cc") && strcmp(c, "y") &&
            strcmp(c, "%y") && strcmp(c, "icc"))
            diag_fatal(file, s->line, "asm clobber '%s' is not a SPARC "
                                      "register (%%g0-%%i7, \"cc\", \"y\" or "
                                      "\"memory\")", c);
    }
    sp_mark_template_regs(a->tmpl, used);
    /* a call changes the outs and the scratch globals */
    calls = spasm_template_calls(a->tmpl);
    if (calls) {
        for (int r = SP_O0; r <= SP_O7; r++)
            used[r] = 1;
        for (int r = SP_G1; r <= SP_G4; r++)
            used[r] = 1;
    }

    for (int i = 0; i < nops; i++) {
        int r = -1;
        if (regs[i] != -2 || tie[i] >= 0)
            continue;
        for (int k = 0; k < SP_NPOOL; k++)
            if (!used[sp_asm_pool[k]]) {
                r = sp_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }
    for (int i = 0; i < nops; i++)
        if (tie[i] >= 0)
            regs[i] = regs[tie[i]];

    char *text = a->is_basic ? xstrndup(a->tmpl, strlen(a->tmpl))
                             : sp_subst(file, s->line, a->tmpl, regs, imms,
                                        isimm, ismem, names, nops);
    struct code c = { 0 };
    char err[512];
    if (spasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    free(text);

    /* Immediates were consumed by the template and carry no run-time
     * value, so only register operands become IR operands. */
    struct ir_asm *ia = xcalloc(1, sizeof *ia);
    ia->code = c.p;
    ia->codelen = c.len;
    ia->calls = calls;
    ia->out = xcalloc((size_t)(a->nout ? a->nout : 1), sizeof *ia->out);
    ia->in = xcalloc((size_t)(a->nin ? a->nin : 1), sizeof *ia->in);
    for (int i = 0; i < a->nin; i++) {
        if (isimm[a->nout + i])
            continue;
        struct ir_asm_op *o = &ia->in[ia->nin++];
        o->reg = regs[a->nout + i];
        /* An "m" operand names MEMORY: the register carries its ADDRESS
         * and the template dereferences it. */
        o->mem = asm_constraint_mem_only(a->in[i].constraint);
        o->temp = o->mem ? gen_addr(fn, a->in[i].expr)
                         : gen_expr(fn, a->in[i].expr);
        o->size = 4;                  /* a temp holds the promoted value */
    }
    /* An "=r" output of an integer or a pointer is a VALUE: the asm's
     * dst, stored to its lvalue afterwards (irg_asm_out_store). The first
     * is this asm's own dst; each further one is a continuation right
     * after it (ir_asm.cont). Anything else -- "+", "m", another type --
     * is written through the lvalue's address. */
    int vk[2 * MAX_PARAMS], vaddr[2 * MAX_PARAMS], nv = 0;
    for (int i = 0; i < a->nout; i++) {
        struct expr *lv = a->out[i].expr;
        int inout = strchr(a->out[i].constraint, '+') != NULL;
        int mem = asm_constraint_mem_only(a->out[i].constraint);
        if (!inout && !mem && irg_asm_val_ok(lv, 4)) {
            vaddr[nv] = irg_asm_out_addr(fn, lv);
            vk[nv++] = i;
            if (nv > 1)
                continue;                /* a continuation's */
        }
        struct ir_asm_op *o = &ia->out[ia->nout++];
        o->reg = regs[i];
        o->size = sizes[i];
        o->inout = inout;
        o->mem = mem;
        if (nv && vk[nv - 1] == i) {
            o->val = 1;
            o->temp = -1;
        } else {
            o->temp = gen_addr(fn, lv);
        }
    }
    /* What the asm may change: every register used[] holds (the
     * operands', the clobbers', the template's, a call's) and the
     * scratch an output through an address is stored with. A value live
     * across the asm keeps out of exactly these (regalloc.c). */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (int q = 0; q < SP_NPOOL; q++)
                if (!used[sp_asm_pool[q]]) {
                    ia->scr = sp_asm_pool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
    /* (bit 0, %g0, which nothing is ever allocated to, says "known":
     * a clob of 0 would mean "unknown", which the allocator treats as a
     * call) */
    ia->clob = 1;
    for (int r = 1; r < 32; r++)
        if (used[r])
            ia->clob |= 1UL << r;
    struct ir_ins *ins = emit(fn);
    ins->op = IR_ASM;
    ins->asm_ir = ia;
    ins->dst = nv ? new_temp(fn) : -1;
    ins->a = ins->b = -1;
    int vdst[2 * MAX_PARAMS];
    if (nv)
        vdst[0] = ins->dst;
    for (int k = 1; k < nv; k++) {
        struct ir_asm *c2 = xcalloc(1, sizeof *c2);
        c2->cont = 1;
        c2->in = xcalloc(1, sizeof *c2->in);
        c2->out = xcalloc(1, sizeof *c2->out);
        c2->nout = 1;
        c2->scr = -1;
        c2->out[0].reg = regs[vk[k]];
        c2->out[0].size = sizes[vk[k]];
        c2->out[0].val = 1;
        c2->out[0].temp = -1;
        struct ir_ins *ci = emit(fn);
        ci->op = IR_ASM;
        ci->asm_ir = c2;
        ci->dst = vdst[k] = new_temp(fn);
        ci->a = ci->b = -1;
    }
    for (int k = 0; k < nv; k++)
        irg_asm_out_store(fn, a->out[vk[k]].expr, vaddr[k], vdst[k]);
}

/* What codegen.c takes as an immediate without building it: a signed
 * 13-bit field for add, sub, and, or, xor, smul and a compare (subcc).
 * Here rather than beside the lowering because embls links the optimizer
 * without the code generator. */
int sparc_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
    case IR_MUL: case IR_CMP:
        return imm >= -4096 && imm <= 4095;
    default:
        return 0;
    }
}

/* A 64-bit AND/OR/XOR, which the code generator does half by half: each
 * half the identity, zero, all ones, or a signed 13-bit value. */
static int half_ok(unsigned long c)
{
    long s = (long)(int)(unsigned int)c;
    return c == 0 || c == 0xffffffffUL || (s >= -4096 && s <= 4095);
}

int sparc_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    return half_ok(u & 0xffffffffUL) && half_ok((u >> 32) & 0xffffffffUL);
}
