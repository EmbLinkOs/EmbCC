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
 * A `float` arrives as a double. A struct is its own bytes, by value.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int irg_va_arg_xtensa(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt);
    struct type *word = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);
    long size4;
    int apa, ndx, newndx, sum, arr, l_stk, l_have, l_over, addr;
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (flt && rt->kind == TY_FLOAT) {          /* promoted to double */
        size = 8;
        align = 8;
    }
    if (align > 16)
        align = 16;
    size4 = (size + 3) & ~3L;

    apa = gen_addr(fn, e->lhs);
    ndx = emit_load(fn, emit_bin(fn, IR_ADD, apa, emit_const(fn, 8, 4), 4, 1),
                    word);
    irg_mark_natural(fn, e->lhs);
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
    irg_mark_natural(fn, e->lhs);
    emit_jmp(fn, l_over);
    /* on the stack: one that straddled the register words is wholly
     * there, at index 32 */
    emit_label(fn, l_stk);
    emit_brz(fn, emit_cmp(fn, B_LE, ndx, emit_const(fn, 24, 4), 4, 1), 4,
             l_have);
    emit_mov(fn, newndx, emit_const(fn, 32 + size4, 4));
    emit_label(fn, l_have);
    emit_mov(fn, arr, emit_load(fn, apa, word));
    irg_mark_natural(fn, e->lhs);
    emit_label(fn, l_over);
    emit_store(fn, emit_bin(fn, IR_ADD, apa, emit_const(fn, 8, 4), 4, 1),
               newndx, word);
    irg_mark_natural(fn, e->lhs);
    sum = emit_bin(fn, IR_ADD, arr,
                   emit_bin(fn, IR_SUB, newndx, emit_const(fn, size4, 4), 4,
                            1), 4, 1);
    addr = new_temp(fn);
    emit_mov(fn, addr, sum);

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
