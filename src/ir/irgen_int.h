/* irgen's internal interface: the helpers the target-specific lowering in
 * src/arch/<arch>/irgen.c builds IR with, and the entry points irgen calls
 * there. Not part of the IR's public contract (ir.h) — only irgen and the
 * per-architecture halves of it include this. */
#ifndef EMBCC_IR_IRGEN_INT_H
#define EMBCC_IR_IRGEN_INT_H

#include "ir.h"
#include "../parse/ast.h"

/* ---- building IR (irgen.c) ---- */
struct ir_ins *emit(struct ir_func *fn);
int new_temp(struct ir_func *fn);
int new_label(struct ir_func *fn);
void emit_label(struct ir_func *fn, int label);
void emit_jmp(struct ir_func *fn, int label);
void emit_brz(struct ir_func *fn, int v, int w, int label);
void emit_brnz(struct ir_func *fn, int v, int w, int label);
int emit_const(struct ir_func *fn, long imm, int w);
int emit_bin(struct ir_func *fn, enum ir_op op, int a, int b, int w, int sign);
int emit_cmp(struct ir_func *fn, enum binop pred, int a, int b, int w,
             int sign);
int emit_load(struct ir_func *fn, int addr, const struct type *t);
void emit_store(struct ir_func *fn, int addr, int val, const struct type *t);
void emit_mov(struct ir_func *fn, int dst, int src);
int gen_expr(struct ir_func *fn, struct expr *e);
int gen_addr(struct ir_func *fn, struct expr *e);
int gen_convert(struct ir_func *fn, int v, const struct type *from,
                const struct type *to);

/* ---- per-architecture lowering (src/arch/<arch>/irgen.c) ---- */
/* va_arg(ap, T): SysV's __va_list_tag walk / AAPCS64's va_list record */
int irg_va_arg_sysv(struct ir_func *fn, struct expr *e);
int irg_va_arg_aapcs(struct ir_func *fn, struct expr *e);
/* extended asm: assign operand registers and assemble the template with
 * the target's own inline-asm vocabulary, then emit IR_ASM */
void irg_asm_x86(struct ir_func *fn, struct stmt *s);
void irg_asm_arm64(struct ir_func *fn, struct stmt *s);

#endif
