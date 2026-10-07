/* LoongArch64's share of irgen: the immediates the optimizer may fold,
 * and inline assembly, which is refused by name.
 *
 * The variadic lowering is RISC-V's (irg_va_arg_riscv, src/arch/riscv/
 * irgen.c): the LoongArch psABI passes a variadic argument exactly where
 * RV64's LP64 does -- a0-a7 and then the stack, a 2*GRLEN-aligned scalar
 * in an even-aligned register pair, more than two registers by
 * reference -- and its va_list is the same bare pointer, so that routine,
 * which reads only the pointer width, is right here too (read off clang:
 * `v(0, x)` with an __int128 x puts x in a2:a3). */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../driver/util.h"

/* What the IR_ADD..IR_CMP lowerings in codegen.c take as an immediate
 * without building the constant first -- the optimizer asks before
 * folding one (opt.c's target_imm_foldable). Anything else is folded and
 * then rebuilt at every use, where a value left in a register is built
 * once and can be hoisted out of a loop.
 *
 * add takes a SIGNED 12-bit immediate and sub is an add of the negation;
 * and, or and xor take an UNSIGNED one, 0..4095 -- the difference from
 * RISC-V that matters here. A compare's constant is free when it is zero
 * (r0); mul has no immediate at all. HERE rather than beside the
 * lowerings because embls links the optimizer without the code
 * generator. */
int la_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD:
        return imm >= -2048 && imm <= 2047;
    case IR_SUB:
        return imm >= -2047 && imm <= 2048;
    case IR_AND: case IR_OR: case IR_XOR:
        return imm >= 0 && imm <= 4095;
    case IR_CMP:
        return imm == 0;
    default:
        return 0;
    }
}

void irg_asm_loongarch(struct ir_func *fn, struct stmt *s)
{
    diag_fatal(fn->file, s->line,
               "inline assembly is not supported for %s yet: EmbCC has no "
               "LoongArch assembler vocabulary, and guessing an encoding "
               "is not an option", target_triple_now());
}
