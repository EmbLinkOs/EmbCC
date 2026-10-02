/* AVR's inline-asm lowering.
 *
 * A kernel needs this on every target -- a critical section is `cli`, a
 * context switch is a register save, and neither has a C spelling. On AVR it
 * is needed for one more reason: the machine's I/O space is memory-mapped,
 * so most peripherals are reachable from C, but SREG's interrupt bit and the
 * stack pointer are not things a C expression can touch safely.
 *
 * ---- the constraint letters ------------------------------------------
 *
 * AVR's are its own, and they exist because the register file is NOT
 * uniform. Nearly every restriction in the instruction set shows up as a
 * letter here, which is why there are so many:
 *
 *   r   any register
 *   d   r16-r31 -- the half the immediate instructions (ldi, subi, andi,
 *       cpi) can reach. This is the single most important one: `ldi` cannot
 *       load a constant into r15.
 *   a   r16-r23
 *   w   r24, r26, r28 or r30 -- the four pairs adiw and sbiw reach
 *   e   X, Y or Z -- the three pairs that can address memory at all
 *   b   Y or Z -- and of those, the two that take a displacement
 *   x/y/z  that specific pointer pair
 *   q   the stack pointer
 *   I   0..63 (an adiw/in/out immediate)      M   0..255 (an ldi one)
 *   i/n an integer constant
 *
 * An allocatable operand is drawn from what THIS backend does not need
 * across an asm. Everything is in a frame slot here, so that is almost
 * everything: r18-r27 and r30-r31, plus r0. Not r1, whose zero is an
 * invariant of every AVR object in the program, and not r28/r29, which are
 * the frame pointer this function is addressing its own locals through.
 *
 * ---- what is refused -------------------------------------------------
 *
 * A callee-saved register (r2-r17) named by a template or a clobber list.
 * EmbCC saves nothing around an asm, so naming one would corrupt the
 * caller -- and silently, because the asm itself would work. The message
 * names the register.
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

/* What an allocatable operand may use: everything this backend does not
 * need to survive an asm. Ordered so that the common case takes a register
 * the immediate instructions can also reach, which is what a template
 * written with "d" in mind expects even when it says "r". */
static const int avr_asm_pool[] = {
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 30, 31
};

/* Is this register one EmbCC does not save around an asm? r2-r17 are
 * callee-saved and nothing here preserves them; r1 is the zero register and
 * r28/r29 are the frame pointer. */
static const char *avr_reg_forbidden(int r)
{
    /* r1 is NOT forbidden, and that is deliberate. It is the zero register,
     * but `mul` destroys it -- so a template that multiplies must end with
     * `clr r1`, and refusing to let a template name r1 would forbid the one
     * idiom that makes the machine's multiply reachable from asm at all.
     * avr-gcc takes the same position: naming r1 is allowed and LEAVING IT
     * NON-ZERO is the author's bug, which no compiler can check.
     *
     * r0 is likewise fair game: it is the ABI's scratch. */
    if (r == 28 || r == 29)
        return "half of Y, the frame pointer this function reaches its own "
               "locals through";
    if (r >= 2 && r <= 17)
        return "callee-saved, and EmbCC saves nothing around an asm";
    return NULL;
}

/* The name to substitute for a register. Always rN: X, Y and Z are spelled
 * that way in an instruction that takes a pointer, and a template using one
 * pins it with "x"/"y"/"z" rather than through an operand. */
static const char *avr_reg_name(int r)
{
    static const char *n[32] = {
        "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11",
        "r12","r13","r14","r15","r16","r17","r18","r19","r20","r21","r22",
        "r23","r24","r25","r26","r27","r28","r29","r30","r31"
    };
    return r >= 0 && r < 32 ? n[r] : "?";
}

/* Does `r` satisfy the constraint? The letters are checked in the order a
 * narrower class beats a wider one, so "rd" behaves as "d". */
static int avr_class_ok(const char *c, int r)
{
    int saw = 0;
    for (const char *p = c; *p; p++) {
        switch (*p) {
        case 'd': saw = 1; if (r >= 16) return 1; break;
        case 'a': saw = 1; if (r >= 16 && r <= 23) return 1; break;
        case 'w': saw = 1; if (r == 24 || r == 26 || r == 30) return 1; break;
        case 'e': saw = 1; if (r == 26 || r == 30) return 1; break;
        case 'b': saw = 1; if (r == 30) return 1; break;
        case 'r': case 'g': case 'q': case 'm':
            saw = 1; return 1;
        default: break;
        }
    }
    return !saw;                  /* no class letter: anything will do */
}

static void avr_mark_template_regs(const char *file, int line,
                                   const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; ) {
        if (isalpha((unsigned char)*p) &&
            (p == tmpl || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                            p[-1] == '%'))) {
            int n = 1;
            while (isalnum((unsigned char)p[n]) || p[n] == '_')
                n++;
            int r = avrasm_gpr(p, n);
            if (r >= 0) {
                const char *why = avr_reg_forbidden(r);
                if (why)
                    diag_fatal(file, line,
                               "AVR asm names register '%.*s', which is %s",
                               n, p, why);
                used[r] = 1;
                /* A pointer pair named as X, Y or Z uses BOTH halves. */
                if (n == 1 && (r == 26 || r == 28 || r == 30))
                    used[r + 1] = 1;
            }
            p += n;
            continue;
        }
        p++;
    }
}

static char *avr_subst(const char *file, int line, const char *tmpl,
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
        /* GCC's AVR modifiers name a HALF of a multi-register operand: %A0
         * is its lowest byte, %B0 the next, and so on. They are the normal
         * way a template handles a 16-bit value on an 8-bit machine, so
         * they are here rather than refused. */
        int half = -1, as_ptr = 0;
        if (*p >= 'A' && *p <= 'D' && (p[1] == '[' || isdigit((unsigned char)p[1]))) {
            half = *p - 'A';
            p++;
        } else if (*p == 'a' && (p[1] == '[' || isdigit((unsigned char)p[1]))) {
            /* %a names the operand as a POINTER: X, Y or Z rather than the
             * number of its low register. An instruction that addresses
             * memory is written `ld rd, X` and will not assemble with `r26`
             * in that place, so a template taking an "e" operand has no
             * other way to spell it. */
            as_ptr = 1;
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
                                   "supported for AVR (%%A..%%D name the "
                                   "bytes of a wider operand)",
                       *p ? *p : ' ');
        }
        if (isimm[k]) {
            long v = imms[k];
            if (half > 0)
                v = (v >> (8 * half)) & 0xff;
            len += (size_t)snprintf(out + len, cap - len, "%ld", v);
        } else if (as_ptr) {
            const char *nm = regs[k] == 26 ? "X" : regs[k] == 28 ? "Y"
                           : regs[k] == 30 ? "Z" : NULL;
            if (!nm)
                diag_fatal(file, line,
                           "%%a names operand %%%d as a pointer, but it is in "
                           "%s -- only X, Y and Z address memory, which is "
                           "what the \"e\" and \"b\" constraints are for",
                           k, avr_reg_name(regs[k]));
            len += (size_t)snprintf(out + len, cap - len, "%s", nm);
        } else {
            /* %B0 of a register operand is the next register up: a 16-bit
             * value lives in a PAIR here, low byte first. */
            len += (size_t)snprintf(out + len, cap - len, "%s",
                                    avr_reg_name(regs[k] +
                                                 (half > 0 ? half : 0)));
        }
    }
    out[len] = '\0';
    return out;
}

/* ---- varargs ----------------------------------------------------------
 *
 * AVR's variadic convention is the simplest of any target here, and that is
 * measured rather than assumed: for a variadic call ALL arguments go on the
 * stack, including the NAMED ones. `sum(3, 10, 20, 30)` writes all four
 * words to the outgoing area and puts nothing in a register.
 *
 * So a va_list is a bare POINTER at the next argument -- the same
 * representation AAPCS32 and the RISC-V psABI use -- and there is no
 * register-save area, no record to walk, and no split point to track.
 *
 * Arguments are PACKED at their natural size, with no rounding and no
 * alignment: the stack is byte-addressed here and nothing on this machine
 * wants more. That is the same rule the non-variadic stack arguments follow.
 *
 * One promotion still applies, and it is the C standard's rather than the
 * ABI's: a `float` passed through `...` is promoted to `double`. On this
 * target both are four-byte IEEE single, so the promotion is a no-op -- the
 * one place AVR's unusual `double` makes something simpler.
 */
int irg_va_arg_avr(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    long size = ty_size(rt);
    /* The list is a pointer, and a pointer here is TWO bytes -- so the
     * temporary holding it is an `int` by AVR's data model, not the
     * four-byte one the 32-bit targets use. */
    struct type *ptr = ty_base(TY_INT, 1);

    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    int addr = new_temp(fn);

    emit_mov(fn, addr, cur);
    /* Packed: no rounding up. */
    emit_store(fn, apa,
               emit_bin(fn, IR_ADD, addr,
                        emit_const(fn, size, target_ptr_size()),
                        target_ptr_size(), 1),
               ptr);
    return emit_load(fn, addr, rt);
}

void irg_asm_avr(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *file = fn->file;
    int nops = a->nout + a->nin;
    int regs[2 * MAX_PARAMS], isimm[2 * MAX_PARAMS], sizes[2 * MAX_PARAMS];
    long imms[2 * MAX_PARAMS];
    const char *cons[2 * MAX_PARAMS];
    const char *names[2 * MAX_PARAMS];
    int used[32] = { 0 };

    /* Operands are numbered outputs first, then inputs, as gcc does. */
    for (int i = 0; i < nops; i++) {
        struct asm_operand *op = i < a->nout ? &a->out[i] : &a->in[i - a->nout];
        if (op->reg == ASM_REG_INVALID)
            diag_fatal(file, s->line, "asm constraint \"%s\" is not valid for "
                                      "AVR", op->constraint);
        if (op->reg == ASM_REG_IMM && i < a->nout)
            diag_fatal(file, s->line, "an asm output cannot be an immediate");
        regs[i] = op->reg;
        isimm[i] = op->reg == ASM_REG_IMM;
        imms[i] = op->imm;
        sizes[i] = ty_size(op->expr->ty);
        names[i] = op->name;
        cons[i] = op->constraint;
        if (op->reg >= 0) {
            const char *why = avr_reg_forbidden(op->reg);
            if (why)
                diag_fatal(file, s->line,
                           "an asm operand is pinned to '%s', which is %s",
                           avr_reg_name(op->reg), why);
            used[op->reg] = 1;
        }
    }
    for (int i = 0; i < a->nclob; i++) {
        int r = avrasm_gpr(a->clob[i], (int)strlen(a->clob[i]));
        if (r >= 0) {
            const char *why = avr_reg_forbidden(r);
            if (why)
                diag_fatal(file, s->line,
                           "AVR asm clobbers register '%s', which is %s",
                           a->clob[i], why);
            used[r] = 1;
            if (strlen(a->clob[i]) == 1 && (r == 26 || r == 28 || r == 30))
                used[r + 1] = 1;
        }
    }
    avr_mark_template_regs(file, s->line, a->tmpl, used);

    /* Allocate the operands that asked for a class rather than a register.
     * A value wider than one byte needs a RUN, and an even-aligned one when
     * it is a pointer pair -- which is the whole reason this loop is not
     * "take the next free register". */
    for (int i = 0; i < nops; i++) {
        int need, r = -1;
        if (regs[i] != -2)
            continue;
        need = sizes[i] < 1 ? 1 : sizes[i];
        if (need > 4)
            diag_fatal(file, s->line,
                       "an asm operand of %d bytes needs %d consecutive "
                       "registers, which is more than this backend keeps "
                       "free across an asm", need, need);
        for (unsigned k = 0; k < sizeof avr_asm_pool / sizeof avr_asm_pool[0];
             k++) {
            int base = avr_asm_pool[k], j, ok = 1;
            if (need > 1 && (base & 1))
                continue;              /* a pair starts on an even register */
            if (!avr_class_ok(cons[i], base))
                continue;
            for (j = 0; j < need; j++)
                if (base + j > 31 || base + j <= 1 || used[base + j] ||
                    avr_reg_forbidden(base + j)) { ok = 0; break; }
            if (ok) { r = base; break; }
        }
        if (r < 0)
            diag_fatal(file, s->line,
                       "no register satisfying \"%s\" is free for an asm "
                       "operand of %d byte%s", cons[i], need,
                       need == 1 ? "" : "s");
        for (int j = 0; j < need; j++)
            used[r + j] = 1;
        regs[i] = r;
    }

    char *text = avr_subst(file, s->line, a->tmpl, regs, imms, isimm,
                           names, nops);
    struct code c = { 0 };
    char err[512];
    if (avrasm_assemble(text, &c, err, sizeof err) != 0)
        diag_fatal(file, s->line, "%s (assembling \"%s\")", err, text);
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
        /* An "m" operand names MEMORY: the register carries its ADDRESS and
         * the template dereferences it. A pointer here is two bytes. */
        o->mem = strchr(a->in[i].constraint, 'm') != NULL;
        o->temp = o->mem ? gen_addr(fn, a->in[i].expr)
                         : gen_expr(fn, a->in[i].expr);
        o->size = o->mem ? target_ptr_size() : sizes[a->nout + i];
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
