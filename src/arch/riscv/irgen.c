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
    int lbyref = 0;

    /* A long double is binary128 here. At RV64 that is two registers, an
     * aligned pair when variadic like any 2*XLEN scalar, and read below
     * as itself. At RV32 it is four, which the psABI passes BY REFERENCE:
     * the slot holds a pointer to the caller's copy, one word, and the
     * value is read through it. */
    if (flt && size > 2 * wb) {
        lbyref = 1;
        size = align = wb;
    }

    int apa;
    int cur = irg_va_ptr_read(fn, e->lhs, ptr, &apa);

    /* A struct of more than two registers came by REFERENCE: its slot is
     * a pointer to the caller's copy. A smaller one is its own bytes in
     * whole registers, an aligned pair when its alignment is two
     * registers' (as below). Either way they are copied into the
     * expression's slot. */
    int sdst = -1, sref = 0;
    if (rt->kind == TY_STRUCT) {
        sdst = irg_va_struct_slot(fn, e);
        if (size > 2 * wb) {
            sref = 1;
            size = align = wb;
        }
    }

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
        irg_va_ptr_write(fn, e->lhs, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, wb),
                            wb, 1),
                   ptr);
        if (sdst >= 0) {
            irg_va_copy(fn, sdst, 0, sref ? emit_load(fn, addr, ptr) : addr,
                        ty_size(rt));
            return sdst;
        }

        if (lbyref)
            return emit_load(fn, emit_load(fn, addr, ptr), rt);
        if (flt && size <= 8) {
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

/* The scratch an output stored through its address is written with, as
 * the code generator chooses it. Not t6: a far slot access borrows it. */
static const int rv_scr_pool[] = {
    RV_T0, RV_T1, RV_T2, RV_T3, RV_T4, RV_T5,
    RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7
};

/* Does the template call or trap -- `call`, `tail`, `jal`, `jalr`,
 * `ecall` -- so that it changes what a call changes, whatever its
 * clobber list says? */
static int rv_template_calls(const char *text)
{
    for (const char *p = text; *p; p++) {
        if (!isalpha((unsigned char)*p) ||
            (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                          p[-1] == '.')))
            continue;
        int n = 0;
        while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.')
            n++;
        if ((n == 4 && (!strncmp(p, "call", 4) || !strncmp(p, "tail", 4) ||
                        !strncmp(p, "jalr", 4))) ||
            (n == 3 && !strncmp(p, "jal", 3)) ||
            (n == 5 && !strncmp(p, "ecall", 5)))
            return 1;
        p += n - 1;
    }
    return 0;
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
    struct code c = { 0 };
    char err[512];
    if (rvasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    int calls = rv_template_calls(text);
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
        o->size = target_ptr_size();  /* a temp holds the promoted value */
    }
    /* An "=r" output of an integer or a pointer is a VALUE: the asm's
     * dst, stored to its lvalue afterwards (irg_asm_out_store), so a
     * local it writes is not address-taken. The first is this asm's own
     * dst; each further one is a continuation right after it
     * (ir_asm.cont). Anything else -- "+", "m", another type -- is
     * written through the lvalue's address, as before. Thumb's
     * irg_asm_thumb is the same. */
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
     * everything a call changes -- ra, t0-t6 and a0-a7. A value live
     * across the asm keeps out of exactly these (regalloc.c). */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (unsigned q = 0; q < sizeof rv_scr_pool / sizeof rv_scr_pool[0];
                 q++)
                if (!used[rv_scr_pool[q]]) {
                    ia->scr = rv_scr_pool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
    if (calls)
        for (int r = 0; r < 32; r++)
            if (r == 1 || (r >= 5 && r <= 7) || (r >= 10 && r <= 17) ||
                r >= 28)
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
        struct ir_asm *c = xcalloc(1, sizeof *c);
        c->cont = 1;
        c->in = xcalloc(1, sizeof *c->in);
        c->out = xcalloc(1, sizeof *c->out);
        c->nout = 1;
        c->out[0].reg = regs[vk[k]];
        c->out[0].size = sizes[vk[k]];
        c->out[0].val = 1;
        c->out[0].temp = -1;
        struct ir_ins *ci = emit(fn);
        ci->op = IR_ASM;
        ci->asm_ir = c;
        ci->dst = vdst[k] = new_temp(fn);
        ci->a = ci->b = -1;
    }
    for (int k = 0; k < nv; k++)
        irg_asm_out_store(fn, a->out[vk[k]].expr, vaddr[k], vdst[k]);
}

/* What the IR_ADD..IR_CMP lowerings in codegen.c take as an immediate
 * without building the constant first -- the optimizer asks before
 * folding one (opt.c's target_imm_foldable). Anything else is folded and
 * then rebuilt at every use, a lui/addi pair inside a loop, where a value
 * left in a register is built once and can be hoisted.
 *
 * add/and/or/xor take a signed 12-bit immediate, and sub is an add of the
 * negation. A branch compares two REGISTERS -- there is no immediate
 * form -- so a compare's constant is free only when it is zero, which is
 * x0. mul has no immediate at all. HERE rather than beside the lowerings
 * because embls links the optimizer without the code generator. */
/* ...and for a 64-bit AND, OR or XOR at RV32, which the code generator
 * does half by half (logic_half): a half it takes without building it is
 * zero or all ones, a 12-bit immediate (andi/ori/xori), or for an AND a
 * mask of the low or of the high bits (two shifts). Both halves must be,
 * or the constant stays in a register pair, built once. */
static int riscv_half_ok(int op, unsigned long c)
{
    unsigned long nc = ~c & 0xffffffffUL;
    long sc = (long)(int)(unsigned int)c;
    if (c == 0 || c == 0xffffffffUL || (sc >= -2048 && sc <= 2047))
        return 1;
    return op == IR_AND && ((c & (c + 1)) == 0 || (nc & (nc + 1)) == 0);
}
int riscv_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    unsigned long lo = u & 0xffffffffUL, hi = (u >> 32) & 0xffffffffUL;
    unsigned long idn = op == IR_AND ? 0xffffffffUL : 0;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    /* One half that is the identity costs nothing, so the other half may
     * be anything: built in t4 and applied, it is one or two instructions
     * against a whole pair built and applied -- `v |= 1ULL << 51` is a lui
     * and an or. */
    if (lo == idn || hi == idn)
        return 1;
    return riscv_half_ok(op, lo) && riscv_half_ok(op, hi);
}
int riscv_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD: case IR_AND: case IR_OR: case IR_XOR:
        return imm >= -2048 && imm <= 2047;
    case IR_SUB:
        return imm >= -2047 && imm <= 2048;
    case IR_CMP:
        return imm == 0;
    default:
        return 0;
    }
}
