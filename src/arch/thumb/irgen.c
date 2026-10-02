/* ARMv7-M's variadic lowering (D-015).
 *
 * AAPCS32 passes a variadic argument exactly as it passes a named one —
 * r0-r3 and then the stack — so a `va_list` is a bare POINTER at the
 * next one, with no record to walk and no register-save offsets to
 * track. That is the same shape Darwin's aarch64 uses and the opposite
 * of SysV's and AAPCS64's, both of which point AT a record.
 *
 * The consequence for this file is that advancing the list means
 * writing the va_list VARIABLE, which is why it takes the variable's
 * address rather than its value.
 *
 * The prologue's half of the bargain is in codegen.c: a variadic
 * function pushes r0-r3 immediately below the caller's stack arguments,
 * so that one pointer walks from the registers straight into them.
 */
#include "../../ir/irgen_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asm.h"
#include "emit.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"

#include "../target.h"
#include "../../sema/type.h"

int irg_va_arg_thumb(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    /* The list itself is `char *`, and a pointer here is four bytes. */
    struct type *ptr = ty_base(TY_INT, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);
    long step;

    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);

    /* An eight-byte argument is eight-ALIGNED in the argument area, and
     * the caller aligned it the same way. A variadic `float` arrives
     * promoted to `double`, so it is eight of both. */
    if (flt && rt->kind == TY_FLOAT) {
        size = 8;
        align = 8;
    }
    if (align >= 8)
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
 * the result handed to src/arch/thumb/asm.c. The backend then only has to
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
 * name one. AAPCS32 has only r0-r3 caller-saved, and r12 is the ABI's
 * own scratch -- four and a bit, where RISC-V had fifteen. That is the
 * whole of what an asm may be handed without the prologue having to save
 * something, and running out is reported rather than worked around. */
static const int t_asm_pool[] = { 0, 1, 2, 3, 12 };

/* A register the template NAMES outright is off limits to allocation, and
 * one that is callee-saved is refused: EmbCC saves nothing around an asm,
 * so writing s0-s11 would corrupt the caller. */
static void t_mark_template_regs(const char *file, int line,
                                  const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; ) {
        if (isalpha((unsigned char)*p) &&
            (p == tmpl || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                            p[-1] == '%'))) {
            int n = 1;
            while (isalnum((unsigned char)p[n]) || p[n] == '_')
                n++;
            int r = tasm_gpr(p, n);
            if (r >= 0) {
                /* r4-r11 are callee-saved, and sp/lr/pc are not an
                 * asm's to take. */
                if (r >= 4 && r <= 11)
                    diag_fatal(file, line,
                               "ARMv7-M asm names callee-saved register "
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

static char *t_subst(const char *file, int line, const char *tmpl,
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
                                   "supported for ARMv7-M", *p ? *p : ' ');
        }
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else
            len += (size_t)snprintf(out + len, cap - len, "%s",
                                    t_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_thumb(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[16] = { 0 };

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
        int r = tasm_gpr(a->clob[i], (int)strlen(a->clob[i]));
        if (r >= 4 && r <= 11)
            diag_fatal(file, s->line, "ARMv7-M asm clobbers callee-saved "
                                      "register '%s', which EmbCC does not "
                                      "save around an asm", a->clob[i]);
        if (r >= 0)
            used[r] = 1;
    }
    t_mark_template_regs(file, s->line, a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        if (regs[i] != -2)
            continue;
        int r = -1;
        for (unsigned k = 0; k < sizeof t_asm_pool / sizeof t_asm_pool[0]; k++)
            if (!used[t_asm_pool[k]]) {
                r = t_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = t_subst(file, s->line, a->tmpl, regs, imms, isimm,
                          names, nops);
    struct code c = { 0 };
    char err[512];
    if (tasm_assemble(text, &c, err, sizeof err) != 0)
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
        o->size = 4;                  /* a temp holds the promoted value */
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

/* What the IR_ADD..IR_CMP lowerings in codegen.c encode as an immediate without
 * building the constant in a register first -- the answer the optimizer
 * asks for before folding one (target_imm_foldable). It must say what
 * those lowerings DO: anything else is folded and then rebuilt at each
 * use, which is exactly what asking avoids.
 *
 * HERE rather than beside them because the optimizer asks it, and embls
 * links the optimizer without the code generator. */
int thumb_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD: case IR_SUB:
        return (imm >= -4095 && imm <= 4095) || t_imm_ok(imm);
    case IR_AND:
        /* ...or a mask of the low bits, which is `ubfx rd, rn, #0, #n`
         * when the modified immediate cannot hold it: 0xfff is not one. */
        return t_imm_ok(imm) ||
               (imm > 0 && imm <= 0x7fffffffL && ((imm + 1) & imm) == 0);
    case IR_OR: case IR_XOR:
        return t_imm_ok(imm);
    case IR_CMP:
        return (imm >= 0 && imm <= 255) || t_imm_ok(imm);
    case IR_MUL: {
        /* No multiply-immediate, but a shifted-operand add or rsb (and a
         * shift) does 3, 5, 6, 7, 9, 10, 12, 15, 20, 24...: see the
         * codegen lowering. Anything else is a constant in a register. */
        int k, neg, j;
        return target_mul_shift_add(imm, &k, &neg, &j);
    }
    default:
        return 0;
    }
}
