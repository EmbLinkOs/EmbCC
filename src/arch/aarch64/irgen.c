/* aarch64's share of IR generation: the AAPCS64 va_arg walk and extended asm
 * through the aarch64 inline-asm vocabulary (asm.c). Built from irgen's
 * helpers (src/ir/irgen_int.h). */
#include "../../ir/irgen_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../driver/util.h"
#include "../../sema/sema.h"
#include "../../sema/type.h"
#include "asm.h"

/* va_arg for AAPCS64. The record va_start fills (aarch64/codegen.c
 * IR_VA_START): __stack at +0, __gr_top +8, __vr_top +16, __gr_offs +24,
 * __vr_offs +28. gcc's sequence, exactly: if the offset is already >= 0 the
 * register save area is spent; otherwise advance it by one slot (8 for x,
 * 16 for v) and, if that carried it past 0, the argument did not fit either;
 * else the value is at top + the OLD offset. The stack path reads __stack
 * and advances it by 8. A variadic float arrives promoted to double. */
int irg_va_arg_aapcs(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    struct type *s32 = ty_base(TY_INT, 0);
    struct type *ptr = ty_base(TY_LONG, 1);
    int ap = gen_expr(fn, e->lhs);

    int a_offs = emit_bin(fn, IR_ADD, ap, emit_const(fn, flt ? 28 : 24, 8), 8, 1);
    int a_top = emit_bin(fn, IR_ADD, ap, emit_const(fn, flt ? 16 : 8, 8), 8, 1);
    int addr = new_temp(fn);
    int l_stack = new_label(fn), l_done = new_label(fn);

    /* __int128: an even register pair (C.8: the offset rounded up to 16,
     * as the save area's x0 is at -64), 16 bytes of it */
    int i128 = rt->kind == TY_INT128;
    int offs = emit_load(fn, a_offs, s32);
    emit_brnz(fn, emit_cmp(fn, B_GE, offs, emit_const(fn, 0, 4), 4, 1), 4,
              l_stack);                                  /* already spent */
    if (i128)
        offs = emit_bin(fn, IR_AND,
                        emit_bin(fn, IR_ADD, offs, emit_const(fn, 15, 4), 4, 1),
                        emit_const(fn, -16, 4), 4, 1);
    int next = emit_bin(fn, IR_ADD, offs,
                        emit_const(fn, flt || i128 ? 16 : 8, 4), 4, 1);
    emit_store(fn, a_offs, next, s32);
    emit_brnz(fn, emit_cmp(fn, B_GT, next, emit_const(fn, 0, 4), 4, 1), 4,
              l_stack);                                  /* did not fit */
    int top = emit_load(fn, a_top, ptr);
    int off64 = gen_convert(fn, offs, s32, ty_base(TY_LONG, 0));
    emit_mov(fn, addr, emit_bin(fn, IR_ADD, top, off64, 8, 1));
    emit_jmp(fn, l_done);

    emit_label(fn, l_stack);
    int stk = emit_load(fn, ap, ptr);
    int ssz = 8;
    if (rt->kind == TY_LDOUBLE || i128) {  /* a 16-aligned stack slot of 16 */
        stk = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, stk, emit_const(fn, 15, 8), 8, 1),
                       emit_const(fn, -16, 8), 8, 1);
        ssz = 16;
    }
    emit_mov(fn, addr, stk);
    emit_store(fn, ap, emit_bin(fn, IR_ADD, stk, emit_const(fn, ssz, 8), 8, 1),
               ptr);
    emit_label(fn, l_done);

    if (rt->kind == TY_LDOUBLE)     /* a whole v register's slot */
        return emit_load(fn, addr, rt);
    if (flt) {
        int v = emit_load(fn, addr, ty_base(TY_DOUBLE, 0));
        if (rt->kind == TY_FLOAT) {
            struct ir_ins *cv = emit(fn);
            cv->op = IR_F2F; cv->a = v; cv->size = 8; cv->w = 4;
            cv->dst = new_temp(fn);
            return cv->dst;
        }
        return v;
    }
    return emit_load(fn, addr, rt);
}


/* ---- aarch64 extended asm --------------------------------------------- */

/* Registers an "r" operand may be given, in preference order. Excluded:
 * x12 (the codegen's address scratch — a far stack slot is reached through
 * it, which would clobber an operand already loaded there), x16..x18 (IP0,
 * IP1, the platform register), and x19 upward, which are callee-saved and
 * which EmbCC does not save around an asm. x0..x7 come last because
 * register-asm variables for PSCI and semihosting calls live there. */
static const int a64_asm_pool[] = { 9, 10, 11, 13, 14, 15,
                                    0, 1, 2, 3, 4, 5, 6, 7, 8 };

/* A register the template NAMES outright is off limits to allocation, and one
 * that is callee-saved is refused: writing it would corrupt the caller. */
static void a64_mark_template_regs(const char *file, int line,
                                   const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; ) {
        /* '%' before it makes it an operand reference (%w0, %x1), not a
         * register the template names — those are resolved separately. */
        if ((*p == 'x' || *p == 'w') &&
            (p == tmpl || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                            p[-1] == '%'))) {
            int n = 1;
            while (isalnum((unsigned char)p[n]) || p[n] == '_')
                n++;
            int r = a64asm_gpr(p, n);
            if (r >= 0 && r < 31) {
                if (r >= 19)
                    diag_fatal(file, line,
                               "aarch64 asm names callee-saved register '%.*s', "
                               "which EmbCC does not save around an asm", n, p);
                used[r] = 1;
            }
            p += n;
            continue;
        }
        p++;
    }
}

/* Substitutes operands into the template: %N, %wN, %xN, %[name] and the
 * w/x-modified named forms, and %% for a literal percent. A register operand
 * prints as its W or X name — the modifier decides if given, else the
 * operand's own width, exactly as gcc does — and an immediate prints as its
 * value. */
static char *a64_subst(const char *file, int line, const char *tmpl,
                       const int *regs, const long *imms, const int *isimm,
                       const int *sizes, const char *const *names, int nops)
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
        char mod = 0;
        if (*p == 'w' || *p == 'x') {
            mod = *p;
            p++;
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
                                   "supported for aarch64", *p ? *p : ' ');
        }
        if (isimm[k]) {
            if (mod)
                diag_fatal(file, line, "a %%%c modifier on an immediate asm "
                                       "operand makes no sense", mod);
            len += (size_t)snprintf(out + len, cap - len, "%ld", imms[k]);
        } else {
            char form = mod ? mod : (sizes[k] > 4 ? 'x' : 'w');
            len += (size_t)snprintf(out + len, cap - len, "%c%d", form, regs[k]);
        }
    }
    out[len] = '\0';
    return out;
}

void irg_asm_arm64(struct ir_func *fn, struct stmt *s)
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
                                      "aarch64", op->constraint);
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
        int r = a64asm_gpr(a->clob[i], (int)strlen(a->clob[i]));
        if (r >= 19 && r < 31)
            diag_fatal(file, s->line, "aarch64 asm clobbers callee-saved "
                                      "register '%s', which EmbCC does not "
                                      "save around an asm", a->clob[i]);
        if (r >= 0 && r < 31)
            used[r] = 1;
    }
    a64_mark_template_regs(file, s->line, a->tmpl, used);

    for (int i = 0; i < nops; i++) {
        if (regs[i] != -2)
            continue;
        int r = -1;
        for (unsigned k = 0; k < sizeof a64_asm_pool / sizeof a64_asm_pool[0]; k++)
            if (!used[a64_asm_pool[k]]) {
                r = a64_asm_pool[k];
                break;
            }
        if (r < 0)
            diag_fatal(file, s->line, "no free register for an asm operand");
        used[r] = 1;
        regs[i] = r;
    }

    char *text = a64_subst(file, s->line, a->tmpl, regs, imms, isimm, sizes,
                           names, nops);
    struct code c = { 0, 0, 0 };
    char err[512];
    if (a64asm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s", err);
    free(text);

    /* Immediates were consumed by the template and carry no run-time value,
     * so only register operands become IR operands. */
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
        o->temp = gen_expr(fn, a->in[i].expr);
        o->size = 8;                  /* a temp holds the promoted value */
    }
    for (int i = 0; i < a->nout; i++) {
        struct ir_asm_op *o = &ia->out[ia->nout++];
        o->reg = regs[i];
        o->temp = gen_addr(fn, a->out[i].expr);
        o->size = sizes[i];
        o->inout = strchr(a->out[i].constraint, '+') != NULL;
    }
    struct ir_ins *ins = emit(fn);
    ins->op = IR_ASM;
    ins->asm_ir = ia;
}

/* Innermost enclosing loop's exit and continue targets; sema already
 * rejected break/continue outside any loop. */