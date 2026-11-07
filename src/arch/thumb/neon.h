/* A32 Advanced SIMD (NEON) and VFP from a learned table (neon.c). */
#ifndef EMBCC_ARCH_THUMB_NEON_H
#define EMBCC_ARCH_THUMB_NEON_H

/* The A32 word for the statement [stmt, stmt+len): 1 and *word; 0 when
 * the table holds no form of its mnemonic and shape (another encoder's,
 * or no one's); -1 with err when it has the form but not these operands. */
int neon_encode(const char *stmt, int len, unsigned long *word, char *err,
                int errlen);

/* MVE (ARMv8.1-M's vector extension), from tools/mve-isa's table: as
 * neon_encode, the word's first halfword in its high 16 bits */
int mve_encode(const char *stmt, int len, unsigned long *word, char *err,
               int errlen);

#endif
