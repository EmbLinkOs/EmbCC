/* The predefined-macro tables, one per target.
 *
 * ARCHITECTURE.md §5: real newlib headers select their fixed-width types
 * from the full GCC predefined-macro family and hard-#error when it is
 * absent (the break that cost TCC patch 0002). Each table is generated
 * from `<triple>-gcc -dM -E` by tools/gen-predef.sh — never hand-derived.
 * That is also why the tables come from different compiler versions: each
 * is taken from the compiler EmbLinkOS actually builds that architecture
 * with, and the aarch64 side is pinned to 16.2.0 (myos ARM64.md §2.8).
 * The ARMv7-M and RISC-V tables come from CLANG rather than gcc (D-015,
 * D-016): clang carries every target in one binary, where each of
 * arm-none-eabi-gcc and riscv64-elf-gcc is a separate toolchain download.
 * RISC-V is TWO tables and not one parameterised by __riscv_xlen: the
 * widths disagree about more than the pointer -- the int-fast types, the
 * lock-free atomic set and __SIZEOF_INT128__ all move -- and a generated
 * file has no business being edited into a parameterised one. Like the gcc ones it is pinned to the
 * version that generated it -- the file's own header says which -- and
 * tests/golden/predef.sh compares against whatever is on PATH, so a
 * different clang will show up there as a handful of added macros
 * rather than as silence.
 *
 * Deliberately NOT in the tables (THE RULE — claim only what is present):
 *   __GNUC__ family  — EmbCC is not gcc; claiming it would route real
 *                      headers onto gcc-extension paths EmbCC cannot honor.
 *   __STDC__ family  — describes the compiler, not the target; the
 *                      preprocessor defines these itself.
 */
#ifndef EMBCC_CPP_PREDEF_H
#define EMBCC_CPP_PREDEF_H

struct predef_macro {
    /* For function-like macros the name carries its parameter list
     * verbatim, e.g. "__INT64_C(c)" — the M2 preprocessor splits it. */
    const char *name;
    const char *value; /* replacement text, verbatim from the reference gcc */
};

extern const struct predef_macro predef_macros_x86_64[];
extern const int predef_macro_count_x86_64;
extern const struct predef_macro predef_macros_aarch64[];
extern const int predef_macro_count_aarch64;
extern const struct predef_macro predef_macros_cxx_x86_64[];
extern const int predef_macro_count_cxx_x86_64;
extern const struct predef_macro predef_macros_cxx_aarch64[];
extern const int predef_macro_count_cxx_aarch64;
extern const struct predef_macro predef_macros_thumb[];
extern const int predef_macro_count_thumb;
/* ARMv8-M Mainline (Cortex-M33) has its own generated table: the feature
 * macros differ from v7-M's throughout, not just __ARM_ARCH. */
extern const struct predef_macro predef_macros_thumbv8m[];
extern const int predef_macro_count_thumbv8m;
extern const struct predef_macro predef_macros_cxx_thumb[];
extern const int predef_macro_count_cxx_thumb;
extern const struct predef_macro predef_macros_cxx_thumbv8m[];
extern const int predef_macro_count_cxx_thumbv8m;
/* ARMv6-M (Cortex-M0/M0+/M1): Thumb-1, no divide, no exclusives, no
 * unaligned access -- its own table, generated like the other two. */
extern const struct predef_macro predef_macros_thumbv6m[];
extern const int predef_macro_count_thumbv6m;
extern const struct predef_macro predef_macros_cxx_thumbv6m[];
extern const int predef_macro_count_cxx_thumbv6m;
extern const struct predef_macro predef_macros_armv7a[];
extern const int predef_macro_count_armv7a;
extern const struct predef_macro predef_macros_cxx_armv7a[];
extern const int predef_macro_count_cxx_armv7a;
extern const struct predef_macro predef_macros_riscv32[];
extern const int predef_macro_count_riscv32;
extern const struct predef_macro predef_macros_cxx_riscv32[];
extern const int predef_macro_count_cxx_riscv32;
extern const struct predef_macro predef_macros_riscv64[];
extern const int predef_macro_count_riscv64;
extern const struct predef_macro predef_macros_cxx_riscv64[];
extern const int predef_macro_count_cxx_riscv64;
extern const struct predef_macro predef_macros_mips32[];
extern const int predef_macro_count_mips32;
extern const struct predef_macro predef_macros_cxx_mips32[];
extern const int predef_macro_count_cxx_mips32;

/* AVR (ATmega328P). Generated like the others, from the reference
 * compiler's own answer for the triple. */
extern const struct predef_macro predef_macros_avr[];
extern const int predef_macro_count_avr;
extern const struct predef_macro predef_macros_cxx_avr[];
extern const int predef_macro_count_cxx_avr;

/* The language being compiled: 0 C, 1 C++ (its own table — __cplusplus and
 * friends). Set by the driver before preprocessing. */
void predef_set_cxx(int cxx);
int predef_is_cxx(void);

/* The table for the target selected by --target=. Every consumer goes
 * through this rather than naming a table, so adding a third machine
 * touches arch/predef.c and nothing else. */
const struct predef_macro *predef_table(int *count);

#endif
