/* Renesas RX's share of IR generation: va_arg.
 *
 * GCC's RX convention puts every unnamed argument -- and the last named
 * one -- on the stack, each at its type's alignment (at most 4) and
 * taking its own size (docs/internals/rx-plan.md). So a va_list is a
 * bare pointer walking the caller's stack block: va_arg rounds it up to
 * the type's alignment, reads the value there, and steps by the size.
 * The default promotions mean an unnamed scalar is at least an int, and
 * a `float` arrives as a `double` -- which is the same binary32 here. A
 * struct is its own bytes, by value.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"

int irg_va_arg_rx(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    long align = ty_align(rt);

    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (align > 4)
        align = 4;
    if (align > 1)
        cur = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, cur,
                                emit_const(fn, align - 1, 4), 4, 1),
                       emit_const(fn, -align, 4), 4, 1);
    {
        int addr = new_temp(fn);
        emit_mov(fn, addr, cur);
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, size, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (sdst >= 0) {
            irg_va_copy(fn, sdst, 0, addr, size);
            return sdst;
        }
        return emit_load(fn, addr, rt);
    }
}
