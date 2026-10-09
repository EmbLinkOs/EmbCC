/* LoongArch64's share of irgen: the immediates the optimizer may fold,
 * and inline assembly.
 *
 * The variadic lowering is RISC-V's (irg_va_arg_riscv, src/arch/riscv/
 * irgen.c): the LoongArch psABI passes a variadic argument exactly where
 * RV64's LP64 does -- a0-a7 and then the stack, a 2*GRLEN-aligned scalar
 * in an even-aligned register pair, more than two registers by
 * reference -- and its va_list is the same bare pointer, so that routine,
 * which reads only the pointer width, is right here too (read off clang:
 * `v(0, x)` with an __int128 x puts x in a2:a3). */
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

/* What the IR_ADD..IR_CMP lowerings in codegen.c take as an immediate
 * without building the constant first -- the optimizer asks before
 * folding one (src/opt/immfold.c's target_imm_foldable). Anything else is folded and
 * then rebuilt at every use, where a value left in a register is built
 * once and can be hoisted out of a loop.
 *
 * add takes a SIGNED 12-bit immediate and sub is an add of the negation;
 * and, or and xor take an UNSIGNED one, 0..4095 -- the difference from
 * RISC-V that matters here. A compare's constant is free when it is zero
 * (r0); mul has no immediate at all. HERE rather than beside the
 * lowerings because embls links the optimizer without the code
 * generator. */
int la_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD:
        return imm >= -2048 && imm <= 2047;
    case IR_SUB:
        return imm >= -2047 && imm <= 2048;
    case IR_AND: case IR_OR: case IR_XOR:
        return imm >= 0 && imm <= 4095;
    case IR_CMP:
        return imm == 0;
    default:
        return 0;
    }
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE, into register names, and
 * the result handed to src/arch/loongarch/asm.c; the backend then only
 * loads the inputs, splices the bytes and stores the outputs. The same
 * split as RISC-V's (src/arch/riscv/irgen.c irg_asm_riscv), whose shape
 * this follows operand for operand: a register operand substitutes to
 * `$a0`, an immediate to its value, and an "m" operand -- a register
 * holding the ADDRESS -- to `$a0, 0`, the base-and-offset pair a LoongArch
 * load or store takes, as GCC prints it.
 */

/* Registers an asm operand may be given when the constraint does not
 * name one: caller-saved and not an argument register first, t0-t8, then
 * the argument file, which is caller-saved too. */
static const int la_asm_pool[] = {
    LA_T0, LA_T1, LA_T2, LA_T3, LA_T4, LA_T5, LA_T6, LA_T7, LA_T8,
    LA_A0, LA_A1, LA_A2, LA_A3, LA_A4, LA_A5, LA_A6, LA_A7
};

static int la_callee_saved_reg(int r) { return r >= LA_FP && r <= LA_S8; }

/* A register the template NAMES outright is off limits to allocation, and
 * one that is callee-saved is refused: EmbCC saves nothing around an asm,
 * so writing fp or s0-s8 would corrupt the caller. tp and r21 belong to
 * the psABI, and sp and ra to the frame. */
static void la_mark_template_regs(const char *file, int line,
                                  const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n = 1;
        if (*p != '$')
            continue;
        while (isalnum((unsigned char)p[n]) || p[n] == '_')
            n++;
        int r = laasm_gpr(p, n);
        if (r >= 0) {
            if (la_callee_saved_reg(r))
                diag_fatal(file, line,
                           "LoongArch asm names callee-saved register "
                           "'%.*s', which EmbCC does not save around an "
                           "asm", n, p);
            if (r == LA_TP || r == LA_R21)
                diag_fatal(file, line,
                           "LoongArch asm names '%.*s', which the psABI "
                           "reserves", n, p);
            used[r] = 1;
        }
        p += n - 1;
    }
}

static char *la_subst(const char *file, int line, const char *tmpl,
                      const int *regs, const long *imms, const int *isimm,
                      const int *ismem, const char *const *names, int nops)
{
    size_t cap = strlen(tmpl) * 2 + 64, len = 0;
    char *out = xmalloc(cap);
    for (const char *p = tmpl; *p; ) {
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
                                   "supported for LoongArch", *p ? *p : ' ');
        }
        if (bare && !isimm[k])
            diag_fatal(file, line, "%%c%d names a register operand; %%c "
                       "prints a constant, and wants an \"i\" or \"n\" "
                       "operand", k);
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else if (ismem[k])
            len += (size_t)snprintf(out + len, cap - len, "$%s, 0",
                                    la_reg_name(regs[k]));
        else
            len += (size_t)snprintf(out + len, cap - len, "$%s",
                                    la_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

/* Does the template call or trap -- bl, b, jirl, jr, call36, tail36,
 * syscall, break -- so that it changes what a call changes, whatever its
 * clobber list says? (b and jr leave; a template that leaves and comes
 * back is taken to have called.) */
static int la_template_calls(const char *text)
{
    static const char *const calls[] = {
        "bl", "b", "jirl", "jr", "call36", "tail36", "syscall", "break", NULL
    };
    for (const char *p = text; *p; p++) {
        if (!isalpha((unsigned char)*p) ||
            (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                          p[-1] == '.' || p[-1] == '$' || p[-1] == '%')))
            continue;
        int n = 0;
        while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.')
            n++;
        for (int k = 0; calls[k]; k++)
            if ((int)strlen(calls[k]) == n && !strncmp(p, calls[k], (size_t)n))
                return 1;
        p += n - 1;
    }
    return 0;
}

void irg_asm_loongarch(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    int ismem[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[32] = { 0 };

    /* Operands are numbered outputs first, then inputs, as gcc does. */
    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "LoongArch", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        ismem[i] = asm_constraint_mem_only(op->constraint);
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        int r = laasm_gpr(a->clob[i], (int)strlen(a->clob[i]));
        if (r >= 0 && la_callee_saved_reg(r))
            diag_fatal(file, s->line, "LoongArch asm clobbers callee-saved "
                                      "register '%s', which EmbCC does not "
                                      "save around an asm", a->clob[i]);
        if (r >= 0)
            used[r] = 1;
    }
    la_mark_template_regs(file, s->line, a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        if (regs[i] != -2)
            continue;
        int r = -1;
        for (unsigned k = 0; k < sizeof la_asm_pool / sizeof la_asm_pool[0];
             k++)
            if (!used[la_asm_pool[k]]) {
                r = la_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = la_subst(file, s->line, a->tmpl, regs, imms, isimm, ismem,
                          names, nops);
    struct code c = { 0 };
    char err[512];
    if (laasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    /* Its alignments: to four at most, which every instruction has, so
     * they pad as at the template's start. A larger one would pad by where
     * the template lands, which this backend does not do yet. */
    {
        int open = code_asm_settle(&c, 4, CODE_FILL_ZERO, err, sizeof err);
        if (open < 0)
            diag_fatal(file, s->line, "%s", err);
        if (open)
            diag_fatal(file, s->line, "the asm aligns to %d bytes, which "
                       "LoongArch inline asm does not do yet: only alignment to "
                       "4 bytes, which every instruction has", open);
    }
    int calls = la_template_calls(text);
    free(text);

    /* Immediates were consumed by the template and carry no run-time
     * value, so only register operands become IR operands. */
    struct ir_asm *ia = xcalloc(1, sizeof *ia);
    ia->code = c.p;
    ia->codelen = c.len;
    ia->drange = c.drange;
    ia->ndrange = c.ndrange;
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
        o->size = target_ptr_size();  /* a temp holds the promoted value */
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
        if (!inout && !mem && irg_asm_val_ok(lv, target_ptr_size())) {
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
     * operands', the clobbers', the template's), the scratch an output
     * through an address is stored with, and, if the template calls,
     * everything a call changes -- ra, a0-a7 and t0-t8. A value live
     * across the asm keeps out of exactly these (regalloc.c). */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (unsigned q = 0;
                 q < sizeof la_asm_pool / sizeof la_asm_pool[0]; q++)
                if (!used[la_asm_pool[q]]) {
                    ia->scr = la_asm_pool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
    if (calls)
        for (int r = LA_RA; r <= LA_T8; r++)
            if (r != LA_TP && r != LA_SP)
                used[r] = 1;
    for (int r = 0; r < 32; r++)
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
