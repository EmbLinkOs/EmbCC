/* <sys/asm.h> -- the MIPS assembler conventions XC32's header of this name
 * gives PIC32 assembly: LEAF and NESTED open a function, END closes it,
 * and FRAME describes its frame for a debugger. EmbCC's assembler knows
 * the register names (k0, sp, ra...) itself.
 */
#ifndef _SYS_ASM_H
#define _SYS_ASM_H

#if defined(__ASSEMBLER__) || defined(__LANGUAGE_ASSEMBLY__)

#define LEAF(name)          \
    .text;                  \
    .globl name;            \
    .ent name;              \
name:

#define SLEAF(name)         \
    .text;                  \
    .ent name;              \
name:

#define NESTED(name, framesz, rareg) \
    .text;                  \
    .globl name;            \
    .ent name;              \
    .frame sp, framesz, rareg; \
name:

#define SNESTED(name, framesz, rareg) \
    .text;                  \
    .ent name;              \
    .frame sp, framesz, rareg; \
name:

#define END(name)           \
    .size name, .-name;     \
    .end name

#define EXPORT(name)        \
    .globl name;            \
name:

#endif
#endif
