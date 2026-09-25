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
