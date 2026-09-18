/* A growable buffer of machine code — what every target's encoder
 * (x86_64/emit.c, aarch64/emit.c) and the assemblers write into. Target
 * neutral: bytes, little-endian 32-bit words, patching, alignment. */
#ifndef EMBCC_ARCH_CODE_H
#define EMBCC_ARCH_CODE_H

struct code {
    unsigned char *p;
    int len, cap;
};

void code_byte(struct code *c, int b);
void code_u32(struct code *c, unsigned long v);          /* little-endian */
void code_patch32(struct code *c, int off, unsigned long v);
void code_align(struct code *c, int align, int fill);

#endif
