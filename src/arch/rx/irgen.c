/* Renesas RX's share of IR generation: va_arg, and inline assembly.
 *
 * GCC's RX convention puts every unnamed argument -- and the last named
 * one -- on the stack, each at its type's alignment (at most 4) and
 * taking its own size (docs/internals/rx-plan.md). So a va_list is a
 * bare pointer walking the caller's stack block: va_arg rounds it up to
 * the type's alignment, reads the value there, and steps by the size.
 * The default promotions mean an unnamed scalar is at least an int, and
 * a `float` arrives as a `double` -- which is the same binary32 here. A
 * struct is its own bytes, by value.
 */
#include "../../ir/irgen_int.h"
#include "asm.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int irg_va_arg_rx(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);

    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (align > 4)
        align = 4;
    if (align > 1)
        cur = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, cur,
                                emit_const(fn, align - 1, 4), 4, 1),
                       emit_const(fn, -align, 4), 4, 1);
    {
        int addr = new_temp(fn);
        emit_mov(fn, addr, cur);
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, size, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (sdst >= 0) {
            irg_va_copy(fn, sdst, 0, addr, size);
            return sdst;
        }
        return emit_load(fn, addr, rt);
    }
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE, as GCC's RX port prints
 * them, and the result handed to src/arch/rx/asm.c; the backend then only
 * moves the inputs into their registers, splices the bytes and moves the
 * outputs out (rx/codegen.c IR_ASM) -- Xtensa's split (xtensa/irgen.c),
 * operand for operand: a register operand substitutes to `r3`, an
 * immediate to `#5`, and an "m" operand -- a register holding the ADDRESS
 * -- to `[r3]`, the memory operand an RX instruction takes.
 *
 * What the RX ABI changes:
 *
 *   * r0 is the stack pointer: never an operand's register, and a clobber
 *     list may not name it (EmbCC saves nothing around an asm).
 *   * r1-r5 and r14/r15 are caller-saved, r6-r13 callee-saved. A template
 *     may change any of r1-r15 as long as it says so: a value live across
 *     the asm keeps out of every register it changes (ir_asm.clob), and a
 *     callee-saved one it changes is saved by the function's prologue, as
 *     GCC saves it.
 *   * Operands are given r1-r4 first, then r6-r12: not r5, r14 or r15,
 *     the code generator's scratch, and not r13, the frame base of a
 *     function that calls alloca.
 */
static const int rx_asm_pool[] = { 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12 };

static int rx_word_start(const char *text, const char *p)
{
    return p == text || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                          p[-1] == '.' || p[-1] == '%' || p[-1] == '[');
}

/* The registers the template names outright: off limits to allocation,
 * and changed as far as anything outside knows. */
static void rx_mark_template_regs(const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n = 0;
        if (!rx_word_start(tmpl, p) || !isalpha((unsigned char)*p))
            continue;
        while (isalnum((unsigned char)p[n]) || p[n] == '_')
            n++;
        {
            int r = rxasm_gpr(p, n);
            if (r >= 0)
                used[r] = 1;
        }
        p += n - 1;
    }
}

static char *rx_subst(const char *file, int line, const char *tmpl,
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
            diag_fatal(file, line, "asm template modifier '%%%c' is not "
                                   "supported for RX", *p ? *p : ' ');
        }
        if (bare && !isimm[k])
            diag_fatal(file, line, "%%c%d names a register operand; %%c "
                       "prints a constant, and wants an \"i\" or \"n\" "
                       "operand", k);
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len,
                                    bare ? "%ld" : "#%ld", imms[k]);
        else if (ismem[k])
            len += (size_t)snprintf(out + len, cap - len, "[r%d]", regs[k]);
        else
            len += (size_t)snprintf(out + len, cap - len, "r%d", regs[k]);
    }
    out[len] = '\0';
    return out;
}

void irg_asm_rx(struct ir_func *fn, struct stmt *s)
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
                                      "RX", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        ismem[i] = asm_constraint_mem_only(op->constraint);
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        if (!isimm[i] && !ismem[i] && sizes[i] > 4)
            diag_fatal(file, s->line, "RX asm operand %d is %d bytes; an "
                                      "operand is one 32-bit register (pass a "
                                      "64-bit value as two)", i, sizes[i]);
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        const char *c = a->clob[i];
        int r = rxasm_gpr(c, (int)strlen(c));
        if (r == 0)
            diag_fatal(file, s->line, "RX asm clobbers '%s', the stack "
                                      "pointer; EmbCC does not save it around "
                                      "an asm", c);
        if (r > 0) {
            used[r] = 1;
            continue;
        }
        if (strcmp(c, "memory") && strcmp(c, "cc"))
            diag_fatal(file, s->line, "asm clobber '%s' is not an RX register "
                                      "(r1-r15, \"cc\" or \"memory\")", c);
    }
    rx_mark_template_regs(a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        int r = -1;
        if (regs[i] != -2)
            continue;
        for (unsigned k = 0; k < sizeof rx_asm_pool / sizeof rx_asm_pool[0];
             k++)
            if (!used[rx_asm_pool[k]]) {
                r = rx_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = rx_subst(file, s->line, a->tmpl, regs, imms, isimm, ismem,
                          names, nops);
    struct code c = { 0 };
    char err[512];
    if (rxasm_assemble(text, &c, err, sizeof err) != 0)
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
     * operands', the clobbers', the template's) and the scratch an output
     * through an address is stored with. A value live across the asm
     * keeps out of exactly these (regalloc.c). */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (unsigned q = 0;
                 q < sizeof rx_asm_pool / sizeof rx_asm_pool[0]; q++)
                if (!used[rx_asm_pool[q]]) {
                    ia->scr = rx_asm_pool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
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
