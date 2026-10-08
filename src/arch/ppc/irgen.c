/* 32-bit PowerPC's share of IR generation: va_arg, and which constants the
 * optimizer may fold into an instruction.
 *
 * The SVR4/EABI va_list is a 12-byte record, which va_start builds in the
 * callee's frame (codegen.c) and `va_list` points at:
 *
 *   +0  gpr                 how many of r3..r10 the arguments read so far
 *                           used (an unsigned char)
 *   +1  fpr                 the same for f1..f8 -- always 0, soft float
 *   +4  overflow_arg_area   the next argument on the caller's stack
 *   +8  reg_save_area       r3..r10 as the prologue stored them
 *
 * An argument of one word comes from the save area while gpr < 8; a
 * long long or (soft) double takes an ODD-numbered pair, so gpr is rounded
 * up to even first and the pair must fit by 8. Once one does not fit, gpr
 * becomes 8 -- nothing later is read from registers -- and the argument
 * comes from the overflow area, 8-aligned for an 8-byte one. A struct of
 * any size travels BY REFERENCE: its word is a pointer to the caller's
 * copy. A float arrives as a double. This is clang's walk for
 * powerpc-none-eabi, read off its code (docs/internals/powerpc-plan.md).
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "asm.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int irg_va_arg_ppc(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    struct type *u8 = ty_int_of_size(1, 1);
    struct type *ptr = ty_int_of_size(4, 1);
    int is_struct = rt->kind == TY_STRUCT;
    int flt = !is_struct && ty_is_float(rt);
    int words = is_struct ? 1 : (flt || ty_size(rt) > 4) ? 2 : 1;
    int sdst = is_struct ? irg_va_struct_slot(fn, e) : -1;
    int ap = gen_expr(fn, e->lhs);              /* the record's address */
    int a_ova = emit_bin(fn, IR_ADD, ap, emit_const(fn, 4, 4), 4, 1);
    int a_rsa = emit_bin(fn, IR_ADD, ap, emit_const(fn, 8, 4), 4, 1);
    int addr = new_temp(fn);
    int l_over = new_label(fn), l_done = new_label(fn);
    int gpr, v;

    if (ty_size(rt) > 8 && !is_struct)
        diag_fatal(fn->file, e->line, "va_arg of a %d-byte scalar is not "
                   "supported for %s", ty_size(rt), target_triple_now());
    gpr = emit_load(fn, ap, u8);
    fn->ins[fn->nins - 1].natural = 1;
    if (words == 2)
        gpr = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, gpr, emit_const(fn, 1, 4), 4, 1),
                       emit_const(fn, -2, 4), 4, 1);
    emit_brz(fn, emit_cmp(fn, B_LT, gpr, emit_const(fn, 9 - words, 4), 4, 0),
             4, l_over);
    /* from the register save area: reg_save_area + 4 * gpr */
    {
        int rsa = emit_load(fn, a_rsa, ptr);
        fn->ins[fn->nins - 1].natural = 1;
        emit_mov(fn, addr,
                 emit_bin(fn, IR_ADD, rsa,
                          emit_bin(fn, IR_SHL, gpr, emit_const(fn, 2, 4), 4, 0),
                          4, 1));
        emit_store(fn, ap,
                   emit_bin(fn, IR_ADD, gpr, emit_const(fn, words, 4), 4, 1),
                   u8);
        fn->ins[fn->nins - 1].natural = 1;
        emit_jmp(fn, l_done);
    }
    /* from the overflow area, which every later argument comes from too */
    emit_label(fn, l_over);
    {
        int ova;
        emit_store(fn, ap, emit_const(fn, 8, 4), u8);
        fn->ins[fn->nins - 1].natural = 1;
        ova = emit_load(fn, a_ova, ptr);
        fn->ins[fn->nins - 1].natural = 1;
        if (words == 2)
            ova = emit_bin(fn, IR_AND,
                           emit_bin(fn, IR_ADD, ova, emit_const(fn, 7, 4), 4, 1),
                           emit_const(fn, -8, 4), 4, 1);
        emit_mov(fn, addr, ova);
        emit_store(fn, a_ova,
                   emit_bin(fn, IR_ADD, ova, emit_const(fn, 4 * words, 4), 4, 1),
                   ptr);
        fn->ins[fn->nins - 1].natural = 1;
    }
    emit_label(fn, l_done);

    if (is_struct) {
        /* the word is a pointer to the caller's copy */
        int src = emit_load(fn, addr, ptr);
        fn->ins[fn->nins - 1].natural = 1;
        irg_va_copy(fn, sdst, 0, src, ty_size(rt));
        return sdst;
    }
    if (flt) {
        v = emit_load(fn, addr, ty_base(TY_DOUBLE, 0));
        fn->ins[fn->nins - 1].natural = 1;
        if (rt->kind == TY_FLOAT) {               /* promoted to double */
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
    v = emit_load(fn, addr, rt);
    fn->ins[fn->nins - 1].natural = 1;
    return v;
}

/* What codegen.c takes as an immediate without building it: addi's SIGNED
 * 16 bits for add (a subtraction adds the negation) and mulli's for a
 * multiply; the UNSIGNED 16 bits of andi./ori/xori, or the same shifted up
 * by 16 (andis./oris/xoris), and for an AND any run of ones rlwinm masks;
 * for a compare 0..32767, which both cmpwi and cmplwi take. Here rather
 * than beside the lowering because embls links the optimizer without the
 * code generator. */
static int rlw_mask(unsigned long m)
{
    unsigned long n;
    m &= 0xffffffffUL;
    if (m == 0)
        return 0;
    /* a run of ones, or the complement of one (a mask that wraps) */
    n = m & 1 ? ~m & 0xffffffffUL : m;
    if (n == 0)
        return 1;
    while (!(n & 1))
        n >>= 1;
    return (n & (n + 1)) == 0;
}

int ppc_imm_foldable(int op, long imm)
{
    unsigned long u = (unsigned long)imm & 0xffffffffUL;
    switch (op) {
    case IR_ADD: case IR_MUL:
        return imm >= -32768 && imm <= 32767;
    case IR_SUB:
        return imm >= -32767 && imm <= 32768;
    case IR_OR: case IR_XOR:
        return (imm >= 0 && imm <= 0xffff) || (u & 0xffffUL) == 0;
    case IR_AND:
        return (imm >= 0 && imm <= 0xffff) || (u & 0xffffUL) == 0 ||
               rlw_mask(u);
    case IR_CMP:
        return imm >= 0 && imm <= 32767;
    default:
        return 0;
    }
}

/* A 64-bit AND/OR/XOR, which the code generator does half by half: each
 * half the identity, zero, all ones, or what the 32-bit form takes. */
static int half_ok(int op, unsigned long c)
{
    if (c == 0 || c == 0xffffffffUL)
        return 1;
    return ppc_imm_foldable(op == IR_AND ? IR_AND : IR_OR, (long)c);
}

int ppc_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    unsigned long lo = u & 0xffffffffUL, hi = (u >> 32) & 0xffffffffUL;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    return half_ok(op, lo) && half_ok(op, hi);
}

/* ---- inline assembly ----------------------------------------------------
 *
 * The template's operands are substituted HERE and the result handed to
 * src/arch/ppc/asm.c; the backend then only loads the inputs, splices the
 * bytes and stores the outputs -- the Xtensa and SPARC split. A register
 * operand substitutes to `%_N` (register N, spelt so that the assembler
 * can tell an operand from a register the template names itself), an
 * immediate to its value, and an "m" operand -- a register holding the
 * ADDRESS -- to `0(%_N)`, the D-form GCC prints; %U and %X (the update
 * and indexed forms GCC may pick for an "m") print nothing, since this
 * operand is always the plain D-form.
 *
 * What the EABI changes:
 *
 *   * r1 is the stack pointer and r2 and r13 the small-data anchors; an
 *     operand never goes there and a clobber list may not name them. r0
 *     reads as 0 where a base goes, and r9-r12 and r0 are the code
 *     generator's scratch: no operand goes there either.
 *   * Which registers a template names cannot be read off its text --
 *     GCC's PowerPC registers are bare numbers -- so the template is
 *     assembled once with every operand in a placeholder register, and
 *     the assembler reports the ones it named (ppcasm_named). A value live
 *     across the asm keeps out of those, the operands', the clobbers' and
 *     the scratch (ir_asm.clob), and a callee-saved one among them
 *     (r14-r31) is saved by the prologue.
 *   * A template that links (bl, bctrl, a write of LR, or an "lr"
 *     clobber) makes the function save LR (ir_asm.calls); one that calls
 *     changes r0 and r3-r12, which its operands keep out of.
 */

/* The registers an operand may be given: r3-r8 (nothing lives across a
 * call in them), then r14-r30. Not r31 (the frame base under alloca). */
static const int ppc_asm_pool[] = {
    3, 4, 5, 6, 7, 8, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    27, 28, 29, 30
};
#define PPC_NPOOL ((int)(sizeof ppc_asm_pool / sizeof ppc_asm_pool[0]))

static char *ppc_subst(const char *file, int line, const char *tmpl,
                       const int *regs, const long *imms, const int *isimm,
                       const int *ismem, const char *const *names, int nops)
{
    size_t cap = strlen(tmpl) * 2 + 64, len = 0;
    char *out = xmalloc(cap);
    for (const char *p = tmpl; *p; ) {
        int k = -1, mod = 0;
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
        if ((*p == 'U' || *p == 'X' || *p == 'c') &&
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
        } else if (isalpha((unsigned char)*p) &&
                   (isdigit((unsigned char)p[1]) || p[1] == '[')) {
            diag_fatal(file, line, "asm template modifier '%%%c' is not "
                                   "supported for PowerPC", *p);
        } else {
            out[len++] = '%';               /* %r3, as written */
            continue;
        }
        if ((mod == 'U' || mod == 'X') && !ismem[k])
            diag_fatal(file, line, "asm template modifier '%%%c' needs a "
                                   "memory operand", mod);
        if (mod == 'c' && !isimm[k])
            diag_fatal(file, line, "asm template modifier '%%c' needs a "
                                   "constant operand");
        if (mod == 'U' || mod == 'X')
            continue;
        if (isimm[k])
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        else if (ismem[k])
            len += (size_t)snprintf(out + len, cap - len, "0(%%_%d)", regs[k]);
        else
            len += (size_t)snprintf(out + len, cap - len, "%%_%d", regs[k]);
    }
    out[len] = '\0';
    return out;
}

/* The template's named registers, read from its text when the
 * placeholder assembly failed: every rN / %rN word. */
static unsigned long ppc_text_regs(const char *t)
{
    unsigned long m = 0;
    for (const char *p = t; *p; p++) {
        int n = 0, r;
        if (p > t && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
            continue;
        while (isalnum((unsigned char)p[n]) || p[n] == '%')
            n++;
        r = ppcasm_is_reg(p, n);
        if (r >= 0)
            m |= 1UL << r;
        if (n)
            p += n - 1;
    }
    return m;
}

void irg_asm_ppc(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    int ismem[2 * MAX_PARAMS], tie[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[32] = { 0 };
    int links = 0;
    unsigned long named = 0;
    char err[512];

    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        const char *c = op->constraint;
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "PowerPC", op->constraint);
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
            diag_fatal(file, s->line, "PowerPC asm operand %d is %d bytes; an "
                                      "operand is one 32-bit register (pass a "
                                      "64-bit value as two)", i, sizes[i]);
        if (op->reg >= 0)
            used[op->reg] = 1;
    }
    for (int i = 0; i < a->nclob; i++) {
        const char *c = a->clob[i];
        int r = ppcasm_gpr(c, (int)strlen(c));
        if (r == 1 || r == 2 || r == 13)
            diag_fatal(file, s->line, "PowerPC asm clobbers '%s', which holds "
                                      "%s; EmbCC does not save it around an "
                                      "asm", c,
                       r == 1 ? "the stack pointer"
                              : "an EABI small-data anchor");
        if (r >= 0) {
            used[r] = 1;
            continue;
        }
        if (!strcmp(c, "lr")) {
            links = 1;
            continue;
        }
        if (strcmp(c, "memory") && strcmp(c, "cc") && strcmp(c, "ctr") &&
            strcmp(c, "xer") && !(strlen(c) == 3 && c[0] == 'c' &&
                                  c[1] == 'r' && c[2] >= '0' && c[2] <= '7'))
            diag_fatal(file, s->line, "asm clobber '%s' is not a PowerPC "
                                      "register (r0-r31, cr0-cr7, ctr, lr, "
                                      "xer, \"cc\" or \"memory\")", c);
    }

    /* What the template names, and whether it links: assembled once with
     * the operands in placeholders (two tries, as a placeholder may
     * collide with a register the template writes beside it). */
    if (a->is_basic) {
        struct code c0 = { 0 };
        if (ppcasm_assemble(a->tmpl, &c0, err, sizeof err) != 0)
            diag_fatal(file, s->line, "%s", err);
        named = ppcasm_named();
        links |= ppcasm_links();
        free(c0.p);
    } else {
        int ok = 0;
        for (int base = 3; base <= 14 && !ok; base += 11) {
            int ph[2 * MAX_PARAMS];
            struct code c0 = { 0 };
            char *t0;
            for (int i = 0; i < nops; i++)
                ph[i] = regs[i] >= 0 ? regs[i] : (base + i) % 32;
            for (int i = 0; i < nops; i++)
                if (tie[i] >= 0)
                    ph[i] = ph[tie[i]];
            t0 = ppc_subst(file, s->line, a->tmpl, ph, imms, isimm, ismem,
                           names, nops);
            if (ppcasm_assemble(t0, &c0, err, sizeof err) == 0) {
                ok = 1;
                named = ppcasm_named();
                links |= ppcasm_links();
            }
            free(t0);
            free(c0.p);
        }
        if (!ok)
            named = ppc_text_regs(a->tmpl);
    }
    if (named & (1UL << 1 | 1UL << 2 | 1UL << 13))
        named &= ~(1UL << 1 | 1UL << 2 | 1UL << 13);   /* never allocated */
    for (int r = 0; r < 32; r++)
        if (named >> r & 1)
            used[r] = 1;
    /* a call changes r0 and r3-r12 */
    if (links) {
        used[0] = 1;
        for (int r = 3; r <= 12; r++)
            used[r] = 1;
    }

    for (int i = 0; i < nops; i++) {
        int r = -1;
        if (regs[i] != -2 || tie[i] >= 0)
            continue;
        for (int k = 0; k < PPC_NPOOL; k++)
            if (!used[ppc_asm_pool[k]]) {
                r = ppc_asm_pool[k];
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
                             : ppc_subst(file, s->line, a->tmpl, regs, imms,
                                         isimm, ismem, names, nops);
    struct code c = { 0 };
    if (ppcasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    free(text);

    struct ir_asm *ia = xcalloc(1, sizeof *ia);
    ia->code = c.p;
    ia->codelen = c.len;
    ia->calls = links;
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
    int vk[2 * MAX_PARAMS], vaddr[2 * MAX_PARAMS], nv = 0;
    for (int i = 0; i < a->nout; i++) {
        struct expr *lv = a->out[i].expr;
        int inout = strchr(a->out[i].constraint, '+') != NULL;
        int mem = asm_constraint_mem_only(a->out[i].constraint);
        if (!inout && !mem && irg_asm_val_ok(lv, 4)) {
            vaddr[nv] = irg_asm_out_addr(fn, lv);
            vk[nv++] = i;
            if (nv > 1)
                continue;
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
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (int q = 0; q < PPC_NPOOL; q++)
                if (!used[ppc_asm_pool[q]]) {
                    ia->scr = ppc_asm_pool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
    /* (bit 1, r1, which nothing is ever allocated to, says "known") */
    ia->clob = 1UL << 1;
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
