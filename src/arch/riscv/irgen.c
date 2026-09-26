/* RISC-V's variadic lowering (D-016).
 *
 * The psABI passes a variadic argument in the same places a named one
 * goes -- a0-a7 and then the stack -- so a `va_list` is a bare POINTER
 * at the next one, with no record to walk and no register-save offsets
 * to track. That is the same shape AAPCS32 uses and the opposite of
 * SysV's and AAPCS64's, both of which point AT a record.
 *
 * It is NOT shared with the ARMv7-M lowering even though the two come
 * out looking alike. The resemblance is a coincidence of two ABIs
 * choosing the same representation at one width: this one's step and
 * alignment are XLEN and 2*XLEN, which differ between RV32 and RV64,
 * where AAPCS32's are fixed at 4 and 8. A shared routine would have to
 * be parameterised by both and would still be one place where a change
 * for one target silently moved the other.
 *
 * The ONE rule here that is not the obvious one: a variadic argument of
 * 2*XLEN bytes is aligned to 2*XLEN, where a FIXED argument of the same
 * type is not aligned at all. The caller does the same thing on its side
 * (place_arg's `variadic` flag), and the two have to agree or a
 * `long long` after an odd number of words is read one word early.
 *
 * The prologue's half of the bargain is in codegen.c: a variadic
 * function spills a0-a7 immediately below the caller's stack arguments,
 * so one pointer walks from the registers straight into them.
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

int irg_va_arg_riscv(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    int wb = target_ptr_size();                 /* XLEN in bytes */
    /* The list itself is a `char *`, so the cell holding it is one
     * pointer wide -- four bytes at RV32 and eight at RV64. */
    struct type *ptr = ty_int_of_size(wb, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);
    long step;

    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);

    /* A variadic `float` arrives promoted to `double`, so it is eight
     * bytes of both size and alignment however it was written. */
    if (flt && rt->kind == TY_FLOAT) {
        size = 8;
        align = 8;
    }
    if (align >= 2 * wb) {
        long a = 2 * (long)wb;
        cur = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, cur, emit_const(fn, a - 1, wb),
                                wb, 1),
                       emit_const(fn, -a, wb), wb, 1);
    }

    {
        int addr = new_temp(fn);
        emit_mov(fn, addr, cur);
        step = (size + wb - 1) & ~(long)(wb - 1);
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, wb),
                            wb, 1),
                   ptr);

        if (flt) {
            /* Read the double that was passed, then narrow if the
             * program asked for a float. */
            int v = emit_load(fn, addr, ty_base(TY_DOUBLE, 0));
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
        return emit_load(fn, addr, rt);
    }
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE, into register names, and
 * the result handed to src/arch/riscv/asm.c. The backend then only has to
 * load the inputs, emit the bytes and store the outputs -- it never sees
 * text. That split is the aarch64 backend's (src/arch/aarch64/irgen.c),
 * and it is worth keeping identical: the two then share the operand
 * semantics, and only the vocabulary differs.
 *
 * RISC-V has no %w/%x modifier question. Every register has one name at
 * one width, so an operand substitutes to `a0` whatever its type -- where
 * aarch64 must choose between w0 and x0 and gets it from the operand's
 * size. One less thing to get wrong; the compiler's own zero- and
 * sign-extension rules still apply to what it put in the register.
 */

/* Registers an asm operand may be given when the constraint does not
 * name one. Caller-saved and not an argument register, so nothing the
 * ABI cares about is disturbed: t0-t6 in order, then the argument file,
 * which is caller-saved too. */
static const int rv_asm_pool[] = {
    RV_T0, RV_T1, RV_T2, RV_T3, RV_T4, RV_T5, RV_T6,
    RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7
};

/* A register the template NAMES outright is off limits to allocation, and
 * one that is callee-saved is refused: EmbCC saves nothing around an asm,
 * so writing s0-s11 would corrupt the caller. */
static void rv_mark_template_regs(const char *file, int line,
                                  const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; ) {
        if (isalpha((unsigned char)*p) &&
            (p == tmpl || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                            p[-1] == '%'))) {
            int n = 1;
            while (isalnum((unsigned char)p[n]) || p[n] == '_')
                n++;
            int r = rvasm_gpr(p, n);
            if (r >= 0) {
                /* s0-s11: x8, x9 and x18-x27. */
                if (r == 8 || r == 9 || (r >= 18 && r <= 27))
                    diag_fatal(file, line,
                               "RISC-V asm names callee-saved register "
                               "'%.*s', which EmbCC does not save around an "
                               "asm", n, p);
                used[r] = 1;
            }
            p += n;
            continue;
        }
        p++;
    }
}

static char *rv_subst(const char *file, int line, const char *tmpl,
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
                                   "supported for RISC-V", *p ? *p : ' ');
        }
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else
            len += (size_t)snprintf(out + len, cap - len, "%s",
                                    rv_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_riscv(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[32] = { 0 };

    /* Operands are numbered outputs first, then inputs, as gcc does. */
    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "RISC-V", op->constraint);
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
        int r = rvasm_gpr(a->clob[i], (int)strlen(a->clob[i]));
        if (r == 8 || r == 9 || (r >= 18 && r <= 27))
            diag_fatal(file, s->line, "RISC-V asm clobbers callee-saved "
                                      "register '%s', which EmbCC does not "
                                      "save around an asm", a->clob[i]);
        if (r >= 0)
            used[r] = 1;
    }
    rv_mark_template_regs(file, s->line, a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        if (regs[i] != -2)
            continue;
        int r = -1;
        for (unsigned k = 0; k < sizeof rv_asm_pool / sizeof rv_asm_pool[0]; k++)
            if (!used[rv_asm_pool[k]]) {
                r = rv_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = rv_subst(file, s->line, a->tmpl, regs, imms, isimm,
                          names, nops);
    struct code c = { 0, 0, 0 };
    char err[512];
    if (rvasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    free(text);

    /* Immediates were consumed by the template and carry no run-time
     * value, so only register operands become IR operands. */
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
        /* An "m" operand names MEMORY: the register carries its ADDRESS
         * and the template dereferences it. */
        o->mem = strchr(a->in[i].constraint, 'm') != NULL;
        o->temp = o->mem ? gen_addr(fn, a->in[i].expr)
                         : gen_expr(fn, a->in[i].expr);
        o->size = target_ptr_size();  /* a temp holds the promoted value */
    }
    for (int i = 0; i < a->nout; i++) {
        struct ir_asm_op *o = &ia->out[ia->nout++];
        o->reg = regs[i];
        o->temp = gen_addr(fn, a->out[i].expr);
        o->size = sizes[i];
        o->inout = strchr(a->out[i].constraint, '+') != NULL;
        o->mem = strchr(a->out[i].constraint, 'm') != NULL;
    }
    struct ir_ins *ins = emit(fn);
    ins->op = IR_ASM;
    ins->asm_ir = ia;
    ins->dst = -1;
    ins->a = ins->b = -1;
}
