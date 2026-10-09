/* The backend registry: what the driver needs to know about each code
 * generator, in one table (docs/internals/redesign.md, "the backend
 * interface").
 *
 * Before this table, the driver chose a code generator through a chain of
 * `if (ta == TARGET_X) codegen_unit_x(...)`, chose the helper predicate
 * through a parallel chain of conditionals, and refused unwind tables in
 * nine near-identical blocks, one per target, each naming its own table
 * format. A new backend had to find and extend every one of them. It now
 * adds one row here, and the driver reads the row.
 *
 * A row holds facts and entry points, no behaviour of its own: the
 * backends stay where they are (src/arch/<arch>/), and target.c keeps the
 * data model. */
#include "backend.h"

#include <stdlib.h>
#include <string.h>

static const struct backend_desc g_backends[] = {
    [TARGET_X86_64] = {
        .family = "x86-64", .codegen = codegen_unit, .ra_at_o0 = 0,
        .op_calls_helper = NULL,
        .unwind_unwritten = NULL,
        .cxx_exceptions = BACKEND_CXX_EXC_OK,
        .firmware = 0, .ld_scripts = 0,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL },
    [TARGET_AARCH64] = {
        .family = "AArch64", .codegen = codegen_unit_arm64, .ra_at_o0 = 0,
        .op_calls_helper = a64_op_calls_helper,
        .unwind_unwritten = NULL,
        .cxx_exceptions = BACKEND_CXX_EXC_OK,
        .firmware = 0, .ld_scripts = 0,
        .call_insn = "bl", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL },
    [TARGET_THUMB] = {
        .family = "ARM", .codegen = codegen_unit_thumb, .ra_at_o0 = 1,
        .op_calls_helper = t_op_calls_helper,
        .unwind_unwritten = "ARM EHABI unwind tables (.ARM.exidx)",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "bl", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .frame_sp = 13, .frame_ra = 14 },     /* sp, lr */
    [TARGET_RISCV32] = {
        .family = "RISC-V", .codegen = codegen_unit_riscv, .ra_at_o0 = 1,
        .op_calls_helper = rv_op_calls_helper,
        .unwind_unwritten = "RISC-V .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL },
    [TARGET_RISCV64] = {
        .family = "RISC-V", .codegen = codegen_unit_riscv, .ra_at_o0 = 1,
        .op_calls_helper = rv_op_calls_helper,
        .unwind_unwritten = "RISC-V .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_OK,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL },
    [TARGET_AVR] = {
        .family = "AVR", .codegen = codegen_unit_avr, .ra_at_o0 = 1,
        .op_calls_helper = NULL,
        .unwind_unwritten = "AVR .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 1,
        .no_asm_text = NULL },
    [TARGET_MIPS32] = {
        .family = "MIPS", .codegen = codegen_unit_mips, .ra_at_o0 = 0,
        .op_calls_helper = mips_op_calls_helper,
        .unwind_unwritten = "MIPS .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "jal", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = mips32_target_option },
    [TARGET_MIPS64] = {
        .family = "MIPS", .codegen = codegen_unit_mips, .ra_at_o0 = 0,
        .op_calls_helper = mips_op_calls_helper,
        .unwind_unwritten = "MIPS .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_BIG_ENDIAN,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "jal", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = mips64_target_option },
    [TARGET_LOONGARCH64] = {
        .family = "LoongArch", .codegen = codegen_unit_loongarch, .ra_at_o0 = 1,
        .op_calls_helper = la_op_calls_helper,
        .unwind_unwritten = "LoongArch .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_OK,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "bl", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = loongarch_target_option },
    [TARGET_TRICORE] = {
        .family = "TriCore", .codegen = codegen_unit_tricore, .ra_at_o0 = 0,
        .op_calls_helper = tc_op_calls_helper,
        .unwind_unwritten = "TriCore .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = tricore_target_option },
    [TARGET_XTENSA] = {
        .family = "Xtensa", .codegen = codegen_unit_xtensa, .ra_at_o0 = 0,
        .op_calls_helper = xtensa_op_calls_helper,
        .unwind_unwritten = "Xtensa .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = "compile with -c (there is no Xtensa assembler here to check the text "
                       "against)",
        .option = xtensa_target_option },
    [TARGET_PPC32] = {
        .family = "PowerPC", .codegen = codegen_unit_ppc, .ra_at_o0 = 0,
        .op_calls_helper = ppc_op_calls_helper,
        .unwind_unwritten = "PowerPC .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "bl", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = ppc_target_option },
    [TARGET_SPARC32] = {
        .family = "SPARC", .codegen = codegen_unit_sparc, .ra_at_o0 = 0,
        .op_calls_helper = sparc_op_calls_helper,
        .unwind_unwritten = "SPARC .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "call", .call_delay_slot = 1, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = sparc_target_option },
    [TARGET_COLDFIRE] = {
        .family = "ColdFire", .codegen = codegen_unit_coldfire, .ra_at_o0 = 0,
        .op_calls_helper = cf_op_calls_helper,
        .unwind_unwritten = "ColdFire .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "jsr", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 1, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = coldfire_target_option },
    [TARGET_RX] = {
        .family = "RX", .codegen = codegen_unit_rx, .ra_at_o0 = 0,
        .op_calls_helper = rx_op_calls_helper,
        .unwind_unwritten = "RX .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "bsr", .call_delay_slot = 0, .sym_prefix = "_",
        .imm_prefixed = 1, .text_p2align = 2,
        .no_asm_text = "EmbCC writes no RX assembly text (use -c; .s files and "
                       "inline asm do assemble)",
        .option = rx_target_option },
};

int option_listed(const char *arg, const char *const *exact,
                  const char *const *prefix)
{
    for (; *exact; exact++)
        if (!strcmp(arg, *exact))
            return 1;
    for (; *prefix; prefix++)
        if (!strncmp(arg, *prefix, strlen(*prefix)))
            return 1;
    return 0;
}

const struct backend_desc *backend_get(enum target_arch a)
{
    if ((unsigned)a >= sizeof g_backends / sizeof g_backends[0] ||
        !g_backends[a].codegen)
        abort();                    /* a target with no row: a build error */
    return &g_backends[a];
}
