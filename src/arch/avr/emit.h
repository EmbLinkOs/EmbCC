/* The AVR instruction encoder.
 *
 * Every instruction is 16 bits except lds, sts, jmp and call, which are
 * 32. Halfwords go out LITTLE-endian, and the four 32-bit forms are two
 * halfwords each in that order -- writing one as a u32 reverses them and
 * is the single easiest way to get this file wrong, which is why there is
 * one place (hw) that appends and nothing else touches the buffer.
 *
 * Registers are r0-r31 and EIGHT bits wide. A 16-bit value lives in a
 * PAIR (rN+1:rN, low byte first) and a 32-bit value in four; the pair for
 * a pointer has a name -- X is r27:r26, Y is r29:r28, Z is r31:r30 -- and
 * only those three can address memory. Callers pass the LOW register of a
 * pair and this file never guesses which half it was given.
 *
 * Three instruction groups have restricted operands, and they are the
 * three places AVR code most often fails to assemble:
 *
 *   - the immediate forms (ldi, subi, andi, ...) reach only r16-r31,
 *     which is why the register allocator's pool matters more here than
 *     anywhere else;
 *   - adiw/sbiw reach only r24, r26, r28 and r30, and only 0..63;
 *   - ldd/std displace only off Y and Z, never X.
 *
 * Each is checked rather than masked: a wrong register silently encodes a
 * different one, and that is a class of bug no test of the RESULT would
 * localise.
 *
 * Checked instruction by instruction against llvm-mc (tools/avrcheck),
 * for the same reason the RISC-V and VFP vocabularies are.
 */
#ifndef EMBCC_ARCH_AVR_EMIT_H
#define EMBCC_ARCH_AVR_EMIT_H

#include "../code.h"

/* The named pointer pairs, by their LOW register. */
enum { AVR_X = 26, AVR_Y = 28, AVR_Z = 30 };

/* The two-register ALU group, by the opcode field that distinguishes
 * them. All eleven share one encoder because they share one operand
 * layout; `mul` is here too, which is why the enum is not called
 * something narrower. */
enum avr_rr {
    AVR_ADD = 0x0C00, AVR_ADC = 0x1C00, AVR_SUB = 0x1800, AVR_SBC = 0x0800,
    AVR_AND = 0x2000, AVR_OR  = 0x2800, AVR_EOR = 0x2400, AVR_MOV = 0x2C00,
    AVR_CP  = 0x1400, AVR_CPC = 0x0400, AVR_MUL = 0x9C00
};
void avr_rr(struct code *c, enum avr_rr op, int d, int r);

/* The immediate group. `d` must be r16-r31 and `k` 0..255. */
enum avr_ri {
    AVR_LDI = 0xE000, AVR_SUBI = 0x5000, AVR_SBCI = 0x4000,
    AVR_ANDI = 0x7000, AVR_ORI = 0x6000, AVR_CPI = 0x3000
};
void avr_ri(struct code *c, enum avr_ri op, int d, int k);

/* The single-register group, by its low-nibble sub-opcode. */
enum avr_r1 {
    AVR_COM = 0x0, AVR_NEG = 0x1, AVR_SWAP = 0x2, AVR_INC = 0x3,
    AVR_ASR = 0x5, AVR_LSR = 0x6, AVR_ROR = 0x7, AVR_DEC = 0xA
};
void avr_r1(struct code *c, enum avr_r1 op, int d);

/* 16-bit add/subtract of a small constant, and a 16-bit move. `d` must
 * be r24, r26, r28 or r30 for adiw/sbiw and even for movw. */
void avr_adiw(struct code *c, int d, int k);
void avr_sbiw(struct code *c, int d, int k);
void avr_movw(struct code *c, int d, int r);

/* Memory, through X/Y/Z. `mode` is how the pointer moves. */
enum avr_ptr_mode { AVR_PTR_NONE = 0, AVR_PTR_POST_INC = 1, AVR_PTR_PRE_DEC = 2 };
void avr_ld(struct code *c, int d, int ptr, enum avr_ptr_mode m);
void avr_st(struct code *c, int ptr, int r, enum avr_ptr_mode m);
/* Displaced, off Y or Z only, q in 0..63. */
void avr_ldd(struct code *c, int d, int ptr, int q);
void avr_std(struct code *c, int ptr, int q, int r);
/* Absolute 16-bit data address: the only 32-bit data access. */
void avr_lds(struct code *c, int d, int addr);
void avr_sts(struct code *c, int addr, int r);
/* Program space -- the Harvard half. Reads the byte Z points at in
 * FLASH, which an ordinary ld cannot reach. */
void avr_lpm(struct code *c, int d, int post_inc);

/* I/O space, and the single-bit forms that only reach the low 32 ports. */
void avr_in(struct code *c, int d, int addr);
void avr_out(struct code *c, int addr, int r);
void avr_cbi(struct code *c, int addr, int bit);
void avr_sbi(struct code *c, int addr, int bit);

/* Stack. */
void avr_push(struct code *c, int r);
void avr_pop(struct code *c, int r);

/* Control flow. The branch displacement is in WORDS and relative to the
 * instruction AFTER the branch, which is what makes an off-by-one here a
 * jump into the middle of something. Returns the offset of the emitted
 * halfword so a forward branch can be patched. */
enum avr_cond {
    /* The condition is three bits plus a sense bit: brne is "not equal"
     * and breq is the same three bits with the sense flipped, which is
     * why these come in pairs and share one encoder. */
    AVR_BR_EQ = 0, AVR_BR_NE = 1, AVR_BR_CS = 2, AVR_BR_CC = 3,
    AVR_BR_MI = 4, AVR_BR_PL = 5, AVR_BR_LT = 6, AVR_BR_GE = 7
};
int  avr_br(struct code *c, enum avr_cond cond, int word_disp);
void avr_patch_br(struct code *c, int at, int word_disp);
int  avr_rjmp(struct code *c, int word_disp);
int  avr_rcall(struct code *c, int word_disp);
void avr_patch_rjmp(struct code *c, int at, int word_disp);
/* The 32-bit forms, which reach the whole program space. The operand is
 * a BYTE address and is halved here, because the instruction counts
 * words and every caller has a byte address. */
void avr_jmp(struct code *c, long byte_addr);
void avr_call(struct code *c, long byte_addr);
void avr_ret(struct code *c);
void avr_reti(struct code *c);
void avr_nop(struct code *c);
/* `ijmp`/`icall` jump to where Z points -- how a call through a function
 * pointer is made. */
void avr_ijmp(struct code *c);
void avr_icall(struct code *c);

/* Writes one line per instruction form, generated from the tables
 * themselves, so a form added here cannot escape the referee. */
#include <stdio.h>
void avr_vocabulary(FILE *f);
/* The same forms, encoded. One walk serves both so they cannot drift. */
void avr_encode_vocabulary(struct code *c);

#endif
