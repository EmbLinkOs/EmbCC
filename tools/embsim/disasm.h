/* disasm.h -- an instruction as text, for the fault report (disasm.c) */
#ifndef EMBSIM_DISASM_H
#define EMBSIM_DISASM_H

#include "image.h"
#include "sim.h"

/* the Thumb instruction at pc (its halfwords; h2 is read only for a
 * 32-bit one): its size in bytes, and the text in b */
int dis_thumb(u32 pc, u32 h1, u32 h2, char *b, size_t n);
/* the RISC-V instruction at pc (16 bits for a compressed one) */
int dis_riscv(u32 pc, u32 insn, int xlen, char *b, size_t n);
/* the MIPS32 instruction at pc */
int dis_mips(u32 pc, u32 insn, char *b, size_t n);
/* the PIC24F instruction at pc (a program address; i2 the word after
 * it, read for a two-word one): its size in words */
int dis_pic24(u32 pc, u32 i, u32 i2, char *b, size_t n);
/* --disassemble: the image's code (an ELF's executable segments, by
 * function; a PIC24 HEX image's program memory that is not erased), as
 * loaded on s */
void dis_listing(struct sim *s, const struct image *m, FILE *o);

#endif
