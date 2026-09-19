/* Unwind tables (.eh_frame) and exception tables (.gcc_except_table) —
 * what a C++ exception needs of the functions EmbCC compiled (Itanium C++
 * ABI's exception handling; the System V psABIs' .eh_frame).
 *
 * .eh_frame: for each function, where the caller's frame (the CFA) is and
 * where the return address and the registers the function saved are kept,
 * as the DWARF call frame information libgcc's unwinder reads. The rules
 * describe each function from the end of its prologue on — exceptions
 * pass only through calls, and no call comes before the prologue ends or
 * inside an epilogue.
 *
 * .gcc_except_table: for a function with exception regions (irgen's
 * ir_func.eh), its LSDA in the format g++'s personality routine
 * (__gxx_personality_v0, which its FDE names) reads: every call's range
 * with its landing pad and action chain, the chains (catch types, whose
 * selectors are their places in the type table, and cleanups), and the
 * type table of typeinfo objects.
 *
 * As with dwarf.c, pointers are written as zero and returned as
 * relocations for the driver to bind: 32-bit PC-relative ones, as g++'s
 * are. */
#ifndef EMBCC_DEBUG_EH_H
#define EMBCC_DEBUG_EH_H

#include "../ir/ir.h"

/* what an eh_reloc binds to */
enum { EHT_TEXT,        /* .text's section symbol */
       EHT_LSDA,        /* .gcc_except_table's section symbol */
       EHT_PERSONALITY, /* __gxx_personality_v0 */
       EHT_GLOBAL };    /* glob's own symbol (a typeinfo object) */

struct eh_reloc {
    int in_lsda;        /* the field is in .gcc_except_table, not .eh_frame */
    int off;            /* the 4-byte field's offset in its section */
    int target;         /* EHT_* */
    long addend;        /* the target's offset */
    struct global *glob;
};

struct eh_buf {
    unsigned char *p;
    int len, cap;
};

struct eh_out {
    struct eh_buf frame, lsda;
    struct eh_reloc *relocs;
    int nrelocs, reloccap;
};

/* The tables for every function of iu with code (caller zero-inits out);
 * aarch64 when arm64. */
void eh_emit(struct ir_unit *iu, int arm64, struct eh_out *out);
void eh_free(struct eh_out *out);

#endif
