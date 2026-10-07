/* 32-bit PowerPC's share of IR generation: va_arg, and which constants the
 * optimizer may fold into an instruction.
 *
 * The SVR4/EABI va_list is a 12-byte record, which va_start builds in the
 * callee's frame (codegen.c) and `va_list` points at:
 *
 *   +0  gpr                 how many of r3..r10 the arguments read so far
 *                           used (an unsigned char)
 *   +1  fpr                 the same for f1..f8 -- always 0, soft float
 *   +4  overflow_arg_area   the next argument on the caller's stack
 *   +8  reg_save_area       r3..r10 as the prologue stored them
 *
 * An argument of one word comes from the save area while gpr < 8; a
 * long long or (soft) double takes an ODD-numbered pair, so gpr is rounded
 * up to even first and the pair must fit by 8. Once one does not fit, gpr
 * becomes 8 -- nothing later is read from registers -- and the argument
 * comes from the overflow area, 8-aligned for an 8-byte one. A struct of
 * any size travels BY REFERENCE: its word is a pointer to the caller's
 * copy. A float arrives as a double. This is clang's walk for
 * powerpc-none-eabi, read off its code (docs/internals/powerpc-plan.md).
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"

int irg_va_arg_ppc(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    struct type *u8 = ty_int_of_size(1, 1);
    struct type *ptr = ty_int_of_size(4, 1);
    int is_struct = rt->kind == TY_STRUCT;
    int flt = !is_struct && ty_is_float(rt);
    int words = is_struct ? 1 : (flt || ty_size(rt) > 4) ? 2 : 1;
    int sdst = is_struct ? irg_va_struct_slot(fn, e) : -1;
    int ap = gen_expr(fn, e->lhs);              /* the record's address */
    int a_ova = emit_bin(fn, IR_ADD, ap, emit_const(fn, 4, 4), 4, 1);
    int a_rsa = emit_bin(fn, IR_ADD, ap, emit_const(fn, 8, 4), 4, 1);
    int addr = new_temp(fn);
    int l_over = new_label(fn), l_done = new_label(fn);
    int gpr, v;

    if (ty_size(rt) > 8 && !is_struct)
        diag_fatal(fn->file, e->line, "va_arg of a %d-byte scalar is not "
                   "supported for %s", ty_size(rt), target_triple_now());
    gpr = emit_load(fn, ap, u8);
    fn->ins[fn->nins - 1].natural = 1;
    if (words == 2)
        gpr = emit_bin(fn, IR_AND,
                       emit_bin(fn, IR_ADD, gpr, emit_const(fn, 1, 4), 4, 1),
                       emit_const(fn, -2, 4), 4, 1);
    emit_brz(fn, emit_cmp(fn, B_LT, gpr, emit_const(fn, 9 - words, 4), 4, 0),
             4, l_over);
    /* from the register save area: reg_save_area + 4 * gpr */
    {
        int rsa = emit_load(fn, a_rsa, ptr);
        fn->ins[fn->nins - 1].natural = 1;
        emit_mov(fn, addr,
                 emit_bin(fn, IR_ADD, rsa,
                          emit_bin(fn, IR_SHL, gpr, emit_const(fn, 2, 4), 4, 0),
                          4, 1));
        emit_store(fn, ap,
                   emit_bin(fn, IR_ADD, gpr, emit_const(fn, words, 4), 4, 1),
                   u8);
        fn->ins[fn->nins - 1].natural = 1;
        emit_jmp(fn, l_done);
    }
    /* from the overflow area, which every later argument comes from too */
    emit_label(fn, l_over);
    {
        int ova;
        emit_store(fn, ap, emit_const(fn, 8, 4), u8);
        fn->ins[fn->nins - 1].natural = 1;
        ova = emit_load(fn, a_ova, ptr);
        fn->ins[fn->nins - 1].natural = 1;
        if (words == 2)
            ova = emit_bin(fn, IR_AND,
                           emit_bin(fn, IR_ADD, ova, emit_const(fn, 7, 4), 4, 1),
                           emit_const(fn, -8, 4), 4, 1);
        emit_mov(fn, addr, ova);
        emit_store(fn, a_ova,
                   emit_bin(fn, IR_ADD, ova, emit_const(fn, 4 * words, 4), 4, 1),
                   ptr);
        fn->ins[fn->nins - 1].natural = 1;
    }
    emit_label(fn, l_done);

    if (is_struct) {
        /* the word is a pointer to the caller's copy */
        int src = emit_load(fn, addr, ptr);
        fn->ins[fn->nins - 1].natural = 1;
        irg_va_copy(fn, sdst, 0, src, ty_size(rt));
        return sdst;
    }
    if (flt) {
        v = emit_load(fn, addr, ty_base(TY_DOUBLE, 0));
        fn->ins[fn->nins - 1].natural = 1;
        if (rt->kind == TY_FLOAT) {               /* promoted to double */
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
    v = emit_load(fn, addr, rt);
    fn->ins[fn->nins - 1].natural = 1;
    return v;
}

/* What codegen.c takes as an immediate without building it: addi's SIGNED
 * 16 bits for add (a subtraction adds the negation) and mulli's for a
 * multiply; the UNSIGNED 16 bits of andi./ori/xori, or the same shifted up
 * by 16 (andis./oris/xoris), and for an AND any run of ones rlwinm masks;
 * for a compare 0..32767, which both cmpwi and cmplwi take. Here rather
 * than beside the lowering because embls links the optimizer without the
 * code generator. */
static int rlw_mask(unsigned long m)
{
    unsigned long n;
    m &= 0xffffffffUL;
    if (m == 0)
        return 0;
    /* a run of ones, or the complement of one (a mask that wraps) */
    n = m & 1 ? ~m & 0xffffffffUL : m;
    if (n == 0)
        return 1;
    while (!(n & 1))
        n >>= 1;
    return (n & (n + 1)) == 0;
}

int ppc_imm_foldable(int op, long imm)
{
    unsigned long u = (unsigned long)imm & 0xffffffffUL;
    switch (op) {
    case IR_ADD: case IR_MUL:
        return imm >= -32768 && imm <= 32767;
    case IR_SUB:
        return imm >= -32767 && imm <= 32768;
    case IR_OR: case IR_XOR:
        return (imm >= 0 && imm <= 0xffff) || (u & 0xffffUL) == 0;
    case IR_AND:
        return (imm >= 0 && imm <= 0xffff) || (u & 0xffffUL) == 0 ||
               rlw_mask(u);
    case IR_CMP:
        return imm >= 0 && imm <= 32767;
    default:
        return 0;
    }
}

/* A 64-bit AND/OR/XOR, which the code generator does half by half: each
 * half the identity, zero, all ones, or what the 32-bit form takes. */
static int half_ok(int op, unsigned long c)
{
    if (c == 0 || c == 0xffffffffUL)
        return 1;
    return ppc_imm_foldable(op == IR_AND ? IR_AND : IR_OR, (long)c);
}

int ppc_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    unsigned long lo = u & 0xffffffffUL, hi = (u >> 32) & 0xffffffffUL;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    return half_ok(op, lo) && half_ok(op, hi);
}

/* Inline assembly has no PowerPC vocabulary yet: refused by name rather
 * than assembled by another target's. */
void irg_asm_ppc(struct ir_func *fn, struct stmt *s)
{
    diag_fatal(fn->file, s->line, "inline assembly is not supported for %s "
               "yet: EmbCC has no PowerPC assembler", target_triple_now());
}
