/* MIPS32 o32's share of IR generation: va_arg, inline-asm operands, and
 * which constants the optimizer may fold into an instruction.
 *
 * o32 passes a variadic argument exactly where a named one would go --
 * in a block whose first 16 bytes ride in a0-a3 -- and a variadic callee
 * stores a0-a3 into the home area its caller reserved, so the whole
 * argument list is one block in memory and a va_list is a bare pointer
 * walking it (codegen.c's prologue). va_arg rounds the pointer up to 8
 * for an 8-byte-aligned type, reads, and steps by the size in whole
 * words. A `float` arrives as a `double`; a struct of any size is its own
 * bytes, by value -- there is no by-reference rule in o32.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "asm.h"
#include "emit.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* n64 (MIPS64): every argument takes whole doublewords, named or not, and
 * a variadic callee spills a1..a7 just below its incoming stack words,
 * so the list is again one block and va_list a bare pointer. A type
 * aligned to 16 (a long double, an aligned struct) starts at an even
 * doubleword -- clang's va_arg rounds the pointer up to 16 for it, and
 * the saved registers are 16-aligned where an even one is. A scalar
 * narrower than eight bytes was passed extended to the whole doubleword,
 * so big-endian its bytes are the slot's LAST ones; a struct is its own
 * bytes from the slot's start in either order. */
static int va_arg_n64(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    struct type *ptr = ty_int_of_size(8, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);
    long step;
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    int sdst;

    irg_mark_natural(fn, e->lhs);
    sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;
    if (flt && rt->kind == TY_FLOAT)            /* promoted to double */
        size = align = 8;
    if (align >= 16)
        cur = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, cur, emit_const(fn, 15, 8), 8, 1),
                       emit_const(fn, -16, 8), 8, 1);
    {
        int addr = new_temp(fn), at;
        emit_mov(fn, addr, cur);
        step = (size + 7) & ~7L;
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, 8), 8, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (sdst >= 0) {
            irg_va_copy(fn, sdst, 0, addr, ty_size(rt));
            return sdst;
        }
        if (flt && rt->kind == TY_FLOAT) {
            int v = emit_load(fn, addr, ty_base(TY_DOUBLE, 0));
            struct ir_ins *cv;
            fn->ins[fn->nins - 1].natural = 1;
            cv = emit(fn);
            cv->op = IR_F2F;
            cv->a = v;
            cv->size = 8;
            cv->w = 4;
            cv->dst = new_temp(fn);
            return cv->dst;
        }
        at = addr;
        if (target_big_endian() && size < 8)
            at = emit_bin(fn, IR_ADD, addr, emit_const(fn, 8 - size, 8), 8, 1);
        {
            int v = emit_load(fn, at, rt);
            fn->ins[fn->nins - 1].natural = 1;
            return v;
        }
    }
}

int irg_va_arg_mips(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);
    long step;

    if (target_get() == TARGET_MIPS64)
        return va_arg_n64(fn, e);
    /* Every access here is natural (ir_ins.natural), so no lwl/lwr: the
     * va_list is a pointer object, as aligned as its lvalue is, and each
     * argument slot is a whole word, or eight bytes rounded to 8. */
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (flt && rt->kind == TY_FLOAT) {          /* promoted to double */
        size = 8;
        align = 8;
    }
    if (align > 8)
        align = 8;
    if (align == 8)
        cur = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, cur, emit_const(fn, 7, 4), 4, 1),
                       emit_const(fn, -8, 4), 4, 1);
    {
        int addr = new_temp(fn);
        emit_mov(fn, addr, cur);
        step = (size + 3) & ~3L;
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (sdst >= 0) {
            irg_va_copy(fn, sdst, 0, addr, ty_size(rt));
            return sdst;
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

/* ---- inline assembly -----------------------------------------------------
 *
 * The template's operands are substituted here into register names
 * ($t0, $a1, ...) and the text is assembled by mips/asm.c; the backend
 * loads the inputs, splices the bytes and stores the outputs. Operands
 * that name no register come from t0-t9, then v0-v1 and a0-a3: all
 * caller-saved, and nothing is live in a register across an asm. */
static const int mips_asm_pool[] = {
    MIPS_T0, MIPS_T1, MIPS_T2, MIPS_T3, MIPS_T4, MIPS_T5, MIPS_T6,
    MIPS_T7, MIPS_T8, MIPS_T9, MIPS_V0, MIPS_V1,
    MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3
};

/* s0-s7, fp, gp, sp, ra and the kernel's k0/k1: EmbCC saves nothing
 * around an asm, so a template may not write them. at is allowed: the
 * code around an asm keeps nothing in it. */
static int callee_owned(int r)
{
    return (r >= 16 && r <= 23) || r >= 26;
}

static void mark_template_regs(const char *file, int line, const char *tmpl,
                               int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n;
        if (*p != '$')
            continue;
        n = 1;
        while (isalnum((unsigned char)p[n]))
            n++;
        {
            int r = mipsasm_gpr(p, n);
            if (r >= 0) {
                if (callee_owned(r) && r != MIPS_SP)
                    diag_fatal(file, line,
                               "MIPS asm names register '%.*s', which EmbCC "
                               "does not save around an asm", n, p);
                used[r] = 1;
            }
        }
        p += n - 1;
    }
}

static char *mips_subst(const char *file, int line, const char *tmpl,
                        const int *regs, const long *imms, const int *isimm,
                        const char *const *names, int nops)
{
    size_t cap = strlen(tmpl) * 2 + 64, len = 0;
    char *out = xmalloc(cap);
    for (const char *p = tmpl; *p; ) {
        if (len + 32 >= cap) {
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
        int k = -1;
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
        } else {
            diag_fatal(file, line, "asm template modifier '%%%c' is not "
                                   "supported for MIPS", *p ? *p : ' ');
        }
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else
            len += (size_t)snprintf(out + len, cap - len, "$%s",
                                    mips_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_mips(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[32] = { 0 };

    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "MIPS", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        int r = mipsasm_gpr(a->clob[i], (int)strlen(a->clob[i]));
        if (r >= 0 && callee_owned(r))
            diag_fatal(file, s->line, "MIPS asm clobbers register '%s', "
                                      "which EmbCC does not save around an "
                                      "asm", a->clob[i]);
        if (r >= 0)
            used[r] = 1;
    }
    mark_template_regs(file, s->line, a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        if (regs[i] != -2)
            continue;
        int r = -1;
        for (unsigned k = 0; k < sizeof mips_asm_pool / sizeof mips_asm_pool[0];
             k++)
            if (!used[mips_asm_pool[k]]) {
                r = mips_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = mips_subst(file, s->line, a->tmpl, regs, imms, isimm,
                            names, nops);
    struct code c = { 0 };
    char err[512];
    if (mipsasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    free(text);

    struct ir_asm *ia = xcalloc(1, sizeof *ia);
    ia->code = c.p;
    ia->codelen = c.len;
    ia->out = xcalloc((size_t)(a->nout ? a->nout : 1), sizeof *ia->out);
    ia->in = xcalloc((size_t)(a->nin ? a->nin : 1), sizeof *ia->in);
    for (int i = 0; i < a->nin; i++) {
        if (isimm[a->nout + i])
            continue;
        struct ir_asm_op *o = &ia->in[ia->nin++];
        o->reg = regs[a->nout + i];
        o->mem = asm_constraint_mem_only(a->in[i].constraint);
        o->temp = o->mem ? gen_addr(fn, a->in[i].expr)
                         : gen_expr(fn, a->in[i].expr);
        o->size = 4;
    }
    for (int i = 0; i < a->nout; i++) {
        struct ir_asm_op *o = &ia->out[ia->nout++];
        o->reg = regs[i];
        o->temp = gen_addr(fn, a->out[i].expr);
        o->size = sizes[i];
        o->inout = strchr(a->out[i].constraint, '+') != NULL;
        o->mem = asm_constraint_mem_only(a->out[i].constraint);
    }
    struct ir_ins *ins = emit(fn);
    ins->op = IR_ASM;
    ins->asm_ir = ia;
    ins->dst = -1;
    ins->a = ins->b = -1;
}

/* What codegen.c takes as an immediate without building it: addiu's
 * SIGNED 16 bits for add (and a subtraction is an add of the negation),
 * andi/ori/xori's UNSIGNED 16 bits, and for a compare only zero -- a
 * branch compares two registers, and $zero is one. Anything else stays in
 * a register, built once and hoistable, rather than rebuilt at each use.
 * Here rather than beside the lowering because embls links the optimizer
 * without the code generator. */
int mips_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD:
        return imm >= -32768 && imm <= 32767;
    case IR_SUB:
        return imm >= -32767 && imm <= 32768;
    case IR_AND: case IR_OR: case IR_XOR:
        return imm >= 0 && imm <= 0xffff;
    case IR_CMP:
        return imm == 0;
    default:
        return 0;
    }
}

/* A 64-bit AND/OR/XOR, which the code generator does half by half
 * (logic_half): a half it takes without building is the identity, zero
 * or all ones, a 16-bit unsigned immediate, or for an AND a run of low
 * bits (ext) or all but a run of low bits (ins). Both halves must be --
 * or one half is the identity, and the other is built in a scratch. */
static int half_ok(int op, unsigned long c)
{
    unsigned long nc = ~c & 0xffffffffUL;
    if (c == 0 || c == 0xffffffffUL || c <= 0xffff)
        return 1;
    return op == IR_AND && ((c & (c + 1)) == 0 || (nc & (nc + 1)) == 0);
}

int mips_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    unsigned long lo = u & 0xffffffffUL, hi = (u >> 32) & 0xffffffffUL;
    unsigned long idn = op == IR_AND ? 0xffffffffUL : 0;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    if (lo == idn || hi == idn)
        return 1;
    return half_ok(op, lo) && half_ok(op, hi);
}
