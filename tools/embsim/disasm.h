/* disasm.h -- an instruction as text, for the fault report (disasm.c) */
#ifndef EMBSIM_DISASM_H
#define EMBSIM_DISASM_H

#include "sim.h"

/* the Thumb instruction at pc (its halfwords; h2 is read only for a
 * 32-bit one): its size in bytes, and the text in b */
int dis_thumb(u32 pc, u32 h1, u32 h2, char *b, size_t n);
/* the RISC-V instruction at pc (16 bits for a compressed one) */
int dis_riscv(u32 pc, u32 insn, int xlen, char *b, size_t n);

#endif
