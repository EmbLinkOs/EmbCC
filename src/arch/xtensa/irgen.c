/* Xtensa's share of IR generation: va_arg, and which constants the
 * optimizer may fold into an instruction.
 *
 * A va_list is GCC's 12-byte record (xtensa_build_builtin_va_list), held
 * by value so that one passes between EmbCC's objects and GCC's:
 *
 *     struct { int *__va_stk; int *__va_reg; int __va_ndx; }
 *
 * __va_reg is the callee's save area of a2-a7, __va_stk the incoming sp
 * minus 32, and __va_ndx a byte index into both: below 24 an argument is
 * in a register word, at 32 and above on the stack (the gap keeps the
 * stack's 16-byte alignment). va_arg is xtensa_gimplify_va_arg_expr:
 *
 *     ndx = round_up(ap.ndx, align);       (align > 4 only, at most 16)
 *     ap.ndx = ndx + size4;                (size4: the size in words, * 4)
 *     if (ap.ndx <= 24) array = ap.reg;
 *     else { if (ndx <= 24) ap.ndx = 32 + size4;   (wholly on the stack)
 *            array = ap.stk; }
 *     return *(T *)(array + ap.ndx - size4);
 *
 * A `float` arrives as a double. A struct is its own bytes, by value; a
 * _Complex is its two parts, each taken as an argument of its own.
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

/* One step of the walk: the address of the next argument of `size`
 * bytes and `align`, with ap.ndx advanced past it. apa is &ap. */
static int va_step(struct ir_func *fn, struct expr *lv, int apa, long size,
                   long align)
{
    struct type *word = ty_int_of_size(4, 1);
    long size4 = (size + 3) & ~3L;
    int ndx, newndx, arr, l_stk, l_have, l_over, addr;

    if (align > 16)
        align = 16;
    ndx = emit_load(fn, emit_bin(fn, IR_ADD, apa, emit_const(fn, 8, 4), 4, 1),
                    word);
    irg_mark_natural(fn, lv);
    if (align > 4)
        ndx = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, ndx, emit_const(fn, align - 1, 4),
                                4, 1),
                       emit_const(fn, -align, 4), 4, 1);
    newndx = new_temp(fn);
    emit_mov(fn, newndx, emit_bin(fn, IR_ADD, ndx, emit_const(fn, size4, 4),
                                  4, 1));
    arr = new_temp(fn);
    l_stk = new_label(fn);
    l_have = new_label(fn);
    l_over = new_label(fn);
    /* in the register words? */
    emit_brz(fn, emit_cmp(fn, B_LE, newndx, emit_const(fn, 24, 4), 4, 1), 4,
             l_stk);
    emit_mov(fn, arr, emit_load(fn, emit_bin(fn, IR_ADD, apa,
                                             emit_const(fn, 4, 4), 4, 1),
                                word));
    irg_mark_natural(fn, lv);
    emit_jmp(fn, l_over);
    /* on the stack: one that straddled the register words is wholly
     * there, at index 32 */
    emit_label(fn, l_stk);
    emit_brz(fn, emit_cmp(fn, B_LE, ndx, emit_const(fn, 24, 4), 4, 1), 4,
             l_have);
    emit_mov(fn, newndx, emit_const(fn, 32 + size4, 4));
    emit_label(fn, l_have);
    emit_mov(fn, arr, emit_load(fn, apa, word));
    irg_mark_natural(fn, lv);
    emit_label(fn, l_over);
    emit_store(fn, emit_bin(fn, IR_ADD, apa, emit_const(fn, 8, 4), 4, 1),
               newndx, word);
    irg_mark_natural(fn, lv);
    addr = new_temp(fn);
    emit_mov(fn, addr, emit_bin(fn, IR_ADD, arr,
                                emit_bin(fn, IR_SUB, newndx,
                                         emit_const(fn, size4, 4), 4, 1),
                                4, 1));
    return addr;
}

int irg_va_arg_xtensa(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    long size = ty_size(rt);
    long align = ty_align(rt);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;
    int apa, addr;

    if (flt && rt->kind == TY_FLOAT) {          /* promoted to double */
        size = 8;
        align = 8;
    }
    apa = gen_addr(fn, e->lhs);

    /* A _Complex was passed as its two parts, each an argument of its
     * own (xtensa_gimplify_va_arg_expr does the same): the real part may
     * be in a7 and the imaginary one on the stack. */
    if (sdst >= 0 && rt->is_complex && rt->celem) {
        long esz = ty_size(rt->celem);
        int re = va_step(fn, e->lhs, apa, esz, esz);
        irg_va_copy(fn, sdst, 0, re, esz);
        irg_va_copy(fn, sdst, esz, va_step(fn, e->lhs, apa, esz, esz), esz);
        return sdst;
    }
    addr = va_step(fn, e->lhs, apa, size, align);

    /* Every slot is a whole word, 8-aligned for an 8-aligned type, so the
     * reads are natural. */
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

/* Which constants an instruction can take without building them first:
 * addi's -128..127 (and addmi's multiples of 256, which the code
 * generator pairs with an addi), extui's runs of low bits for an AND, and
 * for a comparison 0 or one of the sixteen constants the immediate
 * branches name (b4const and b4constu). Anything else stays in a register,
 * built once rather than at each use. */
static int b4(long v)
{
    static const long c[] = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32,
                              64, 128, 256, 32768, 65536 };
    for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++)
        if (c[k] == v)
            return 1;
    return 0;
}

int xtensa_imm_foldable(int op, long imm)
{
    long v = (long)(int)imm;
    switch (op) {
    case IR_ADD:
        return v >= -32768 + 128 && v <= 32512 + 127;
    case IR_SUB:
        return -v >= -32768 + 128 && -v <= 32512 + 127;
    case IR_AND: {
        unsigned long c = (unsigned long)imm & 0xffffffffUL;
        return c == 0xffffffffUL || ((c & (c + 1)) == 0 && c <= 0xffff);
    }
    case IR_CMP:
        /* x <= k and x > k test k + 1 */
        return v == 0 || b4(v) || b4(v + 1);
    default:
        return 0;
    }
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE, into register names, and
 * the result handed to src/arch/xtensa/asm.c; the backend then only loads
 * the inputs, splices the bytes and stores the outputs -- LoongArch's
 * split (loongarch/irgen.c irg_asm_loongarch), operand for operand: a
 * register operand substitutes to `a10`, an immediate to its value, and
 * an "m" operand -- a register holding the ADDRESS -- to `a10, 0`, the
 * base and offset an Xtensa load or store takes, as GCC prints it.
 *
 * What the windowed ABI changes:
 *
 *   * a0 is this function's return address and a1 its stack pointer.
 *     Neither is ever an operand's register, a clobber list may not name
 *     them (EmbCC saves nothing around an asm), and a template touches
 *     them only where it names them itself.
 *   * a2-a7 survive a call because the window rotates, not because
 *     anything saves them -- so a template may use any of a2-a15 freely,
 *     as long as it says so: a value live across the asm keeps out of
 *     every register the asm changes (ir_asm.clob).
 *   * A call in the template is a windowed call: call8/callx8 rotate by
 *     eight, and the callee's a0-a7 are this function's a8-a15, which the
 *     call therefore changes -- as call4 changes a4-a15 and call12
 *     a12-a15. Those are added to what the asm changes, and no operand is
 *     put in them. call0/callx0 would write a0, the return address; they
 *     are refused.
 */

/* The registers an operand may be given when its constraint names none:
 * a10-a13 first, where a call's arguments go and nothing lives across a
 * call, then a2-a6 and a8-a9. Not a7 (the frame base when the function
 * calls alloca), nor a14/a15, the code generator's scratch. */
static const int xt_asm_pool[] = {
    XT_A10, XT_A11, XT_A12, XT_A13, XT_A2, XT_A3, XT_A4, XT_A5, XT_A6,
    XT_A8, XT_A9
};

/* Is p (in text) the start of a word -- not inside an identifier, a
 * number or a `%[name]`? */
static int xt_word_start(const char *text, const char *p)
{
    return p == text || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                          p[-1] == '.' || p[-1] == '%' || p[-1] == '[');
}

static int xt_word_len(const char *p)
{
    int n = 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.')
        n++;
    return n;
}

/* The registers the template names outright: off limits to allocation,
 * and changed as far as anything outside knows. */
static void xt_mark_template_regs(const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; p++) {
        int n;
        if (!xt_word_start(tmpl, p) || !isalpha((unsigned char)*p))
            continue;
        n = xt_word_len(p);
        {
            int r = xtasm_gpr(p, n);
            if (r >= 0)
                used[r] = 1;
        }
        p += n - 1;
    }
}

/* The window a call in the template rotates (4, 8 or 12), or 0. A call0
 * or callx0 is refused. */
static int xt_template_call(const char *file, int line, const char *text)
{
    int inc = 0;
    for (const char *p = text; *p; p++) {
        int n;
        char mn[16];
        if (!xt_word_start(text, p) ||
            (!isalpha((unsigned char)*p) && *p != '_'))
            continue;
        n = xt_word_len(p);
        if (n < (int)sizeof mn) {
            int k = 0;
            for (int i = *p == '_'; i < n; i++)
                mn[k++] = (char)tolower((unsigned char)p[i]);
            mn[k] = 0;
            if (!strcmp(mn, "call0") || !strcmp(mn, "callx0"))
                diag_fatal(file, line, "%s in Xtensa asm writes a0, which "
                           "holds this function's return address under the "
                           "windowed ABI: call a windowed function with "
                           "call8/callx8", mn);
            {
                int w = !strcmp(mn, "call4") || !strcmp(mn, "callx4") ? 4
                      : !strcmp(mn, "call8") || !strcmp(mn, "callx8") ? 8
                      : !strcmp(mn, "call12") || !strcmp(mn, "callx12")
                      ? 12 : 0;
                if (w && (!inc || w < inc))
                    inc = w;
            }
        }
        p += n - 1;
        /* skip the rest of the statement's operands' words: only a
         * mnemonic can be a call, and an operand named like one is not */
        while (p[1] && p[1] != '\n' && p[1] != ';')
            p++;
    }
    return inc;
}

static char *xt_subst(const char *file, int line, const char *tmpl,
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
                                   "supported for Xtensa", *p ? *p : ' ');
        }
        if (bare && !isimm[k])
            diag_fatal(file, line, "%%c%d names a register operand; %%c "
                       "prints a constant, and wants an \"i\" or \"n\" "
                       "operand", k);
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else if (ismem[k])
            len += (size_t)snprintf(out + len, cap - len, "%s, 0",
                                    xt_reg_name(regs[k]));
        else
            len += (size_t)snprintf(out + len, cap - len, "%s",
                                    xt_reg_name(regs[k]));
    }
    out[len] = '\0';
    return out;
}

void irg_asm_xtensa(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    int ismem[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[16] = { 0 };
    int inc;

    /* Operands are numbered outputs first, then inputs, as gcc does. */
    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "Xtensa", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        ismem[i] = asm_constraint_mem_only(op->constraint);
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        if (!isimm[i] && !ismem[i] && sizes[i] > 4)
            diag_fatal(file, s->line, "Xtensa asm operand %d is %d bytes; an "
                                      "operand is one 32-bit register (pass a "
                                      "64-bit value as two)", i, sizes[i]);
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        const char *c = a->clob[i];
        int r = xtasm_gpr(c, (int)strlen(c));
        if (r == XT_A0 || r == XT_A1)
            diag_fatal(file, s->line, "Xtensa asm clobbers '%s', which holds "
                                      "this function's %s under the windowed "
                                      "ABI; EmbCC does not save it around an "
                                      "asm", c,
                       r == XT_A0 ? "return address" : "stack pointer");
        if (r >= 0) {
            used[r] = 1;
            continue;
        }
        if (strcmp(c, "memory") && strcmp(c, "cc") && strcmp(c, "sar"))
            diag_fatal(file, s->line, "asm clobber '%s' is not an Xtensa "
                                      "register (a2-a15, sar, or \"memory\")",
                       c);
    }
    xt_mark_template_regs(a->tmpl, used);
    /* a windowed call changes the callee's window: from a(inc) up */
    inc = xt_template_call(file, s->line, a->tmpl);
    for (int r = inc; inc && r < 16; r++)
        used[r] = 1;

    for (int i = 0; i < nops; i++) {
        if (regs[i] != -2)
            continue;
        int r = -1;
        for (unsigned k = 0; k < sizeof xt_asm_pool / sizeof xt_asm_pool[0];
             k++)
            if (!used[xt_asm_pool[k]]) {
                r = xt_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = xt_subst(file, s->line, a->tmpl, regs, imms, isimm, ismem,
                          names, nops);
    struct code c = { 0 };
    char err[512];
    xtasm_set_pc(-1);
    if (xtasm_assemble(text, &c, err, sizeof err) != 0)
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
     * operands', the clobbers', the template's, a call's window) and the
     * scratch an output through an address is stored with. A value live
     * across the asm keeps out of exactly these (regalloc.c). */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (unsigned q = 0;
                 q < sizeof xt_asm_pool / sizeof xt_asm_pool[0]; q++)
                if (!used[xt_asm_pool[q]]) {
                    ia->scr = xt_asm_pool[q];
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
