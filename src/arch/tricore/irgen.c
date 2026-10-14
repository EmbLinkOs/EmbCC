/* TriCore's share of IR generation: va_arg, inline asm, and
 * which constants the optimizer may fold into an instruction.
 *
 * The TriCore EABI (as remembered, docs/internals/tricore-plan.md) passes
 * every UNNAMED argument of a variadic call on the stack, in whole words
 * from the caller's stack pointer, after any named ones there. So a
 * va_list is a bare pointer walking those words: va_arg reads at it and
 * steps by the size in whole words -- an 8-byte value is only 4-aligned
 * -- a `float` arrives as a `double`, and a struct larger than 8 bytes
 * arrives as the address of the caller's copy.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "asm.h"
#include "emit.h"
#include "../../sema/type.h"
#include "../../driver/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int irg_va_arg_tricore(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    int byref = rt->kind == TY_STRUCT && size > 8;
    long step;

    /* Every access here is natural (ir_ins.natural): the va_list is a
     * pointer object, as aligned as its lvalue is, and each argument
     * slot is whole words. */
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (flt && rt->kind == TY_FLOAT)            /* promoted to double */
        size = 8;
    {
        int addr = new_temp(fn);
        emit_mov(fn, addr, cur);
        step = byref ? 4 : (size + 3) & ~3L;
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (sdst >= 0) {
            int src = addr;
            if (byref) {
                src = emit_load(fn, addr, ptr);
                fn->ins[fn->nins - 1].natural = 1;
            }
            irg_va_copy(fn, sdst, 0, src, ty_size(rt));
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
 * The template's operands are substituted here into register names (d4,
 * a5, ...) and the text is assembled by tricore/asm.c; the backend loads
 * the inputs, splices the bytes and stores the outputs. An operand of
 * constraint "d" or "r" takes a DATA register, one of "a" or "m" an
 * ADDRESS register ("m": it holds the lvalue's address, written [%0]);
 * they come from the lower context, D2-D7 then D0-D1 and A2-A7, which no
 * caller keeps anything in across an asm: the allocator keeps every value
 * live across one in memory. In the unified numbering of ir_asm_op.reg an
 * address register n is 16 + n. A template or clobber may not name the
 * stack pointer, the return address or the system's global address
 * registers (A0, A1, A8, A9), nor A12-A15, the backend's own scratch. */
static const int tc_asm_dpool[] = { 2, 3, 4, 5, 6, 7, 0, 1 };
static const int tc_asm_apool[] = { 2, 3, 4, 5, 6, 7 };

static int forbidden_areg(int r)
{
    return r == 0 || r == 1 || r == 8 || r == 9 || r >= 10;
}

static void mark_reg(const char *file, int line, const char *p, int n,
                     int *used, const char *what)
{
    int f, r = tcasm_reg(p, n, &f);
    if (r < 0)
        return;
    if (f == 'a' && forbidden_areg(r))
        diag_fatal(file, line, "TriCore asm %s register '%.*s', which EmbCC "
                   "keeps for itself (the stack pointer, the return address, "
                   "the system's global address registers or its own "
                   "scratch)", what, n, p);
    if (f == 'e') {
        used[r] = used[r + 1] = 1;
    } else {
        used[f == 'a' ? 16 + r : r] = 1;
    }
}

static void mark_template_regs(const char *file, int line, const char *tmpl,
                               int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n = 0;
        if (*p == '%' && (p[1] == '%' || isdigit((unsigned char)p[1]) ||
                          p[1] == '['))
            continue;
        if (!isalpha((unsigned char)*p) ||
            (p > tmpl && (isalnum((unsigned char)p[-1]) || p[-1] == '.')))
            continue;
        while (isalnum((unsigned char)p[n]))
            n++;
        mark_reg(file, line, p, n, used, "names");
        p += n - 1;
    }
}

static char *tc_subst(const char *file, int line, const char *tmpl,
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
        /* %c0: the constant alone, as gcc prints it */
        int bare = 0;
        if (*p == 'c' && (p[1] == '[' || isdigit((unsigned char)p[1]))) {
            bare = 1;
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
        } else {
            diag_fatal(file, line, "asm template modifier '%%%c' is not "
                                   "supported for TriCore", *p ? *p : ' ');
        }
        if (bare && !isimm[k])
            diag_fatal(file, line, "%%c%d names a register operand; %%c "
                       "prints a constant, and wants an \"i\" or \"n\" "
                       "operand", k);
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else
            len += (size_t)snprintf(out + len, cap - len, "%s",
                                    regs[k] >= 16 ? tc_reg_name('a', regs[k] - 16)
                                                  : tc_reg_name('d', regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_tricore(struct ir_func *fn, struct stmt *s)
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
                                      "TriCore", op->constraint);
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
    for (int i = 0; i < a->nclob; i++)
        mark_reg(file, s->line, a->clob[i], (int)strlen(a->clob[i]), used,
                 "clobbers");
    mark_template_regs(file, s->line, a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        int r = -1;
        if (regs[i] == -2) {
            for (unsigned k = 0; k < sizeof tc_asm_dpool / sizeof tc_asm_dpool[0];
                 k++)
                if (!used[tc_asm_dpool[k]]) {
                    r = tc_asm_dpool[k];
                    break;
                }
        } else if (regs[i] == -3) {
            for (unsigned k = 0; k < sizeof tc_asm_apool / sizeof tc_asm_apool[0];
                 k++)
                if (!used[16 + tc_asm_apool[k]]) {
                    r = 16 + tc_asm_apool[k];
                    break;
                }
        } else {
            continue;
        }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = tc_subst(file, s->line, a->tmpl, regs, imms, isimm,
                          names, nops);
    struct code c = { 0 };
    char err[512];
    if (tcasm_assemble(text, &c, err, sizeof err) != 0)
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

/* What codegen.c takes as an immediate without building it: ADDI's
 * SIGNED 16 bits for add (and a subtraction is an add of the negation);
 * the RC form's const9 -- ZERO-extended for AND, OR and XOR, signed for
 * MUL; for a compare, what fits every form a compare can take, the RC
 * field signed or unsigned and the k + 1 a GT or LE becomes. Anything
 * else stays in a register, built once and hoistable, rather than
 * rebuilt at each use. Here rather than beside the lowering because embls
 * links the optimizer without the code generator. */
int tc_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD:
        return imm >= -32768 && imm <= 32767;
    case IR_SUB:
        return imm >= -32767 && imm <= 32768;
    case IR_AND: case IR_OR: case IR_XOR:
        return imm >= 0 && imm <= 511;
    case IR_MUL:
        return imm >= -256 && imm <= 255;
    case IR_CMP:
        return imm >= 0 && imm <= 254;
    default:
        return 0;
    }
}
