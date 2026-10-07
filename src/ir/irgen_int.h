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

/* va_arg(ap, struct T): the slot sema gave the expression (its value),
 * and `n` bytes copied into it at `off` from the address `src` */
int irg_va_struct_slot(struct ir_func *fn, struct expr *e);
/* Mark the IR_LOAD/IR_STORE just emitted as naturally aligned when the
 * lvalue `e` is (ir_ins.natural): anything but a packed member. */
void irg_mark_natural(struct ir_func *fn, const struct expr *e);
void irg_va_copy(struct ir_func *fn, int dst, long off, int src, long n);

/* ---- per-architecture lowering (src/arch/<arch>/irgen.c) ---- */
/* va_arg(ap, T): SysV's __va_list_tag walk / AAPCS64's va_list record */
int irg_va_arg_sysv(struct ir_func *fn, struct expr *e);
int irg_va_arg_aapcs(struct ir_func *fn, struct expr *e);
/* Darwin arm64: every variadic argument is on the stack, so the
 * list is the walking pointer itself (aarch64/irgen.c). */
int irg_va_arg_darwin(struct ir_func *fn, struct expr *e);
/* AAPCS32, where a va_list is a bare pointer at the next argument. */
int irg_va_arg_thumb(struct ir_func *fn, struct expr *e);
/* A pointer va_list's read and write-back in va_arg: by variable when it
 * is a local, through its address otherwise (irgen.c). */
int irg_va_ptr_read(struct ir_func *fn, struct expr *lv,
                    const struct type *ptr, int *slot);
void irg_va_ptr_write(struct ir_func *fn, struct expr *lv, int slot,
                      int val, const struct type *ptr);
int irg_va_arg_riscv(struct ir_func *fn, struct expr *e);
int irg_va_arg_mips(struct ir_func *fn, struct expr *e);
/* Xtensa: GCC's {__va_stk, __va_reg, __va_ndx} record, by value. */
int irg_va_arg_xtensa(struct ir_func *fn, struct expr *e);
/* AVR, where a variadic call puts EVERY argument on the stack -- the named
 * ones too -- so the list is a bare pointer and there is no split point. */
int irg_va_arg_avr(struct ir_func *fn, struct expr *e);
/* extended asm: assign operand registers and assemble the template with
 * the target's own inline-asm vocabulary, then emit IR_ASM */
void irg_asm_x86(struct ir_func *fn, struct stmt *s);
void irg_asm_arm64(struct ir_func *fn, struct stmt *s);
void irg_asm_riscv(struct ir_func *fn, struct stmt *s);
void irg_asm_mips(struct ir_func *fn, struct stmt *s);
void irg_asm_thumb(struct ir_func *fn, struct stmt *s);
void irg_asm_avr(struct ir_func *fn, struct stmt *s);

/* Whether an asm operand names MEMORY: its constraint, past = + &, allows
 * nothing but `m`. "rm" and "g" allow a register too, and GCC gives them
 * one, so the template is written for a VALUE -- reading them as memory
 * handed it the operand's address instead. */
int asm_constraint_mem_only(const char *c);

/* An asm output written as a value (ir_asm_op.val): may lv be one, where
 * its value is evaluated before the asm (-1: a local, by STVAR), and the
 * store of the asm's result into it afterwards (irgen.c). */
int irg_asm_val_ok(const struct expr *lv, int maxsize);
int irg_asm_out_addr(struct ir_func *fn, struct expr *lv);
void irg_asm_out_store(struct ir_func *fn, struct expr *lv, int addr,
                       int val);

#endif
