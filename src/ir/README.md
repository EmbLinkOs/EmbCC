# src/ir

EmbIR, the intermediate form — ../../docs/architecture/overview.md §3.

Linear three-address code over virtual registers. Width model: temps hold
promoted values (32-bit int class or 64-bit long/pointer class, the `w` field,
and 16 for a long double, the one value wider than a register);
variables live in memory at true width (`size`), with extending loads and
truncating stores. Labels, branches, short-circuit lowering; pointer arithmetic
scaled here. Loads and stores carry a `vol` flag so the optimizer never touches
a `volatile` (MMIO) access.

The temporaries are single-assignment by construction, which is what makes the
local passes in `../opt` sound with no analysis. Full **SSA form** — dominance
frontiers, phi insertion, renaming and out-of-SSA — is built *on demand* inside
`../opt` for mem2reg at `-O2`, not carried in the IR itself.

The IR is target-neutral, and so is most of its generation. The parts that
are not — `va_arg` (SysV's `__va_list_tag` walk against AAPCS64's va_list
record) and extended inline asm (each target assembles its own template
vocabulary) — live with their target in `../arch/<arch>/irgen.c`, built from
irgen's helpers through the internal header `irgen_int.h`.
