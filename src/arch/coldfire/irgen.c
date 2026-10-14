/* ColdFire's share of IR generation: va_arg, which constants the optimizer
 * may fold into an instruction, and inline asm.
 *
 * The m68k convention passes every argument on the stack in whole words,
 * the unnamed ones of a variadic call exactly as named ones, so a va_list
 * is a `char *` walking the caller's argument words: each argument takes
 * its size rounded up to a word (a long long or double two), and one
 * smaller than a word -- only a composite, since everything narrower than
 * int is promoted -- is right-justified in its word, big-endian
 * (docs/internals/coldfire-plan.md).
 */
#include "../../ir/irgen_int.h"
#include "asm.h"
#include "emit.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int irg_va_arg_coldfire(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    long size = ty_size(rt);
    long words = (size + 3) & ~3L;
    struct type *ptr = ty_int_of_size(4, 1);
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    int addr = new_temp(fn);

    if (size > 8 && rt->kind != TY_STRUCT)
        diag_fatal(fn->file, e->line, "va_arg of a %ld-byte scalar is not "
                   "supported for %s", size, target_triple_now());
    fn->ins[fn->nins - 1].natural = 1;
    /* the value's first byte: right-justified when narrower than a word */
    emit_mov(fn, addr, size < 4 ? emit_bin(fn, IR_ADD, cur,
                                           emit_const(fn, 4 - size, 4), 4, 1)
                                : cur);
    emit_store(fn, apa,
               emit_bin(fn, IR_ADD, cur, emit_const(fn, words, 4), 4, 1),
               ptr);
    fn->ins[fn->nins - 1].natural = 1;
    if (rt->kind == TY_STRUCT) {        /* its bytes, into its own slot */
        int dst = irg_va_struct_slot(fn, e);
        irg_va_copy(fn, dst, 0, addr, size);
        return dst;
    }
    return emit_load(fn, addr, rt);
}

/* Every 32-bit constant is an operand on ColdFire -- addi, subi, andi,
 * ori, eori and cmpi take #imm32 into a data register, and the code
 * generator builds a multiplier in d1 -- and a 64-bit AND/OR/XOR/ADD/SUB
 * takes each half as one too. Here rather than beside the lowering
 * because embls links the optimizer without the code generator. */
int cf_imm_foldable(int op, long imm, int w)
{
    (void)imm;
    if (w == 8)
        return op == IR_AND || op == IR_OR || op == IR_XOR ||
               op == IR_ADD || op == IR_SUB || op == IR_CMP;
    return op == IR_ADD || op == IR_SUB || op == IR_AND || op == IR_OR ||
           op == IR_XOR || op == IR_CMP || op == IR_MUL;
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE, as GCC's m68k port prints
 * them in Motorola syntax, and the result handed to src/arch/coldfire/
 * asm.c; the backend then only moves the inputs into their registers,
 * splices the bytes and moves the outputs out (coldfire/codegen.c IR_ASM)
 * -- RX's and Xtensa's split. A data-register operand substitutes to
 * `%d2`, an address-register one to `%a2`, an immediate to `#5`, and an
 * "m" operand -- an address register holding the ADDRESS -- to `(%a2)`.
 *
 * What the m68k convention changes:
 *
 *   * a6 is the frame pointer and a7 the stack pointer: never an operand's
 *     register, and a clobber list may not name them.
 *   * d0, d1, a0 and a1 are call-clobbered, d2-d7 and a2-a5 callee-saved.
 *     A template may change any register it says it does -- its clobbers,
 *     the registers it names -- and a value live across it keeps out of
 *     those; a callee-saved one among them is saved by the prologue.
 *   * Operands take d0, d1 then d2-d7 (data), a0 then a2-a5 (address):
 *     not a1, which the lowering keeps for its own moves.
 */
static const int cf_asm_dpool[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const int cf_asm_apool[] = { 8, 10, 11, 12, 13 };

static int cf_word_start(const char *text, const char *p)
{
    return p == text || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                          p[-1] == '.');
}

/* The registers the template names outright, with or without a `%`. */
static void cf_mark_template_regs(const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n = 0;
        if (!cf_word_start(tmpl, p) || (!isalpha((unsigned char)*p) &&
                                        *p != '%'))
            continue;
        if (*p == '%')
            n = 1;
        while (isalnum((unsigned char)p[n]) || p[n] == '_')
            n++;
        {
            int r = cfasm_gpr(p, n);
            if (r >= 0)
                used[r] = 1;
        }
        p += n - 1;
    }
}

static char *cf_subst(const char *file, int line, const char *tmpl,
                      const int *regs, const long *imms, const int *isimm,
                      const int *ismem, const char *const *names, int nops)
{
    size_t cap = strlen(tmpl) * 2 + 64, len = 0;
    char *out = xmalloc(cap);
    for (const char *p = tmpl; *p; ) {
        int k = -1;
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
            /* a register named in the template: %d0, %sp, %sr, ... */
            out[len++] = '%';
            continue;
        }
        if (bare && !isimm[k])
            diag_fatal(file, line, "%%c%d names a register operand; %%c "
                       "prints a constant, and wants an \"i\" or \"n\" "
                       "operand", k);
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len,
                                    bare ? "%ld" : "#%ld", imms[k]);
        else if (ismem[k])
            len += (size_t)snprintf(out + len, cap - len, "(%%%s)",
                                    cf_reg_name(regs[k]));
        else
            len += (size_t)snprintf(out + len, cap - len, "%%%s",
                                    cf_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_coldfire(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    int ismem[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[16] = { 0 };

    /* Operands are numbered outputs first, then inputs, as gcc does. */
    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "ColdFire", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        ismem[i] = asm_constraint_mem_only(op->constraint);
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        if (!isimm[i] && !ismem[i] && sizes[i] > 4)
            diag_fatal(file, s->line, "ColdFire asm operand %d is %d bytes; an "
                                      "operand is one 32-bit register (pass a "
                                      "64-bit value as two)", i, sizes[i]);
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        const char *c = a->clob[i];
        int r = cfasm_gpr(c, (int)strlen(c));
        if (r == CF_FP || r == CF_SP)
            diag_fatal(file, s->line, "ColdFire asm clobbers '%s', the %s "
                                      "pointer; EmbCC does not save it around "
                                      "an asm", c,
                       r == CF_SP ? "stack" : "frame");
        if (r >= 0) {
            used[r] = 1;
            continue;
        }
        if (strcmp(c, "memory") && strcmp(c, "cc"))
            diag_fatal(file, s->line, "asm clobber '%s' is not a ColdFire "
                                      "register (%%d0-%%d7, %%a0-%%a5, \"cc\" "
                                      "or \"memory\")", c);
    }
    cf_mark_template_regs(a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        const int *pool;
        int np, r = -1;
        if (regs[i] != -2 && regs[i] != -3)
            continue;
        pool = regs[i] == -2 ? cf_asm_dpool : cf_asm_apool;
        np = regs[i] == -2 ? (int)(sizeof cf_asm_dpool / sizeof *cf_asm_dpool)
                           : (int)(sizeof cf_asm_apool / sizeof *cf_asm_apool);
        for (int k = 0; k < np; k++)
            if (!used[pool[k]]) {
                r = pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free %s register for an asm operand",
                       regs[i] == -2 ? "data" : "address");
        used[r] = 1;
        regs[i] = r;
    }
    for (int i = 0; i < a->nout; i++)
        if (!ismem[i] && CF_IS_A(regs[i]) && sizes[i] != 4)
            diag_fatal(file, s->line, "asm output %d is %d bytes in an address "
                                      "register, which ColdFire moves as a "
                                      "long only", i, sizes[i]);

    /* basic asm is its text: `%d0` and `%%` as written */
    char *text = a->is_basic ? xstrndup(a->tmpl, strlen(a->tmpl))
                             : cf_subst(file, s->line, a->tmpl, regs, imms,
                                        isimm, ismem, names, nops);
    struct code c = { 0 };
    char err[512];
    if (cfasm_assemble(text, &c, err, sizeof err) != 0)
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
        o->mem = asm_constraint_mem_only(a->in[i].constraint);
        o->temp = o->mem ? gen_addr(fn, a->in[i].expr)
                         : gen_expr(fn, a->in[i].expr);
        o->size = 4;
    }
    /* An "=d" output of an integer or a pointer is a VALUE (the asm's dst,
     * stored to its lvalue afterwards); anything else is written through
     * the lvalue's address -- RX's and Xtensa's scheme. */
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
    /* What the asm may change: every register used[] holds and the address
     * register an output through an address is stored with. */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (unsigned q = 0;
                 q < sizeof cf_asm_apool / sizeof cf_asm_apool[0]; q++)
                if (!used[cf_asm_apool[q]]) {
                    ia->scr = cf_asm_apool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
    used[CF_A1] = 1;                     /* the lowering's own */
    for (int r = 0; r < 16; r++)
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
