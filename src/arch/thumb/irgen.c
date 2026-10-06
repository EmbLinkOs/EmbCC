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
    /* a struct is its own bytes, doubleword-aligned by its NATURAL
     * alignment as a named one is (place_arg), copied into the slot */
    if (rt->kind == TY_STRUCT)
        align = ty_natural_align(rt);
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
        if (rt->kind == TY_STRUCT) {
            int dst = irg_va_struct_slot(fn, e);
            irg_va_copy(fn, dst, 0, addr, size);
            return dst;
        }

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

/* The scratch an output stored through its address is written with,
 * as the code generator chooses it: r12 first, the ABI's own. */
static const int t_scr_pool[] = { 12, 0, 1, 2, 3 };

/* Does the template call or trap -- `bl`, `blx`, `svc` -- so that it
 * changes what a call changes, whatever its clobber list says? */
static int t_template_calls(const char *text)
{
    for (const char *p = text; *p; p++) {
        if (!isalpha((unsigned char)*p) ||
            (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                          p[-1] == '.')))
            continue;
        int n = 0;
        while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.')
            n++;
        if ((n == 2 && !strncmp(p, "bl", 2)) ||
            (n == 3 && (!strncmp(p, "blx", 3) || !strncmp(p, "svc", 3))))
            return 1;
        p += n - 1;
    }
    return 0;
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
                                      "ARMv7-M", op->constraint);
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
    /* An IT block is one template's own business: it may not reach into
     * the compiler's code after the asm, whose first instructions would
     * then run conditionally. */
    tasm_reset();
    tasm_set_arch(target_thumb_arch());
    if (tasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    if (tasm_open())
        diag_fatal(file, s->line, "the asm ends inside an IT block, which "
                   "would make the compiler's next instructions conditional");
    int calls = t_template_calls(text);
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
     * dst, stored to its lvalue afterwards (irg_asm_out_store), so a
     * local it writes is not address-taken. The first is this asm's
     * own dst; each further one is a continuation right after it
     * (ir_asm.cont). Anything else -- "+", "m", a wider or other type
     * -- is written through the lvalue's address, as before. */
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
    /* What the asm may change: every register used[] now holds (the
     * operands', the clobbers', the template's), the scratch an output
     * through an address is stored with, and, if the template calls or
     * traps, everything a call changes -- r0-r3, r12 and lr. A value
     * live across the asm keeps out of exactly these (regalloc.c). */
    ia->scr = -1;
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].val && !ia->out[k].mem && ia->scr < 0)
            for (unsigned q = 0; q < sizeof t_scr_pool / sizeof t_scr_pool[0];
                 q++)
                if (!used[t_scr_pool[q]]) {
                    ia->scr = t_scr_pool[q];
                    break;
                }
    if (ia->scr >= 0)
        used[ia->scr] = 1;
    if (calls)
        for (int r = 0; r < 16; r++)
            if (r <= 3 || r == 12 || r == 14)
                used[r] = 1;
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

/* What the IR_ADD..IR_CMP lowerings in codegen.c encode as an immediate without
 * building the constant in a register first -- the answer the optimizer
 * asks for before folding one (target_imm_foldable). It must say what
 * those lowerings DO: anything else is folded and then rebuilt at each
 * use, which is exactly what asking avoids.
 *
 * HERE rather than beside them because the optimizer asks it, and embls
 * links the optimizer without the code generator. */
/* ...and for a 64-bit AND, OR or XOR, which the code generator does half
 * by half (logic_half): a half it takes without building it is all ones
 * or zero (a copy, a zero, a mvn), a modified immediate or the
 * complement of one (bic, orn), or a mask of low bits (ubfx). Both halves
 * must be one of those, or the constant stays in a register pair, built
 * once and hoistable, rather than half of it being rebuilt at each use. */
static int thumb_half_ok(int op, unsigned long c)
{
    unsigned long nc = ~c & 0xffffffffUL;
    if (c == 0 || c == 0xffffffffUL)
        return 1;
    if (t_imm_ok((long)c))
        return 1;
    if ((op == IR_AND || op == IR_OR) && t_imm_ok((long)nc))
        return 1;
    return op == IR_AND && (c & (c + 1)) == 0;
}
int thumb_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    return thumb_half_ok(op, u & 0xffffffffUL) &&
           thumb_half_ok(op, (u >> 32) & 0xffffffffUL);
}
/* A 64-bit compare with constant K, as cmp64 in codegen.c lowers it
 * without building K in a register pair. Three forms, each half of the
 * constant a modified immediate:
 *
 *   1  `subs; sbcs` of a - K, for `<` and `>=` -- and for `>` and `<=`
 *      as `>= K + 1` and `< K + 1`, unless K is the type's maximum;
 *   2  `rsbs; mvn; adcs` of K - a, for `>` (K - a borrows) and `<=` --
 *      and for `<` and `>=` as `<= K - 1` and `> K - 1`. Thumb-2 has no
 *      reverse subtract with carry, but SBC is AddWithCarry(x, ~y, C),
 *      so `adcs` of ~a's high half and K's leaves the same flags;
 *   3  `cmp lo; it eq; cmpeq hi`, for `==` and `!=`.
 *
 * Returns the form, 0 when K must be in registers; the predicate to read
 * off the flags of the subtraction made and K's halves (K + 1's, K - 1's)
 * come back through the pointers. The optimizer folds K into the compare
 * exactly when this says yes, and cmp64 asks again, so the two cannot
 * disagree. */
int thumb_cmp64_imm(int pred, int sign, long imm, int *pout, long *lo,
                    long *hi)
{
    unsigned long long k = (unsigned long long)imm, k2;
    unsigned long long max = sign ? 0x7fffffffffffffffULL : ~0ULL;
    unsigned long long min = sign ? 0x8000000000000000ULL : 0;
    int strict = pred == B_GT || pred == B_LE, p2;
    if (pred == B_EQ || pred == B_NE) {
        *lo = (long)(unsigned)k;
        *hi = (long)(unsigned)(k >> 32);
        *pout = pred;
        return t_imm_ok(*lo) && t_imm_ok(*hi) ? 3 : 0;
    }
    if (pred != B_LT && pred != B_GE && !strict)
        return 0;
    /* form 1: a - K', read as `<` or `>=` */
    if (!strict || k != max) {
        k2 = strict ? k + 1 : k;
        *lo = (long)(unsigned)k2;
        *hi = (long)(unsigned)(k2 >> 32);
        *pout = pred == B_GT || pred == B_GE ? B_GE : B_LT;
        if (t_imm_ok(*lo) && t_imm_ok(*hi))
            return 1;
    }
    /* form 2: K' - a, `a > K'` being `K' - a < 0` */
    if (strict || k != min) {
        k2 = strict ? k : k - 1;
        p2 = pred == B_GT || pred == B_GE ? B_LT : B_GE;
        *lo = (long)(unsigned)k2;
        *hi = (long)(unsigned)(k2 >> 32);
        *pout = p2;
        if (t_imm_ok(*lo) && t_imm_ok(*hi))
            return 2;
    }
    return 0;
}
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
