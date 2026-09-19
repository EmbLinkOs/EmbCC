/* Unwind tables (.eh_frame) — what a C++ exception needs to unwind
 * through a function EmbCC compiled: for each function, where the caller's
 * frame (the CFA) is and where the return address and the registers the
 * function saved are kept, as the DWARF call frame information libgcc's
 * unwinder reads (Itanium C++ ABI's exception handling; the System V
 * psABIs' .eh_frame).
 *
 * The rules describe each function from the end of its prologue on — the
 * only places an exception passes through are calls, and no call comes
 * before the prologue ends or inside an epilogue.
 *
 * As with dwarf.c, the pointers (an FDE's function address) are written as
 * zero and returned as relocations for the driver to bind: 32-bit
 * PC-relative ones, as g++'s are. */
#ifndef EMBCC_DEBUG_EH_H
#define EMBCC_DEBUG_EH_H

#include "../ir/ir.h"

enum { EHT_TEXT };      /* what an eh_reloc binds to: .text's symbol */

struct eh_reloc {
    int off;            /* the 4-byte field's offset in .eh_frame */
    int target;         /* EHT_* */
    long addend;        /* the target's offset */
};

struct eh_out {
    unsigned char *frame;
    int framelen, framecap;
    struct eh_reloc *relocs;
    int nrelocs, reloccap;
};

/* The unwind tables of every function of iu with code (caller zero-inits
 * out); aarch64 when arm64. */
void eh_emit(struct ir_unit *iu, int arm64, struct eh_out *out);
void eh_free(struct eh_out *out);

#endif
