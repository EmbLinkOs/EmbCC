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
#include "../../sema/type.h"

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

    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);

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
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, wb),
                            wb, 1),
                   ptr);

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
