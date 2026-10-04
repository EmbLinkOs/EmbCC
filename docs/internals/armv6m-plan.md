# ARMv6-M: plan for a Cortex-M0 code generator

This page is for EmbCC developers. It surveys what the Thumb backend
(`src/arch/thumb/`) does today and states what an ARMv6-M mode needs:
Cortex-M0, M0+ and M1, triple `thumbv6m-none-eabi` (to be added). Function
names are given rather than line numbers, which move.

The encoder half has started. `src/arch/thumb/emit.c` has the `t1_*`
encoders, which emit only ARMv6-M forms, and `tests/golden/thumb-v6m-encoding.sh`
checks them against `llvm-mc -triple=thumbv6m-none-eabi`
([The encoders](#the-encoders)). No code generation or target plumbing for
ARMv6-M exists yet.

## What ARMv6-M takes away

ARMv6-M is the 16-bit Thumb instruction set plus six 32-bit instructions:
BL, MRS, MSR, DMB, DSB and ISB. Any other 32-bit encoding is UNDEFINED
and raises a HardFault. Most data processing works on r0-r7 only and
always sets the flags. The exceptions are MOV, ADD and CMP with a high
register.

| ARMv7-M feature | Used by the backend in | ARMv6-M |
|---|---|---|
| 32-bit encodings in general | every encoder that falls back to `hw2()` | BL, MRS, MSR, DMB, DSB, ISB only |
| IT blocks | `set_cc` → `t_setcc_low` | none (the encoding is 16-bit but absent) |
| CBZ/CBNZ | `IR_BRZ`/`IR_BRNZ` → `t_cbz` | none (16-bit but absent) |
| MOVW/MOVT | `t_mov_imm`, `t_mov_addr` (every address) | none |
| Modified immediates, ADDW/SUBW | `t_alu_imm`, `t_cmp_imm`, `t_addw`, `t_subw`, `logic_half` | imm3/imm8 on ADDS/SUBS/MOVS/CMP only |
| Shifted register operand | `t_alu_reg_shift` (shift fusion, `mul` by `2^k±1`, `shift64_imm_to`) | none |
| SDIV/UDIV | `IR_DIV`/`IR_MOD` → `t_div` | none |
| MLA/MLS/UMULL/SMULL | mla fusion, `IR_MOD`, 64-bit `IR_MUL` | MULS (32×32→32) only |
| UBFX/SBFX, CLZ, RBIT | `t_bfx` (masks, exponent fields) | none |
| LDRD/STRD | `rd64`/`wr64`, `IR_MEMCPY`, 64-bit loads | none |
| Negative, 12-bit and write-back offsets | `t_ldst_imm` T3/T4, `t_ldst_wb` | imm5 (scaled), sp + imm8×4 |
| Sign-extending loads with an immediate | `t_ldst_imm` with `sign` | LDRSB/LDRSH register offset only |
| Byte and halfword access from sp | `IR_LDVAR`/`IR_STVAR` of narrow locals | none |
| Register offset with a shift, or a high base | `t_ldst_reg` | `[Rn, Rm]`, low registers only |
| TBH, ADR.W | `IR_SWITCH` | ADR T1 (forward 0..1020) |
| LDREX/STREX/CLREX | `thumb_atomic` | none |
| B.W, B<c>.W | `t_b`, `t_bcond` (relaxation pass 0, `IR_SELECT`, `shift64_var`, `t_copy_block`, tail calls) | B ±2 KB, B<c> −256..+254, BL ±16 MB |
| PUSH/POP of r8-r12, POP into lr | the prologue (r9-r11 saved), the variadic epilogue | r0-r7 + lr (push), r0-r7 + pc (pop) |
| Low-register ALU without flags | every `s = 0` call | none |
| SUB sp, sp, Rm | `IR_ALLOCA` | ADD sp, Rm only |
| Unaligned LDR/STR/LDRH | word copies (`t_copy_block`, `IR_MEMCPY`, struct arguments) | faults |
| FPU | `fp_on_vfp`, the VFP encoders | none |

## The encoders

### What emits 32-bit encodings today

Every function in `emit.c` that calls `hw2()`, `movw()` or `code_patch32()`,
and what ARMv6-M code must do instead:

| `emit.c` function | 32-bit form it can emit | ARMv6-M replacement |
|---|---|---|
| `t_movs_reg` | MOVS.W with a high register | `t1_movs_reg` (low); `t_mov_reg` when the flags must survive |
| `t_movs_imm`, `t_mov_imm`, `t_mov_imm_dead_flags` | MOV.W, MOVW, MOVW+MOVT | the constant chooser ([Constants](#constants-and-addresses-literal-pools)) |
| `movw`, `t_movw_movt`, `t_mov_addr` | MOVW/MOVT (`RK_THM_MOVW`/`RK_THM_MOVT`) | `t_ldr_lit16` + `t1_patch_ldr_lit` from a literal pool word (`RK_ABS32`) |
| `t_mvn_reg` | MVN.W whenever `s` = 0 or a register is high | `t1_mvns` |
| `t_alu_reg` | the wide form whenever `s` = 0, a register is high, or a two-operand op has rd ≠ rn | `t1_addsub_reg`, `t1_alu_reg` (copy rn into rd first) |
| `t_alu_reg_shift` | every call | `t1_shift_imm` into a scratch, then `t1_alu_reg`/`t1_addsub_reg` |
| `t_bfx` | UBFX/SBFX | `lsls` then `lsrs`/`asrs`; `uxtb`/`uxth` for an 8/16-bit field at bit 0 |
| `t_alu_imm` | modified immediates, RSB #0 without flags | `t1_addsub_imm3`/`t1_addsub_imm8`; `t1_negs`; else a constant in a scratch |
| `addsubw`, `t_addw`, `t_subw` | ADDW/SUBW | `t1_addsub_imm8` (twice to ±510), `t1_add_sp_imm`, else a constant and `t1_addsub_reg` |
| `t_shift_imm`, `t_shift_reg` | the wide form for `s` = 0, high registers, ROR, rd ≠ rn | `t1_shift_imm`, `t1_shift_reg` (ROR by register only) |
| `t_mul` | MUL.W unless rd = rm and both low | `t1_muls` |
| `t_mla`, `t_mls`, `t_mull`, `t_div` | MLA, MLS, UMULL/SMULL, SDIV/UDIV | `t1_muls` + `t1_addsub_reg`; helper calls ([Helpers](#multiplication-division-and-64-bit-integers)) |
| `t_cmp_imm` | CMP.W with a modified immediate or high register | `t1_cmp_imm` 0..255; `adds` into a scratch for −255..−1 (CMN has no immediate); else `t1_cmp_reg` |
| `t_tst_reg`, `t_tst_imm` | TST.W | `t1_tst` |
| `t_ext`, `t_rev`, `t_rev16` | the wide form for high registers | `t1_ext`, `t1_rev`, `t1_rev16`, `t1_revsh` |
| `t_clz`, `t_rbit` | CLZ, RBIT | not used by `codegen.c` (irgen lowers the bit builtins to SWAR) |
| `t_ldst_imm` | LDR.W imm12, the T4 ±255 form, every signed form | `t1_ldst_imm`, `t1_ldst_sp`; signed: `t1_ldst_reg` or load + `t1_ext` |
| `t_ldst_pair`, `t_ldst_wb` | LDRD/STRD, write-back | two `t1_ldst_imm`, or `t1_ldm_stm` |
| `t_ldst_reg` | a shift, or a high register | `t1_ldst_reg` after `t1_shift_imm` of the index |
| `t_add_sp` | ADDW, MOVW + ADD | `t1_add_sp_imm` ≤ 1020; else a constant and `t1_add_hi(rd, sp)` |
| `t_sp_adjust` | SUBW/ADDW, MOVW + ADD via r12 | `t1_sp_adjust` ≤ 508, repeated, or a constant and `t1_add_hi(sp, rX)` |
| `t_push`, `t_pop` | PUSH.W/POP.W for high registers or POP lr | `t1_push`, `t1_pop`; high registers through low ones |
| `t_ldm_stm` | LDM.W/STM.W | `t1_ldm_stm` |
| `t_b`, `t_bcond`, `t_patch_b`, `t_patch_bcond` | B.W, B<c>.W | `t_b16`, `t_bcond16`, `t_bl` ([Branches](#branches-and-relaxation)) |
| `t_adr_w`, `t_patch_adr_w`, `t_tbh` | ADR.W, TBH | `t1_adr`, `t1_patch_adr`, `add pc` tables ([Switch](#switch-lowering)) |
| `t_ldrex`, `t_strex`, `t_ldrexbh`, `t_strexbh`, `t_clrex` | the exclusives | none ([Atomics](#atomics)) |
| `t_ldr_lit`, `t_ldr_const` | LDR.W literal, MOV.W/MVN.W/MOVW | `t_ldr_lit16` |
| `t_vldm_vstm`, `vfp` and every `t_v*` | VFP | none: `-mfpu` is refused for ARMv6-M |

Three 16-bit encoders are also not ARMv6-M, and a grep for `hw2(` does not
find them: `t_it`, `t_setcc_low` (an IT block) and `t_cbz`/`t_patch_cbz`.
`t_patch_pop` only handles the 32-bit POP; `codegen.c` does not call it.

### Already ARMv6-M

These emit only ARMv6-M encodings for every operand, so ARMv6-M code uses
them as they are: `t_mov_reg`, `t_bx`, `t_blx`, `t_nop`, `t_hint`, `t_svc`,
`t_bkpt`, `t_cps` (`i` only), `t_ldr_lit16`, `t_bcond16`, `t_b16`,
`t_patch_bcond16`, `t_patch_b16`, `t_patch_push` (16-bit form), and the
32-bit `t_bl`, `t_patch_bl`, `t_mrs`, `t_msr` (without BASEPRI and
FAULTMASK) and `t_barrier`. `IR_UD2` (`udf #0`, 0xde00) and `IR_FENCE`
(`dmb sy`) write ARMv6-M bytes directly.

### The `t1_*` encoders

Declared in `emit.h` under "ARMv6-M: the Thumb-1 forms". Each one emits
one 16-bit instruction or nothing:

- a register the form cannot name stops with an internal error, because
  that is a bug in the caller;
- an immediate or offset outside the field makes the `int`-returning ones
  answer 0 (`-1` for `t1_push`/`t1_pop`) and write nothing, as
  `t_ldst_imm` and `t_ldr_lit16` do.

| Function | Instruction |
|---|---|
| `t1_movs_imm`, `t1_movs_reg` | MOVS Rd, #imm8; MOVS Rd, Rm |
| `t1_addsub_reg`, `t1_addsub_imm3`, `t1_addsub_imm8` | ADDS/SUBS Rd, Rn, Rm; Rd, Rn, #0..7; Rdn, #0..255 |
| `t1_alu_reg` | ANDS/EORS/ADCS/SBCS/ORRS/BICS Rdn, Rm |
| `t1_shift_reg`, `t1_shift_imm` | LSLS/LSRS/ASRS/RORS Rdn, Rm; LSLS #0..31, LSRS/ASRS #1..32 |
| `t1_cmp_reg`, `t1_cmp_imm`, `t1_cmn`, `t1_tst` | CMP (T1 or T2), CMP #imm8, CMN, TST |
| `t1_negs`, `t1_mvns`, `t1_muls` | RSBS Rd, Rn, #0; MVNS; MULS Rdm, Rn, Rdm |
| `t1_add_hi` | ADD Rdn, Rm with any registers, sp and pc included; no flags |
| `t1_ext`, `t1_rev`, `t1_rev16`, `t1_revsh` | SXTB/SXTH/UXTB/UXTH, REV, REV16, REVSH |
| `t1_ldst_imm`, `t1_ldst_reg`, `t1_ldst_sp` | `[Rn, #imm5×size]`; `[Rn, Rm]` incl. LDRSB/LDRSH; `[sp, #imm8×4]` |
| `t1_add_sp_imm`, `t1_sp_adjust` | ADD Rd, sp, #imm8×4; ADD/SUB sp, #imm7×4 |
| `t1_adr`, `t1_patch_adr`, `t1_patch_ldr_lit` | ADR Rd, #imm8×4, and re-aiming an ADR or a literal LDR at a code offset |
| `t1_ldm_stm`, `t1_push`, `t1_pop` | LDMIA/STMIA Rn{!}; PUSH/POP of r0-r7 + lr/pc |
| `t1_udf` | UDF #imm8 |

Where an ARMv7-M encoder already writes the form for every operand the
`t1_*` function accepts, the `t1_*` function checks its operands and calls
it (for example `t1_ldst_reg` → `t_ldst_reg` → `narrow_ldst_reg`), so each
encoding is written once. The new encodings are ADDS/SUBS #imm8 (T2),
RORS, CMN, RSBS #0, the high-register ADD, REVSH, ADR, LDMIA/STMIA, UDF and
the two pc-relative patchers.

`tools/t1check/t1check.c` sweeps every `t1_*` function and every encoder
in the list above across its operands: every register in every field,
every immediate and offset, every condition, every LDM/STM and PUSH/POP
list, and BL's offset bit by bit. That is 60,728 instructions from 47
encoders. `tests/golden/thumb-v6m-encoding.sh` assembles the listing with
`llvm-mc -triple=thumbv6m-none-eabi -mcpu=cortex-m0` and compares the
bytes. The test also fails when:

- an encoder writes a size other than its form's (a widened form, or
  nothing);
- llvm-mc rejects or warns about a line, which means the form is not ARMv6-M;
- a `t1_*` function declared in `emit.h` is missing from the sweep;
- `t1check --refuse` finds an out-of-field operand that was encoded
  anyway (63 cases).

### Encoder work still to do

- **A wide-form scan.** A function `t1_first_wide(const struct code *, int
  from, int to)` decodes halfwords (bits 15:11 of `11101`, `11110` or
  `11111` start a 32-bit instruction), skips the data ranges in
  `c->drange`, and returns the offset of the first 32-bit instruction that
  is not BL, MRS, MSR, DMB, DSB or ISB. Run it after every ARMv6-M function
  and refuse the function by name on a hit. A stray ARMv7-M encoder call
  is otherwise silent until the Cortex-M0 faults.
- **The inline assembler.** `asm.c` calls the ARMv7-M encoders, so
  `tasm_set_arch(6)` must restrict the vocabulary. `gas.c`'s `ldr_literal`
  uses `t_ldr_const` (MOV.W/MVN.W/MOVW) and the 32-bit `t_ldr_lit`; for
  ARMv6-M it must always place a literal and use `t_ldr_lit16`.

## One backend or two

**Recommendation: one backend. Keep the ABI, frame, allocator interface
and pass loop in `codegen.c`, shared and selected by a mode flag. Write the
ARMv6-M instruction selector as a separate file.** The reasons, from the
code:

1. **AAPCS32 is the same on both, and the code insists on one copy.**
   `place_arg`, `place_one`, `walk_init`, `outgoing_area`, `t_abi_hints`,
   `t_pair_hints`, the prologue's parameter placement and the argument
   setup in `IR_CALL` all say that "no second copy of AAPCS32 is stated
   here". A separate generator would copy about 400 lines of ABI code,
   which the comments in these functions warn against.
2. **Frame, allocator and relaxation carry over.** `layout` already
   orders slots for the 1020-byte reach of `ldr rt, [sp, #imm]`, which is
   ARMv6-M's reach too. `wide64_map`, `frame_addr_map`, `t_pair_alloc`,
   `gen_func_best` and the pass loop in `gen_func` (scratch-save
   discovery, branch shortening, the `no_tbh` restart, `far_mode`) are the
   structure ARMv6-M needs, with different branch forms. Together that is
   about 2,100 lines.
3. **Instruction selection is Thumb-2 in almost every case.** `gen_ins`
   (about 1,450 lines), `gen_ins64`, `cmp64`, `shift64_*`, `logic_half`,
   `set_cc`, `thumb_atomic` and `IR_SWITCH` use IT, CBZ, TBH, ADR.W, UBFX,
   MLA/MLS, SDIV, UMULL, LDRD, shifted operands, modified immediates or
   ADDW. Adding `if (v6m)` to each case would write every case twice in
   one function. Every one of the 364 direct encoder calls in `codegen.c`
   would then be a place where a wide form could slip into ARMv6-M output.
4. **The register model is the other way round.** `rdr`, `wreg`, `LO()`
   and `lo_free` treat high scratch registers as the rule and low ones as
   an optimisation. On ARMv6-M a low register is required for every
   computation ([Registers](#registers)).

Proposed layout:

- `src/arch/thumb/cg.h` (new, internal): `struct t_fn`, `struct
  t_sites`, and the shared helpers `codegen.c` keeps static today (`rd`,
  `wr`, `rd64`, `wr64`, `slot_of`, `faddr`, `want_label`, `jump_to`,
  `jump_if`, `call_helper`, `note_*`, `place_one`).
- `src/arch/thumb/codegen.c`: the driver, ABI, frame and allocator. A
  flag `g_t_v6m` (from `target_thumb_arch() == 6`) selects the prologue,
  epilogue and frame-access forms, and calls `gen_ins` or `gen_ins_v6m`.
- `src/arch/thumb/isel_v6m.c` (new): `gen_ins_v6m`, `gen_ins64_v6m`, the
  literal pool, the constant chooser, `set_cc_v6m`, the switch lowering
  and the atomics. It includes a header that declares only the `t1_*`
  encoders and the ARMv6-M set above, so an ARMv7-M encoder call there
  fails to compile.

Each new `src/` file must be added to `SRCS` and `EMBLS_SRCS` in the
Makefile, and `build.ebm` regenerated in the same commit.

## Registers

### Scratch roles

| Role today | Register | Why it fails on ARMv6-M | ARMv6-M |
|---|---|---|---|
| `T_ACC` | r12 | nothing but MOV/ADD/CMP/BX can use it | low scratch S0 |
| `T_TMP`, `A_HI` | r11 | as above | low scratch S1 |
| `T_ADDR`, `B_LO` | r10 | as above; also a load/store base | low scratch S2 |
| `T_SCR`, `B_HI` | r9 | as above | low scratch S3 |
| `A_LO` | r12 | as above | S0 |
| `R_SCR` (cycle breaker for `ra_parallel_move`) | r9 | MOV works with any register, so it would still work | r12: caller-saved, never allocated, no push needed |
| indirect call target | r12 (`rd(F, i->a, T_ACC); t_blx`) | a load into r12 is 32-bit | load into a low scratch before the argument registers are filled, `mov r12, rX`, then `blx r12` |

The high registers keep one job: **parking**. r12 first, then r8-r11,
hold a low register's value while that register serves as a scratch.

### Where the low scratch comes from

Reserving two to four of r0-r7 would take a quarter to half of the
registers that compute. Instead, generalise the existing `lo_free`
mechanism:

1. Per IR instruction, the selector asks for up to k low scratch
   registers (k ≤ 4). `lo_free(F, n)` already gives the low registers that
   hold nothing live into or out of instructions n and n+1 (r0-r3 always,
   r4-r7 once the prologue saves them anyway). Extend `lo_op_ok`, which
   today excludes calls, helpers, asm, atomics and every 64-bit path, to
   every op. At `-O0` (no allocator) every low register except the frame
   base is free between instructions.
2. When fewer than k are free, **borrow**: take a low register that
   instruction n neither reads nor writes, `mov rP, rL` it into a parking
   register, and `mov rL, rP` it back after the instruction. Record the
   parking register through `t_scr()`, as r9-r11 are recorded today, so
   the prologue saves r8-r11 only when a pass used them. MOV does not
   touch the flags, so the restore can sit between a compare and its
   branch. `jump_if` must emit pending restores before the branch.
3. Rewrite the 64-bit slot paths to need two scratch registers, not four.
   Process the words one at a time: the carry and borrow survive LDR,
   STR, `add rd, sp, #imm` and MOV between `adds` and `adcs`.

Calls, helpers and atomics place values in fixed registers. They take
scratch registers explicitly: stack arguments are stored before r0-r3 are
loaded, as `IR_CALL` already orders it, and an indirect target goes
through r12.

### Allocation pool

| Pool today (`t_pool_base`) | ARMv6-M |
|---|---|
| `T_POOL` r0-r8 | r0-r7: a value in r8 can only be moved |
| `T_POOL_VA` r4-r8 | r4-r7 |
| `T_POOL_FB` r0-r6, r8 | r0-r6 |
| `T_POOL_VA_FB` r4-r6, r8 | r4-r6 |
| `T_PAIRS` r0:r1, r2:r3, r4:r5, r6:r7 | unchanged (all low) |

`t_callee_saved` is unchanged. r8-r11 are never allocated. Using them as
cheap spill homes is a later optimisation.

### Prologue and epilogue

- `save_mask_for` today ORs in r9-r11 and pads with r3. On ARMv6-M the
  mask is r4-r7, lr, the r3 pad, and enough extra low registers to stage
  any parked high registers. The full sequence is
  `push {r4-r7, lr}; mov r4, r8; …; push {r4-…}`, and the reverse on exit.
  `t_patch_push` already handles the 16-bit form. The pass loop's
  `scr_save` decides which high registers are saved, as it does for
  r9-r11 today.
- The frame: `t1_sp_adjust` reaches 508 bytes. Use it twice up to 1016,
  and above that `ldr rX, =-frame` with `add sp, rX` (`t1_add_hi`). rX
  must be a callee-saved low register that was just pushed, because r0-r3
  still hold the arguments.
- The variadic epilogue pops lr (`t_pop(t, mask)` then `bx lr`), which the
  16-bit POP cannot encode. Use `pop {r4-…}`, `pop {r3}`, `add sp, #16`,
  `bx r3`. r3 is not a return register.
- `IR_ALLOCA` uses `sub sp, sp, rX`, which ARMv6-M lacks. Use
  `t1_negs` + `add sp, rX`.
- The leaf path (`nopush`, `bx lr`) carries over unchanged, but a far
  branch through BL clobbers lr ([Branches](#branches-and-relaxation)).

## Flags

Every low-register data-processing instruction sets N and Z, and most set
C and V. The ARMv7-M backend relies on one invariant, stated at
`t_mov_imm_dead_flags`: **no flag value survives from one IR instruction
to the next.** That invariant is what makes an ARMv6-M selector practical:
flags only need to be preserved inside a single lowering. These lowerings
compute between a flag-setting instruction and the instruction that reads
its result:

- `set_cc` writes 1 and 0 with flag-free MOVW. On ARMv6-M, build the 1
  *before* the compare, into a register that is not an operand, then
  branch over `movs d, #0`. Better are the branch-free idioms, one per
  condition. For example, `a == b` is
  `subs d, a, b; rsbs t, d, #0; adcs d, t`, and `a <u b` is
  `cmp a, b; sbcs d, d; negs d, d`.
- `IR_SELECT` and `gen_ins64`'s `IR_SELECT` load an arm after the compare.
  Load both arms first, or compare last.
- The 64-bit carry chains in `gen_ins64` and `cmp64`. Only LDR, STR, MOV,
  `add rd, sp, #imm`, ADR and the sp adjustments may sit between `adds`
  and `adcs`. MOVS keeps C and V but changes N and Z.
- `shift64_var`, `t_copy_block` and `thumb_atomic` loop on a compare.

Flag-free ways to put a value in a register: MOV from a register, LDR
(including a literal), ADR, and `add rd, sp, #imm`.

## Constants and addresses: literal pools

### Constants

`t_mov_imm` today is MOVS, MOVW, MOV.W or MOVW+MOVT. The ARMv6-M chooser,
for a destination in r0-r7 with the flags dead:

| Value | Sequence | Bytes |
|---|---|---|
| 0..255 | `movs` | 2 |
| ~v in 0..255 | `movs`, `mvns` | 4 |
| byte << k | `movs`, `lsls` | 4 |
| 256..510 | `movs #255`, `adds` | 4 |
| −255..−1 (flags dead) | `movs`, `negs` | 4 |
| anything else, or the flags live | `ldr rd, [pc, #off]` + a pool word | 2 + 4 |

The same chooser serves `IR_CONST`, `operand_b`, `operand_b64`,
`logic_half`, `IR_SWITCH`'s bound and the frame-offset fallbacks in `rd`,
`wr`, `fb_addr`, `t_add_sp` and `t_sp_adjust`.

### Symbol addresses

`IR_GADDR`, `IR_STRADDR` and `IR_FADDR` call `t_mov_addr` and record two
sites (`RK_THM_MOVW`/`RK_THM_MOVT`) with `note_glob`, `note_str` and
`note_fn`. On ARMv6-M each becomes a pool word with **one** site of kind
`RK_ABS32` at the word's offset, recorded by the same `note_*` functions.
The rest of the chain already handles this kind:

- the driver writes text relocations with `code_rela` and
  `target_reloc_type`, which maps `RK_ABS32` to `R_ARM_ABS32`;
- a string's offset travels in the RELA addend (`target_reloc_addend`
  returns the bias unchanged for Thumb);
- EmbLD applies `R_ARM_ABS32` as `S + A`, and S already carries the Thumb
  bit of a function symbol (`link.c`);
- `asmout.c` names `RK_ABS32` as `R_ARM_ABS32` for `-S`.

### Pools and islands

LDR (literal) T1 loads from `Align(pc, 4) + imm8×4`: forward only, at most
1020 bytes, from a word-aligned entry. The design:

- Per function, a pending pool of entries: a constant, or (kind, symbol,
  addend). Identical entries share one word. A load emits
  `t_ldr_lit16(t, rd, 0)` and records (offset, entry).
- **Dumping a pool**: if code continues after the pool, emit `t_b16` over
  it. Pad to four bytes with `t_nop` (0xbf00, valid on ARMv6-M), mark the
  words with `code_mark_data` (for the `$d`/`$t` mapping symbols), emit
  each word (zero plus a site for a symbol), and patch every load with
  `t1_patch_ldr_lit`. A patch that fails is an internal error, never a
  dropped load.
- **Placement**: track the earliest deadline, `Align(at + 4, 4) + 1020`
  over the pending loads, less the pool's size. Between IR instructions,
  dump when the code could pass the deadline within the largest output
  one IR instruction can produce. That bound has to be enforced:
  straight-line `IR_MEMCPY` must switch to its loop below a fixed size.
  Prefer a natural barrier (after an unconditional `b`, the return jump,
  a `bx`), which needs no branch around the pool. Otherwise dump at the
  deadline with a branch. The function's last pool follows the epilogue.
- **With the pass loop**: pass 0 decides where the islands go and records
  them by IR index. Later passes put the pools at the same indexes. Code
  between a load and its pool only shrinks, so every load still reaches.
  A pool's alignment pad can grow by two bytes when the parity before it
  changes. Branch measurements that cross a pool therefore need two bytes
  of slack per pool. Without the slack, the "relaxed branch no longer
  reaches" internal error in `gen_func` can fire.
- This relies on `.text` being four-aligned in the object, as the word
  jump table already does (the `t->len % 4` nop in `IR_SWITCH`).

`src/as/gas.c` has a working literal-pool implementation for assembly
files (`pool_entry`, `pool_dump`, `ldr_literal`). It shows the
deduplication and the `$d` marking. The compiler's pool must place its own
islands, because `.ltorg` is the programmer's job in an assembly file.

## Frame access

- Words: `t1_ldst_sp` reaches 1020 bytes, as the 16-bit form `layout`
  already favours. Up to 1144: `add rX, sp, #1020`, then `[rX, #off]`.
  Beyond: a constant, `add rX, sp` (`t1_add_hi`), `[rX]`. `rd` today
  falls back to `t_mov_imm` + `t_ldst_reg(…, F->fb, …)`, an `[sp, rX]`
  access ARMv6-M does not have; `wr` falls back to MOVW and a wide ADD.
- Bytes and halfwords: there is no sp-relative LDRB/LDRH/STRB/STRH.
  `IR_LDVAR`/`IR_STVAR` of a narrow local need `add rX, sp, #off` and then
  `[rX, #0]`. A non-address-taken narrow scalar can be kept in a word slot
  and accessed as a word, extended at the store.
- Signed narrow loads: LDRSB/LDRSH take only `[Rn, Rm]`. Use `ldrb`/`ldrh`
  + `t1_ext`, or `movs rX, #off` + `t1_ldst_reg`.
- A VLA function's frame base r7 reaches only 124 bytes with
  `ldr rt, [r7, #imm5×4]`. Beyond that it costs two instructions per
  access (`movs`/literal + register offset).
- `fb_addr`'s r7 path uses `t_addw`. On ARMv6-M use `adds` (≤ 255 into the
  same register) or a constant + `t1_addsub_reg`.

## Branches and relaxation

ARMv6-M branch forms:

| Kind | Short | Medium | Far |
|---|---|---|---|
| unconditional | `b` ±2 KB (2 bytes) | — | `bl` ±16 MB (4 bytes, clobbers lr) |
| conditional | `b<c>` −256..+254 (2) | `b<!c> .+2; b label` ±2 KB (4) | `b<!c> .+4; bl label` (6) |

Map the existing machinery onto these forms:

- Pass 0 emits the medium form for every conditional branch and the short
  form for every unconditional one. It records each branch's size class
  by ordinal, as `shortb` does today. If pass 0 finds a target beyond
  ±2 KB, it reruns in far mode, as the existing `far_mode` does for
  B<c>.W's ±1 MB. Far mode forces lr into the push mask.
- Shortening: the medium form shrinks to `b<c>` when the target is in
  reach. Because medium is only 2 bytes larger, positions converge fast.
  Repeat measure-and-shrink while anything shrinks, beyond today's three
  passes. Each pass only shrinks code (pools aside, see above).
- `invert_last_bcond` must re-emit at the same size class, as it already
  keeps the ordinal. CBZ does not exist: `IR_BRZ`/`IR_BRNZ` are
  `cmp r, #0` + `b<c>` (or `t1_movs_reg`/`lsls #0` when that sets Z for
  free).
- Internal branches that call `t_bcond` and `t_b` directly (`IR_SELECT`,
  `shift64_var`, `t_copy_block`) span a few instructions and become
  `t_bcond16` and `t_b16`.

## Switch lowering

`IR_SWITCH` today is `cmp; bhs default` followed by `tbh` with a halfword
table, or `adr.w; ldr.w [.., lsl #2]; add; bx` with a word table (`T_TBH`
and `T_TAB` fixups). Without TBB/TBH, the compact form is clang's for
`thumbv6m`:

```text
    cmp   rI, #n          @ t1_cmp_imm, or a constant and t1_cmp_reg
    bhs   default         @ relaxed like any branch
    adr   rB, table       @ t1_adr, patched by t1_patch_adr
    ldrb  rT, [rB, rI]    @ t1_ldst_reg (halfword table: lsls rT, rI, #1; ldrh)
    lsls  rT, rT, #1
    add   pc, rT          @ t1_add_hi(15, rT): pc reads as this + 4
    .p2align 2
table: .byte (case - (add + 4)) / 2, ...
```

`add pc` writes pc without interworking (BranchWritePC), so the entries
are plain halfword offsets. A new fixup kind beside `T_TBH` fills them.
When an entry does not fit (a case behind the dispatch, or too far), the
`no_tbh` restart switches that switch to the word table:
`lsls rT, rI, #2; adr rB, table; ldr rT, [rB, rT]; adds rT, rB; bx rT`,
with the existing `T_TAB` entries (`(target | 1) - table`). This needs
two low scratch registers besides rI. rI may still be live in the case
blocks, so it is not reused as a scratch.

## Multiplication, division and 64-bit integers

| Operation | ARMv7-M today | ARMv6-M | Helper | In `lib/rt`? |
|---|---|---|---|---|
| 32-bit `/` | `sdiv`/`udiv` | call | `__aeabi_idiv`, `__aeabi_uidiv` | no |
| 32-bit `%`, and `/` paired with `%` | `udiv` + `mls` | call, one for both | `__aeabi_idivmod`, `__aeabi_uidivmod` (quotient r0, remainder r1) | no |
| 32-bit `*` | `mul`/`muls` | `muls` | — | — |
| 64-bit `*` | `umull` + `mla` | call | `__aeabi_lmul` | no (`__muldi3` exists only in `avr64.c`, under `__AVR__`) |
| 64-bit shift by a variable | branch + 5 shifts, 4 scratch | call | `__aeabi_llsl`, `__aeabi_llsr`, `__aeabi_lasr` | no |
| 64-bit shift by a constant | `shift64_imm_to` (shifted `orr`) | `lsls`/`lsrs`/`orrs`, 3 registers | — | — |
| 64-bit `/`, `%` | `__divdi3` etc. | unchanged | `__divdi3`, `__udivdi3`, `__moddi3`, `__umoddi3` | yes (`int64.c`) |
| 64-bit add, sub, logic, neg, compare | inline | inline (`adds`/`adcs`, `subs`/`sbcs`, `rsbs`/`sbcs`) | — | — |
| float, double | libgcc names | unchanged | `__addsf3`, `__adddf3`, … | yes (`softfp.c`) |
| clz, ctz, popcount | irgen's SWAR sequence | unchanged (`muls`) | — | — |

The 32-bit and 64-bit-multiply helpers use the RTABI names, which clang
calls for `thumbv6m-none-eabi`. `__aeabi_idivmod` returns both results
from one call, which no libgcc-named routine does. libgcc for ARM exports
the `__aeabi_*` names as well, so objects still link beside it. `lib/rt`
needs a new file, guarded by `__ARM_ARCH_6M__`, defining these helpers and
`__aeabi_idiv0`. Follow the rule in `lib/rt/README.md`: the shifts are
written on 32-bit halves, and division must not divide. Add
`thumbv6m-none-eabi` to the `rt-embedded` triples in the Makefile.

Optimizer passes that assume ARMv7-M instructions must be gated on the
architecture level. They do not produce wrong code, but they cost size:

- the divide/remainder pairing in `opt.c` (`kok` assumes `udiv; mls`; on
  ARMv6-M the pair becomes one `__aeabi_idivmod`);
- `mla_keeps_reg` (there is no MLA);
- `target_mul_shift_add` and the `IR_MUL` case of `thumb_imm_foldable`
  (there is no shifted operand);
- `thumb_imm_foldable` and `thumb_imm_foldable64` in `thumb/irgen.c`, and
  `const_is_expensive` in `opt.c`, which use `t_imm_ok` and 0..0xffff:
  the ARMv7-M immediates, not ARMv6-M's 0..255.

## Atomics

`thumb_atomic` is a `dmb; ldrex; op; strex; cmp; bne; dmb` loop. ARMv6-M
has no exclusives. Aligned loads and stores of up to four bytes are still
single-copy atomic. That covers plain atomic loads and stores (with `dmb`)
and `IR_FENCE`. For the read-modify-writes and compare-and-swap there are
two options:

- **Library calls** (`__atomic_fetch_add_4`, `__atomic_compare_exchange_4`,
  …), which is what clang emits for `thumbv6m`. Objects interoperate, and
  the policy lives in one replaceable place. `lib/rt` can provide
  PRIMASK-based versions.
- **Inline interrupt masking**: `mrs r12, primask; cpsid i; ldr; op; str;
  msr primask, r12`. MRS and MSR take r12, so no low register is spent
  saving PRIMASK. This is correct only on a single core with no other bus
  master, and only in privileged code. CPS is ignored in unprivileged
  Thread mode, so on a part with the unprivileged extension the sequence
  is silently not atomic.

**Recommendation:** library calls by default, with the predefined
`__GCC_ATOMIC_*_LOCK_FREE` values set to 1 as clang sets them for this
triple. Inline masking can follow as an opt-in for privileged-only
firmware. An RTOS on EmbCC will run tasks unprivileged, which is the case
where inline masking fails. 8-byte atomics also become calls
(`__atomic_*_8`) rather than the refusal they are today.

## Alignment

ARMv6-M faults on any unaligned LDR, STR, LDRH or STRH. The ARMv7-M
backend writes `Tag_CPU_unaligned_access` = 1 (`attrs.c`) and depends on
it in these places:

- `IR_MEMCPY`/`IR_MEMZERO` copy by words (straight-line, or
  `t_copy_block`), but **the IR carries no alignment for them**. A struct
  of `char` arrays with alignment 1 is copied a word at a time.
- `IR_CALL` copies a struct argument to the stack and into registers with
  word loads from the struct's address.
- `IR_RET` reads a three-byte composite as a word ("at least four-byte
  aligned"). This is true only if every such object is.
- `IR_LOAD`/`IR_STORE` with `natural` = 0 (a packed member) use an
  ordinary LDR/STR.

The fix: give `IR_MEMCPY`/`IR_MEMZERO` an alignment field (every
hand-built one must set it; default to 1, the safe value). Make the
ARMv6-M copies and struct-argument moves use the widest access the
alignment allows. Split every non-`natural` access into bytes. Write
`Tag_CPU_unaligned_access` = 0.

## Calls

- `bl` (`t_bl`, `RK_CALL` → `R_ARM_THM_CALL`) and `blx rX` are ARMv6-M.
  `call_helper` needs no change.
- Tail calls are `t_b` (B.W) with `R_ARM_THM_JUMP24`. ARMv6-M has no B.W,
  and the 16-bit B reaches ±2 KB with no call relocation, so `t_tail_ok`
  returns 0 for ARMv6-M.
- `IR_ASM`'s scratch pool is `{ 12, 0, 1, 2, 3 }`, and its operand
  load/store goes through `ldst_must(…, scr, …)`. With r12 as the base
  that is a 32-bit form. Use `{ 0, 1, 2, 3 }` on ARMv6-M.

## Target plumbing

- `src/arch/target.c`: triples `thumbv6m-none-eabi` and `thumbv6m`;
  `g_thumb_arch` = 6. Refuse `-mfpu=` and `-mfloat-abi=hard`
  (`arm_float_resolve` in the driver).
- `src/arch/thumb/attrs.c`: `Tag_CPU_arch` = 12 (v6S-M, what clang
  writes for `thumbv6m`), `Tag_THUMB_ISA_use` = 1,
  `Tag_CPU_unaligned_access` = 0, no FP tags.
- Predefined macros: `src/arch/thumbv6m/predef.c` and `predef_cxx.c`,
  generated by `tools/gen-predef.sh` from clang as `thumbv8m`'s are, then
  selected in `src/arch/predef.c`. Against `thumbv7m`, clang has
  `__ARM_ARCH` 6, `__ARM_ARCH_6M__`, `__ARM_ARCH_ISA_THUMB` 1,
  `__ARM_FEATURE_COPROC` 0, the atomic lock-free values 1, no `__thumb2__`,
  and none of `__ARM_ARCH_EXT_IDIV__`, `__ARM_FEATURE_IDIV`, `_CLZ`,
  `_LDREX`, `_QBIT`, `_SAT`, `_UNALIGNED` or `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_*`.
- EmbLD reads `Tag_CPU_arch` into `arm_arch` (`arm_attrs_scan`), but
  `arm_attrs_check` never compares it. An ARMv6-M image that links an
  ARMv7-M `librt.a` or `libc.a` links without complaint and faults on the
  first Thumb-2 instruction. The linker should at least warn when it mixes
  v6-M/v6S-M objects with later ones.

## Testing

- **Encoders**: `thumb-v6m-encoding.sh` (done).
- **A board**: QEMU's `microbit` machine is a Cortex-M0 (nRF51: flash at
  0, 16 KB of RAM at 0x20000000, UART0 at 0x40002000). Add
  `tests/harness/thumb-m0/` (boot, UART, link script) beside
  `tests/harness/thumb/`, and use `qrun.sh --until` with a sentinel. With
  16 KB of RAM, the large exec tests also run on the existing Cortex-M3
  board. ARMv6-M code is valid ARMv7-M. The wide-form scan replaces the
  undefined-instruction trap the M3 does not take. Setting
  `CCR.UNALIGN_TRP` in the M3 harness's boot code would supply the
  unaligned-access fault, if QEMU models that bit; check before relying
  on it.
- **Execution**: a `thumb-v6m-exec.sh` on the pattern of `thumb-exec.sh`,
  at every `-O` level, against the host and clang
  (`clang -target thumbv6m-none-eabi`).
- **Stress**: `EMBCC_RA_MAXPOOL` runs, which force borrowing and parking;
  the random-program fuzzer on the new board; the ABI fuzzer against
  clang's `thumbv6m` (the ABI is AAPCS32 unchanged).

## Order of implementation

1. **Encoders and their referee.** Done: the `t1_*` functions,
   `tools/t1check`, `thumb-v6m-encoding.sh`.
2. **Target plumbing.** Triple, `g_thumb_arch` = 6, attributes, predefined
   macros, `-mfpu` refusal, and the EmbLD architecture check. Codegen
   refuses every function by name ("the ARMv6-M backend cannot lower …").
3. **The skeleton at `-O0`.** The `cg.h` split, `isel_v6m.c`, the
   wide-form scan, prologue and epilogue, frame access, the constant
   chooser and literal pools, calls and returns, branches with the new
   relaxation, and `IR_CONST`/`IR_MOV`/ALU/`IR_LOAD`/`IR_STORE`/`IR_CMP`/
   `IR_BRZ`/`IR_JMP`. First image on the `microbit` board.
4. **`lib/rt` for ARMv6-M.** The `__aeabi_*` helpers and their test
   against the host, as `rt.sh` does; `build-rt.sh thumbv6m-none-eabi`.
5. **The rest of the 32-bit IR**: division and remainder through the
   helpers, shifts, extension, byte swap, `set_cc`, `IR_SELECT`,
   `IR_SWITCH`, `IR_MEMCPY`/`IR_MEMZERO` with the new alignment field,
   varargs, `IR_ALLOCA`, inline asm.
6. **64-bit integers and soft float**: `gen_ins64_v6m`, with the helper
   calls unchanged for float and double.
7. **Atomics** through library calls.
8. **The allocator** (`-O1` and up): the r0-r7 pool, pairs,
   `lo_free` for every op, borrowing and parking; `-O2` exec runs and
   `EMBCC_RA_MAXPOOL` stress; a fuzz board.
9. **The optimizer's ARMv7-M assumptions**, gated on the level.
10. **Size against clang** per function (`clang -fno-inline-functions`,
    symbol sizes from `nm -S`), then the branch-free `set_cc` idioms,
    literal sharing, `ldm`/`stm` block copies and the other code-size work.

## Key risks

1. **Silent widening.** A single ARMv7-M encoder call reaching ARMv6-M
   output assembles, links and faults only when executed. Mitigations: the
   separate selector file with a restricted header, the per-function
   wide-form scan, and running tests on a real Cortex-M0 model.
2. **Unaligned accesses.** The ARMv7-M backend leans on unaligned support
   in four places ([Alignment](#alignment)), and `IR_MEMCPY` has no
   alignment to consult. The fix touches the IR, and the M3 board will not
   catch a miss unless `UNALIGN_TRP` works there.
3. **Literal pools and relaxation together.** Forward-only 1020-byte
   reach, islands inside long functions, and a pass loop whose
   correctness argument is "code only shrinks", which a pool's alignment
   pad breaks by two bytes. The pass-0 placement by IR index and the slack
   in branch measurement both need tests at the reach limits, as
   `thumb-relax.sh` and `thumb-far.sh` test the branches today.
4. **Flags.** Every low-register ALU op clobbers them. Each ARMv7-M
   lowering that computes between a compare and the instruction that
   reads the flags (`set_cc`, `IR_SELECT`, the 64-bit carry chains,
   `shift64_var`, `t_copy_block`, `thumb_atomic`) has to be re-sequenced,
   not translated.
5. **Register pressure.** Eight computing registers include the four
   argument registers. A 64-bit pair takes a quarter of them, and
   borrowing adds moves under pressure. The VLA frame base r7 reaches only
   124 bytes.
6. **Mixed objects at link time.** EmbLD ignores `Tag_CPU_arch`, so an
   ARMv7-M runtime library in an ARMv6-M image goes unnoticed until it
   runs.
7. **Atomics under an RTOS.** Inline PRIMASK masking is not atomic in
   unprivileged code, which is where RTOS tasks run. This is why library
   calls are the recommended default.
