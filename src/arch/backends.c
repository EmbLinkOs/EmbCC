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

static const struct backend_desc g_backends[] = {
    [TARGET_X86_64] = {
        "x86-64", codegen_unit, 0, NULL, NULL, 0 },
    [TARGET_AARCH64] = {
        "AArch64", codegen_unit_arm64, 0, a64_op_calls_helper, NULL, 0 },
    [TARGET_THUMB] = {
        "ARM", codegen_unit_thumb, 1, t_op_calls_helper,
        "ARM EHABI unwind tables (.ARM.exidx)", BACKEND_CXX_EXC_NONE },
    [TARGET_RISCV32] = {
        "RISC-V", codegen_unit_riscv, 1, rv_op_calls_helper,
        "RISC-V .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_RISCV64] = {
        "RISC-V", codegen_unit_riscv, 1, rv_op_calls_helper,
        "RISC-V .eh_frame", 0 },
    [TARGET_AVR] = {
        "AVR", codegen_unit_avr, 1, NULL,
        "AVR .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_MIPS32] = {
        "MIPS", codegen_unit_mips, 0, mips_op_calls_helper,
        "MIPS .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_MIPS64] = {
        "MIPS", codegen_unit_mips, 0, mips_op_calls_helper,
        "MIPS .eh_frame", BACKEND_CXX_EXC_BIG_ENDIAN },
    [TARGET_LOONGARCH64] = {
        "LoongArch", codegen_unit_loongarch, 1, la_op_calls_helper,
        "LoongArch .eh_frame", 0 },
    [TARGET_TRICORE] = {
        "TriCore", codegen_unit_tricore, 0, tc_op_calls_helper,
        "TriCore .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_XTENSA] = {
        "Xtensa", codegen_unit_xtensa, 0, xtensa_op_calls_helper,
        "Xtensa .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_PPC32] = {
        "PowerPC", codegen_unit_ppc, 0, ppc_op_calls_helper,
        "PowerPC .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_SPARC32] = {
        "SPARC", codegen_unit_sparc, 0, sparc_op_calls_helper,
        "SPARC .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_COLDFIRE] = {
        "ColdFire", codegen_unit_coldfire, 0, cf_op_calls_helper,
        "ColdFire .eh_frame", BACKEND_CXX_EXC_NONE },
    [TARGET_RX] = {
        "RX", codegen_unit_rx, 0, rx_op_calls_helper,
        "RX .eh_frame", BACKEND_CXX_EXC_NONE },
};

const struct backend_desc *backend_get(enum target_arch a)
{
    if ((unsigned)a >= sizeof g_backends / sizeof g_backends[0] ||
        !g_backends[a].codegen)
        abort();                    /* a target with no row: a build error */
    return &g_backends[a];
}
