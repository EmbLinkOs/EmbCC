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
       EHT_GLOBAL,      /* glob's own symbol (a typeinfo object) */
       EHT_PR0, EHT_PR1,  /* ARM: __aeabi_unwind_cpp_pr0/1, by R_ARM_NONE */
       EHT_XDATA };       /* Win64: .xdata's section symbol */

/* how a field is relocated */
enum { EHR_PREL32,      /* target - here, 32 bits */
       EHR_PREL31,      /* ARM: the low 31 bits of target - here */
       EHR_NONE,        /* ARM: no field, only a reference to the symbol */
       EHR_RVA32,       /* Win64: the target's image-relative address */
       EHR_ABS64 };     /* Win64: its absolute address, 64 bits */

struct eh_reloc {
    int in_lsda;        /* the field is in .gcc_except_table, not .eh_frame
                         * (Win64: 0 .xdata, 2 .pdata) */
    int off;            /* the 4-byte field's offset in its section */
    int target;         /* EHT_* */
    long addend;        /* the target's offset */
    struct global *glob;
    int kind;           /* EHR_* */
};

struct eh_buf {
    unsigned char *p;
    int len, cap;
};

/* One function, for a format that describes functions rather than
 * instructions. DWARF's FDE is a program of rules; Darwin's compact
 * unwind is one 32-bit word per function plus its personality and LSDA,
 * so the emitter there needs the functions as a LIST rather than as a
 * byte stream it has already written. Recorded while the FDEs are
 * built, because that is where the LSDA offsets are known. */
struct eh_func {
    long code_off, code_len;
    int lsda_off;       /* -1 when the function has no exception table */
};

struct eh_out {
    struct eh_buf frame, lsda, pdata;
    struct eh_reloc *relocs;
    int nrelocs, reloccap;
    struct eh_func *funcs;
    int nfuncs, funccap;
};

/* The tables for every function of iu with code (caller zero-inits out);
 * aarch64 when arm64. */
void eh_emit(struct ir_unit *iu, int arm64, struct eh_out *out);
/* The same from the backend's CFI records (ir_cfi), for every other
 * target: sp and ra are its DWARF stack pointer and return address. */
void eh_emit_cfi(struct ir_unit *iu, int sp, int ra, struct eh_out *out);
/* 32-bit ARM: EHABI's tables, in frame (.ARM.exidx: two words a
 * function, in address order) and lsda (.ARM.extab: the entries that do
 * not fit a word, C++'s with their LSDA), each frame's unwinding made of
 * the Thumb backend's CFI records. */
void eh_emit_arm(struct ir_unit *iu, struct eh_out *out);
/* Win64: each function's UNWIND_INFO (the backend's, f->win_uw) into
 * frame, which is .xdata -- with the personality and the LSDA when it
 * has handlers -- and .pdata's begin/end/unwind triples into pdata. */
void eh_emit_win64(struct ir_unit *iu, struct eh_out *out);
void eh_free(struct eh_out *out);

#endif
