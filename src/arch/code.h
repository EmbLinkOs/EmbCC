/* A growable buffer of machine code — what every target's encoder
 * (x86_64/emit.c, aarch64/emit.c) and the assemblers write into. Target
 * neutral: bytes, little-endian 32-bit words, patching, alignment. */
#ifndef EMBCC_ARCH_CODE_H
#define EMBCC_ARCH_CODE_H

struct code {
    unsigned char *p;
    int len, cap;
};

/* ---- a statement that names a symbol ---------------------------------
 *
 * The contract between an assembler front end (src/as/gas.c) and a
 * target's own assembler: the target recognises a statement whose operand
 * is a SYMBOL, rewrites it into one it can encode with a zero in that
 * place, and says where each relocation goes.
 *
 * It lives here, in the header both sides already include, because
 * defining it in each gave two structurally identical types and one
 * incompatible function pointer. A target with no such forms leaves the
 * hook NULL.
 */
struct asm_symsite {
    int off;                     /* bytes from the start of this statement */
    int reloc;                   /* the target's relocation type */
};

struct asm_symform {
    int sym_at, sym_len;         /* the symbol's extent within the statement */
    /* A constant offset written after the symbol: `lds r24, buf+5`. It goes
     * into the relocation's addend, never into the instruction. This field
     * did not exist, and the AVR assembler read `buf+5` as `buf` -- the
     * identifier stopped at the `+` and nothing looked at what followed --
     * so the load went to buf[0] and the object assembled and linked
     * cleanly. */
    long addend;
    char encode[128];            /* the statement with a zero operand */
    struct asm_symsite site[2];
    int nsites;
};

void code_byte(struct code *c, int b);
void code_u16(struct code *c, unsigned v);               /* little-endian */
void code_u32(struct code *c, unsigned long v);          /* little-endian */
void code_patch32(struct code *c, int off, unsigned long v);
void code_align(struct code *c, int align, int fill);

#endif
