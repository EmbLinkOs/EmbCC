/* SPARC V8's share of IR generation: va_arg, and which constants the
 * optimizer may fold into an instruction.
 *
 * The SPARC ABI passes a variadic argument exactly where a named one
 * would go -- word after word, the first six in %o0-%o5 -- and a variadic
 * callee stores %i0-%i5 into the home area its caller reserved at %fp+68,
 * so the whole argument list is one block of words and a va_list is a
 * bare pointer walking it (codegen.c's prologue). There is no alignment
 * padding anywhere: a double or long long may start at any word. A
 * `float` arrives as a `double`. Every aggregate -- a structure, a union,
 * a _Complex -- and a long double is passed BY REFERENCE: its word is the
 * address of the caller's copy.
 */
#include "../../ir/irgen_int.h"

#include "../target.h"
#include "../../sema/type.h"
#include "../../driver/util.h"
#include "../../sema/sema.h"

/* Is an argument of this type passed as the address of a copy? */
static int by_reference(const struct type *t)
{
    return t->kind == TY_STRUCT || ty_is_xldouble(t);
}

int irg_va_arg_sparc(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    int flt = ty_is_float(rt) && !ty_is_xldouble(rt);
    struct type *ptr = ty_int_of_size(4, 1);
    long size = ty_size(rt);
    int byref = by_reference(rt);

    /* Every access here is natural: the va_list is a pointer object, as
     * aligned as its lvalue is, and every argument word is a word. An
     * eight-byte value at a word that is not 8-aligned is read as two
     * words by the backend, which never uses ldd for it. */
    int apa = gen_addr(fn, e->lhs);
    int cur = emit_load(fn, apa, ptr);
    irg_mark_natural(fn, e->lhs);
    int sdst = rt->kind == TY_STRUCT ? irg_va_struct_slot(fn, e) : -1;

    if (flt && rt->kind == TY_FLOAT)            /* promoted to double */
        size = 8;
    {
        int addr = new_temp(fn);
        long step = byref ? 4 : (size + 3) & ~3L;
        emit_mov(fn, addr, cur);
        emit_store(fn, apa,
                   emit_bin(fn, IR_ADD, addr, emit_const(fn, step, 4), 4, 1),
                   ptr);
        irg_mark_natural(fn, e->lhs);
        if (byref) {
            /* the word is the copy's address */
            int p = emit_load(fn, addr, ptr);
            fn->ins[fn->nins - 1].natural = 1;
            if (sdst >= 0) {
                irg_va_copy(fn, sdst, 0, p, ty_size(rt));
                return sdst;
            }
            {
                int v = emit_load(fn, p, rt);
                fn->ins[fn->nins - 1].natural = 1;
                return v;
            }
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

/* Inline assembly: EmbCC has no SPARC assembler yet, so an asm statement
 * with any instruction in it is refused by name (an empty template -- a
 * compiler barrier -- is accepted and emits nothing). */
void irg_asm_sparc(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    const char *p = a->tmpl ? a->tmpl : "";
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')
        p++;
    if (*p || a->nout || a->nin)
        diag_fatal(fn->file, s->line,
                   "inline assembly is not supported for sparc-none-elf "
                   "yet: EmbCC has no SPARC assembler (only an empty asm "
                   "with no operands, a compiler barrier, is accepted)");
    {
        struct ir_asm *ia = xcalloc(1, sizeof *ia);
        struct ir_ins *b = emit(fn);
        ia->scr = -1;
        b->op = IR_ASM;
        b->asm_ir = ia;
        b->dst = -1;
        b->a = b->b = -1;
    }
}

/* What codegen.c takes as an immediate without building it: a signed
 * 13-bit field for add, sub, and, or, xor, smul and a compare (subcc).
 * Here rather than beside the lowering because embls links the optimizer
 * without the code generator. */
int sparc_imm_foldable(int op, long imm)
{
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
    case IR_MUL: case IR_CMP:
        return imm >= -4096 && imm <= 4095;
    default:
        return 0;
    }
}

/* A 64-bit AND/OR/XOR, which the code generator does half by half: each
 * half the identity, zero, all ones, or a signed 13-bit value. */
static int half_ok(unsigned long c)
{
    long s = (long)(int)(unsigned int)c;
    return c == 0 || c == 0xffffffffUL || (s >= -4096 && s <= 4095);
}

int sparc_imm_foldable64(int op, long imm)
{
    unsigned long u = (unsigned long)imm;
    if (op != IR_AND && op != IR_OR && op != IR_XOR)
        return 0;
    return half_ok(u & 0xffffffffUL) && half_ok((u >> 32) & 0xffffffffUL);
}
