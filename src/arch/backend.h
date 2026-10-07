#ifndef EMBCC_CODEGEN_CODEGEN_H
#define EMBCC_CODEGEN_CODEGEN_H

#include <stddef.h>

#include "code.h"
#include "../ir/ir.h"
#include "target.h"

/* Calls to functions with no definition in this unit cannot be
 * resolved here — each becomes a relocation the driver hands to the
 * ELF writer (R_X86_64_PLT32 against the callee's UNDEF symbol). */
/* long double: which vregs hold a 16-byte value (a long double local, the
 * result of any op with w == 16, a MOV of one) — NULL if none. Both
 * backends give those a 16-byte slot and never a register. */
struct ir_func;
char *cg_wide_vregs(struct ir_func *fn);

/* The vregs holding a float or a double (never a long double), for a
 * backend that gives them their own register class. NULL when none. */
char *cg_float_vregs(struct ir_func *fn);
/* The same, deciding a value both kinds of op touch by its uses, for a
 * backend whose integer lowering can read and write an FP register home
 * (AArch64): see the definition. */
char *cg_float_vregs_by_cost(struct ir_func *fn);

/* Turn each string site's INDEX into its offset in .rodata. A backend
 * records the index while lowering (that is what IR_STRADDR carries) and
 * calls this once, at the end of its unit, before handing the sites back.
 * Shared because it was once the same line in four backends and missing
 * from the fifth. */
struct ir_unit;
struct strsite;
void cg_resolve_strsites(struct ir_unit *iu, struct strsite *s, int n);
struct func;
int cg_call_local(const struct func *caller, const struct func *callee);

struct extcall {
    int patch_off;        /* offset of the rel32 field in .text */
    struct func *callee;  /* canonical, !has_defn */
    int tail;             /* a tail call -- a branch (RK_TAIL) */
};

/* String-address sites: the instruction field the linker must point into
 * .rodata. One site on x86-64 (a RIP-relative lea's rel32); TWO on aarch64,
 * where materialising an address takes an adrp/add pair — hence `kind`,
 * which says which half of the address this site is. */
struct strsite {
    int patch_off;        /* offset of the relocated instruction in .text */
    int str_off;          /* target offset inside .rodata */
    enum reloc_kind kind;
};

/* Global-variable address sites, against the global's own symbol. */
struct gsite {
    int patch_off;
    struct global *glob;
    enum reloc_kind kind;
};

/* Function-address sites (&f / passing f), against the function's symbol. */
struct fsite {
    int patch_off;
    struct func *target;
    enum reloc_kind kind;
    /* Added to the target's address -- a label inside it: x86-64's
     * jump table of absolute entries, and a computed goto's &&label on
     * the targets that materialise it as an absolute address (the
     * function's own symbol plus the label's offset). 0 for &f. */
    long addend;
};

/* Lowers the unit to x86-64 into one .text image and fills each
 * func's code_off/code_len. Intra-unit calls are resolved here (rel32
 * patched once all functions are placed); external call sites are
 * returned via ext and next for the driver to relocate. The array is
 * malloc'd; caller frees. */
/* want_debug (from -g) turns on collection of each func's (offset,line)
 * line table (ir_func.lines); off, output is byte-for-byte as before. */
void codegen_unit(struct ir_unit *iu, struct code *text,
                  struct extcall **ext, int *next,
                  struct strsite **strs, int *nstrs,
                  struct gsite **gs, int *ngs,
                  struct fsite **fs, int *nfs, int want_debug, int optimize,
                  int no_sse, int regalloc);

/* The same lowering for aarch64 (AAPCS64). Same signature, same site
 * lists, so the driver picks one on --target= and nothing downstream
 * knows which machine produced the image. */
/* And for ARMv7-M (Thumb-2, AAPCS32). Same signature again — a third
 * machine the driver picks on --target= and nothing downstream knows
 * about. This one refuses far more than it emits; see the header of
 * src/arch/thumb/codegen.c for what and why. */
void codegen_unit_thumb(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc);

/* And for RISC-V -- ONE function for RV32 and RV64 alike, which reads
 * target_xlen() to know which. The other three backends are one per
 * machine; these two widths are one machine, and D-016 says why. */
void codegen_unit_riscv(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc);
/* The .riscv.attributes payload: the ISA string and stack alignment the
 * code was built for, as clang and gcc record them. Without it a
 * disassembler knows only RV32I and the C extension, and read every
 * mul, div and lr/sc as <unknown>. A malloc'd buffer the caller frees. */
unsigned char *riscv_build_attributes(size_t *len);

/* And for MIPS32r2 o32 soft-float (PIC32's core), in either byte order. */
void codegen_unit_mips(struct ir_unit *iu, struct code *text,
                       struct extcall **ext, int *next,
                       struct strsite **strs, int *nstrs,
                       struct gsite **gs, int *ngs,
                       struct fsite **fs, int *nfs, int want_debug,
                       int optimize, int no_sse, int regalloc);
/* The 24-byte .MIPS.abiflags payload EmbCC's objects carry: MIPS32r2,
 * 32-bit GPRs, no FPU, the soft-float ABI -- clang's for the same flags. */
void mips_build_abiflags(unsigned char out[24]);
/* The MIPS encoder's byte order (arch/mips/emit.h), which the driver sets
 * from target_big_endian() once the target is chosen. */
void mips_set_big_endian(int on);
void mips_set_64(int on);           /* ...and MIPS64's doubleword forms */

/* And for LoongArch64, LP64S (soft float): RV64's lowering with
 * LoongArch's instructions (src/arch/loongarch/codegen.c). */
void codegen_unit_loongarch(struct ir_unit *iu, struct code *text,
                            struct extcall **ext, int *next,
                            struct strsite **strs, int *nstrs,
                            struct gsite **gs, int *ngs,
                            struct fsite **fs, int *nfs, int want_debug,
                            int optimize, int no_sse, int regalloc);

/* And for TriCore 1.6.1, the AURIX core: little-endian, soft float, the
 * TriCore EABI (docs/internals/tricore-plan.md). */
void codegen_unit_tricore(struct ir_unit *iu, struct code *text,
                          struct extcall **ext, int *next,
                          struct strsite **strs, int *nstrs,
                          struct gsite **gs, int *ngs,
                          struct fsite **fs, int *nfs, int want_debug,
                          int optimize, int no_sse, int regalloc);

/* And for Xtensa, the windowed ABI of the ESP32 (LX6) and ESP32-S3
 * (LX7), little-endian, soft float. A function's literal pool precedes
 * it, so its symbol is at code_off + code_entry. */
void codegen_unit_xtensa(struct ir_unit *iu, struct code *text,
                         struct extcall **ext, int *next,
                         struct strsite **strs, int *nstrs,
                         struct gsite **gs, int *ngs,
                         struct fsite **fs, int *nfs, int want_debug,
                         int optimize, int no_sse, int regalloc);
/* And for 32-bit PowerPC, the embedded EABI, big-endian, soft float. */
void codegen_unit_ppc(struct ir_unit *iu, struct code *text,
                      struct extcall **ext, int *next,
                      struct strsite **strs, int *nstrs,
                      struct gsite **gs, int *ngs,
                      struct fsite **fs, int *nfs, int want_debug,
                      int optimize, int no_sse, int regalloc);

/* And for Renesas RX (RXv1), GCC's rx-elf ABI with 32-bit doubles and no
 * FPU (docs/internals/rx-plan.md). */
void codegen_unit_rx(struct ir_unit *iu, struct code *text,
                     struct extcall **ext, int *next,
                     struct strsite **strs, int *nstrs,
                     struct gsite **gs, int *ngs,
                     struct fsite **fs, int *nfs, int want_debug,
                     int optimize, int no_sse, int regalloc);
/* And for 32-bit SPARC V8 (LEON3), big-endian, soft float, with register
 * windows (docs/internals/sparc-plan.md). */
void codegen_unit_sparc(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc);
/* And for ColdFire (m68k-none-elf): ISA_A, big-endian, every argument on
 * the stack, soft float. */
void codegen_unit_coldfire(struct ir_unit *iu, struct code *text,
                           struct extcall **ext, int *next,
                           struct strsite **strs, int *nstrs,
                           struct gsite **gs, int *ngs,
                           struct fsite **fs, int *nfs, int want_debug,
                           int optimize, int no_sse, int regalloc);

/* And for AVR -- an EIGHT-bit machine, where nothing that matters fits in
 * a register and every value is a run of them. Same signature all the
 * same, so the driver still picks one on --target= and nothing downstream
 * knows which machine produced the image. */
void codegen_unit_avr(struct ir_unit *iu, struct code *text,
                      struct extcall **ext, int *next,
                      struct strsite **strs, int *nstrs,
                      struct gsite **gs, int *ngs,
                      struct fsite **fs, int *nfs, int want_debug,
                      int optimize, int no_sse, int regalloc);

void codegen_unit_arm64(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc);

#endif
