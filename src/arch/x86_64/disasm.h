/* x86-64 instruction decoding. See disasm.c. */
#ifndef EMBCC_X86_64_DISASM_H
#define EMBCC_X86_64_DISASM_H

/* Decode one instruction at code[0..n) with runtime address `addr`, writing
 * its AT&T text to `out` (which needs ~256 bytes). Returns the byte length,
 * always >= 1: an undecodable byte becomes a one-byte `.byte` so the caller
 * never desyncs. */
int embdbg_decode_one(const unsigned char *code, int n, unsigned long addr,
                      char *out);

#endif
