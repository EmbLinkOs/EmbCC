/* ColdFire's share of IR generation: va_arg, which constants the optimizer
 * may fold into an instruction, and inline asm (refused).
 *
 * The m68k convention passes every argument on the stack in whole words,
 * the unnamed ones of a variadic call exactly as named ones, so a va_list
 * is a `char *` walking the caller's argument words: each argument takes
 * its size rounded up to a word (a long long or double two), and one
 * smaller than a word -- only a composite, since everything narrower than
 * int is promoted -- is right-justified in its word, big-endian
 * (docs/internals/coldfire-plan.md).
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"

int irg_va_arg_coldfire(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    long size = ty_size(rt);
    long words = (size + 3) & ~3L;
    struct type *ptr = ty_int_of_size(4, 1);
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    int addr = new_temp(fn);

    if (size > 8 && rt->kind != TY_STRUCT)
        diag_fatal(fn->file, e->line, "va_arg of a %ld-byte scalar is not "
                   "supported for %s", size, target_triple_now());
    fn->ins[fn->nins - 1].natural = 1;
    /* the value's first byte: right-justified when narrower than a word */
    emit_mov(fn, addr, size < 4 ? emit_bin(fn, IR_ADD, cur,
                                           emit_const(fn, 4 - size, 4), 4, 1)
                                : cur);
    emit_store(fn, apa,
               emit_bin(fn, IR_ADD, cur, emit_const(fn, words, 4), 4, 1),
               ptr);
    fn->ins[fn->nins - 1].natural = 1;
    if (rt->kind == TY_STRUCT) {        /* its bytes, into its own slot */
        int dst = irg_va_struct_slot(fn, e);
        irg_va_copy(fn, dst, 0, addr, size);
        return dst;
    }
    return emit_load(fn, addr, rt);
}

/* Every 32-bit constant is an operand on ColdFire -- addi, subi, andi,
 * ori, eori and cmpi take #imm32 into a data register, and the code
 * generator builds a multiplier in d1 -- and a 64-bit AND/OR/XOR/ADD/SUB
 * takes each half as one too. Here rather than beside the lowering
 * because embls links the optimizer without the code generator. */
int cf_imm_foldable(int op, long imm, int w)
{
    (void)imm;
    if (w == 8)
        return op == IR_AND || op == IR_OR || op == IR_XOR ||
               op == IR_ADD || op == IR_SUB || op == IR_CMP;
    return op == IR_ADD || op == IR_SUB || op == IR_AND || op == IR_OR ||
           op == IR_XOR || op == IR_CMP || op == IR_MUL;
}

/* Inline assembly has no ColdFire vocabulary yet: refused by name rather
 * than assembled by another target's. */
void irg_asm_coldfire(struct ir_func *fn, struct stmt *s)
{
    diag_fatal(fn->file, s->line, "inline assembly is not supported for %s "
               "yet: EmbCC has no ColdFire assembler", target_triple_now());
}
