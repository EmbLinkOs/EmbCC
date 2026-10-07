/* TriCore's share of IR generation: va_arg, inline asm (refused), and
 * which constants the optimizer may fold into an instruction.
 *
 * The TriCore EABI (as remembered, docs/internals/tricore-plan.md) passes
 * every UNNAMED argument of a variadic call on the stack, in whole words
 * from the caller's stack pointer, after any named ones there. So a
 * va_list is a bare pointer walking those words: va_arg reads at it and
 * steps by the size in whole words -- an 8-byte value is only 4-aligned
 * -- a `float` arrives as a `double`, and a struct larger than 8 bytes
 * arrives as the address of the caller's copy.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"

int irg_va_arg_tricore(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    int byref = rt->kind == TY_STRUCT && size > 8;
    long step;

    /* Every access here is natural (ir_ins.natural): the va_list is a
     * pointer object, as aligned as its lvalue is, and each argument
     * slot is whole words. */
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (flt && rt->kind == TY_FLOAT)            /* promoted to double */
        size = 8;
    {
        int addr = new_temp(fn);
        emit_mov(fn, addr, cur);
        step = byref ? 4 : (size + 3) & ~3L;
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (sdst >= 0) {
            int src = addr;
            if (byref) {
                src = emit_load(fn, addr, ptr);
                fn->ins[fn->nins - 1].natural = 1;
            }
            irg_va_copy(fn, sdst, 0, src, ty_size(rt));
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
}

/* Inline asm needs a TriCore assembler for its templates, which EmbCC
 * does not have yet: refused by name rather than emitted wrongly. */
void irg_asm_tricore(struct ir_func *fn, struct stmt *s)
{
    diag_fatal(fn->file, s->line,
               "inline asm is not supported for tricore-none-elf yet: "
               "EmbCC has no TriCore assembler for the template");
}

/* What codegen.c takes as an immediate without building it: ADDI's
 * SIGNED 16 bits for add (and a subtraction is an add of the negation);
 * the RC form's const9 -- ZERO-extended for AND, OR and XOR, signed for
 * MUL; for a compare, what fits every form a compare can take, the RC
 * field signed or unsigned and the k + 1 a GT or LE becomes. Anything
 * else stays in a register, built once and hoistable, rather than
 * rebuilt at each use. Here rather than beside the lowering because embls
 * links the optimizer without the code generator. */
int tc_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD:
        return imm >= -32768 && imm <= 32767;
    case IR_SUB:
        return imm >= -32767 && imm <= 32768;
    case IR_AND: case IR_OR: case IR_XOR:
        return imm >= 0 && imm <= 511;
    case IR_MUL:
        return imm >= -256 && imm <= 255;
    case IR_CMP:
        return imm >= 0 && imm <= 254;
    default:
        return 0;
    }
}
