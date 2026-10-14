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
#include "thumb/attrs.h"
#include "../elf/elf.h"
#include "../elf/write.h"

#include <stdlib.h>
#include <string.h>

/* ---- the notes an object carries about itself (elf_notes) ---------- */

/* ARM's build attributes: what the object was built for, and the only
 * place downstream that can refuse a combination which cannot work --
 * ld compares Tag_ABI_VFP_args to stop a soft-float object linking
 * against a hard-float one. With no section at all there was nothing to
 * compare, so that link succeeded and the callee read its arguments from
 * registers the caller never wrote. See src/arch/thumb/attrs.h. */
static void arm_elf_notes(struct elfw *w)
{
    size_t alen = 0;
    unsigned char *ab = arm_build_attributes(&alen);
    elfw_add_section(w, ".ARM.attributes", SHT_ARM_ATTRIBUTES, 0,
                     ab, (Elf64_Xword)alen, 1);
    free(ab);
}

/* RISC-V's: the ISA the -march= names (riscv_build_attributes); without
 * it a disassembler knows only RV32I and C. */
static void riscv_elf_notes(struct elfw *w)
{
    size_t alen = 0;
    unsigned char *ab = riscv_build_attributes(&alen);
    elfw_add_section(w, ".riscv.attributes", SHT_RISCV_ATTRIBUTES, 0,
                     ab, (Elf64_Xword)alen, 1);
    free(ab);
}

/* MIPS's ABI flags, as clang's objects carry them: the ISA and register
 * sizes the code needs and the floating-point ABI (soft), so a linker
 * can refuse to mix it with a hard-float object. */
static void mips_elf_notes(struct elfw *w)
{
    unsigned char af[24];
    mips_build_abiflags(af);
    elfw_add_section(w, ".MIPS.abiflags", SHT_MIPS_ABIFLAGS, SHF_ALLOC,
                     af, (Elf64_Xword)sizeof af, 8);
}

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
        .option = thumb_target_option,
        .options_done = thumb_options_done,
        .frame_sp = 13, .frame_ra = 14,     /* sp, lr */
        .elf_notes = arm_elf_notes },
    [TARGET_RISCV32] = {
        .family = "RISC-V", .codegen = codegen_unit_riscv, .ra_at_o0 = 1,
        .op_calls_helper = rv_op_calls_helper,
        .unwind_unwritten = "RISC-V .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = riscv_target_option,
        .options_done = riscv_options_done,
        .frame_sp = 2, .frame_ra = 1,       /* sp, ra */
        .elf_notes = riscv_elf_notes },
    [TARGET_RISCV64] = {
        .family = "RISC-V", .codegen = codegen_unit_riscv, .ra_at_o0 = 1,
        .op_calls_helper = rv_op_calls_helper,
        .unwind_unwritten = "RISC-V .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_OK,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = riscv_target_option,
        .options_done = riscv_options_done,
        .frame_sp = 2, .frame_ra = 1,       /* sp, ra */
        .elf_notes = riscv_elf_notes },
    [TARGET_AVR] = {
        .family = "AVR", .codegen = codegen_unit_avr, .ra_at_o0 = 1,
        .op_calls_helper = NULL,
        .unwind_unwritten = "AVR .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 1,
        .call_insn = "call", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 1,
        .no_asm_text = NULL,
        .option = avr_target_option },
    [TARGET_MIPS32] = {
        .family = "MIPS", .codegen = codegen_unit_mips, .ra_at_o0 = 0,
        .op_calls_helper = mips_op_calls_helper,
        .unwind_unwritten = "MIPS .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_NONE,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "jal", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = mips32_target_option,
        .elf_notes = mips_elf_notes },
    [TARGET_MIPS64] = {
        .family = "MIPS", .codegen = codegen_unit_mips, .ra_at_o0 = 0,
        .op_calls_helper = mips_op_calls_helper,
        .unwind_unwritten = "MIPS .eh_frame",
        .cxx_exceptions = BACKEND_CXX_EXC_BIG_ENDIAN,
        .firmware = 1, .ld_scripts = 0,
        .call_insn = "jal", .call_delay_slot = 0, .sym_prefix = "",
        .imm_prefixed = 0, .text_p2align = 2,
        .no_asm_text = NULL,
        .option = mips64_target_option,
        .elf_notes = mips_elf_notes },
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
