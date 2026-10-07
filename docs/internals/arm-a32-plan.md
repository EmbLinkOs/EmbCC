# ARMv7-A in ARM state (A32): the plan

The target is 32-bit ARM executing the **A32** instruction set:
`armv7a-none-eabi` -- a Cortex-A (A7, A8, A9, A15) or a Cortex-R in ARM
state, little-endian, bare metal, AAPCS with soft float. Every fact below
was read off clang 23 (`--target=armv7a-none-eabi -mfloat-abi=soft`),
llvm-mc or QEMU 11 rather than remembered.

## The decision: one ARM backend, two encoders

EmbCC already has a mature AAPCS32 backend for Cortex-M
(`src/arch/thumb`): the frame, the AAPCS32 argument walk, the register
allocator glue with its register pairs, 64-bit lowering, soft-float
helpers, atomics, the optional VFP, inline asm, `-g`. ARMv7-A in ARM state
has the **same ABI, the same register file and, instruction for
instruction, a superset of the operations** that backend selects (ARMv7-A
A32 has movw/movt, ubfx/sbfx, bfi, rbit, clz, rev, mla/mls, umull,
ldrex/strex of every width, dmb...). What differs is the *encoding* and a
handful of forms:

| Thumb-2 (v7-M) | A32 (v7-A) |
| --- | --- |
| 16- and 32-bit instructions, halfword order | every instruction one little-endian word |
| modified immediate (rotations and replicated bytes) | 8 bits rotated right by an even amount |
| `addw`/`subw`, 0..4095 unrotated | no such form: one or two rotated adds |
| `ldrh`/`ldrsb`/`ldrsh`/`ldrd` imm 0..4095 or -255..255 | imm -255..255 (word and byte: -4095..4095) |
| `ldrd` any two registers | an even register and the next one |
| `it` blocks | a condition field on every instruction |
| `cbz`, `tbh`, 16-bit branches | none; `b<c>` reaches +-32 MB |
| `orn` | none |
| `sdiv`/`udiv` (v7-M) | none in base ARMv7-A: `__aeabi_idiv` and family |
| PC reads as `.+4` | PC reads as `.+8` |
| symbols carry the Thumb bit, `$t` | even addresses, `$a` |
| `R_ARM_THM_CALL`, `THM_JUMP24`, `THM_MOVW_ABS_NC`, `THM_MOVT_ABS` | `R_ARM_CALL`, `JUMP24`, `MOVW_ABS_NC`, `MOVT_ABS` |

Two designs were possible:

**(a)** a third instruction selection (like `v6m.c`, the ARMv6-M one)
sharing the frame and the allocator through `cg.h`, over a new encoder.
`v6m.c` is 3,200 lines; an A32 one would be as large, and would re-derive
every lowering the v7-M selection already gets right -- 64-bit pairs,
compare/branch fusion, shifted operands, bit-field extracts, the pair
allocator's parallel moves -- with fresh chances to get each one wrong.

**(b)** the v7-M instruction selection unchanged, emitting through an
**alternate encoder**: every `t_*` function in `emit.c` keeps its meaning
(`t_alu_reg` is "rd = rn op rm", `t_ldst_imm` is "load or store at this
offset, or answer 0 when no form reaches it", `t_it` is "the next
instructions run under these conditions") and gains one line that sends it
to its A32 counterpart in `a32.c` when the target is A32.

**(b) is chosen.** The v7-M selection is the most exercised code in the
compiler for a 32-bit target (the exec corpus on four Cortex-M boards, the
fuzzers, FreeRTOS); A32 inherits all of it. The encoder contract was
already "refuse when the form cannot say it" (`t_ldst_imm`, `t_alu_imm`,
`t_ldst_pair` return 0), and the code generator already falls back on that
answer, so most A32 restrictions (the narrower `ldrh` offset, `ldrd`'s
register pairing, no `orn`) are expressed in the encoder alone. The
handful that cannot be -- division, the jump table, `cbz`, the two-branch
relaxation pass, the Thumb bit -- are decided in `codegen.c` under
`t_isa_a32` and listed in the next section. The Cortex-M targets are not
touched: in Thumb state every encoder runs exactly the code it ran before
(the dispatch is the first statement, and false), which the Thumb
goldens prove byte for byte.

The A32 encoder (`src/arch/thumb/a32.c`) is a separate file with its own
referee: `tools/a32check` prints every form it can emit with the word it
made, and `tests/golden/arm-a32-encoding.sh` has
`llvm-mc --triple=armv7a-none-eabi` assemble the same lines and compares
them word for word.

### How each Thumb-only idea maps

- **IT blocks.** `t_it(cond, "te")` emits nothing in A32: it queues the
  conditions, and each following encoder call takes the next one into its
  condition field (one call may write two words -- an `addw` split in two
  -- and both carry the condition). A queue left non-empty is an internal
  error.
- **`t_setcc_low`** is `mov<!c> rd, #0; mov<c> rd, #1`, for any register.
- **Immediates.** `t_imm_ok` answers for the A32 form, so every decision
  the code generator and the optimizer (`thumb_imm_foldable`) make about
  "is this constant an operand" is made for the machine that runs it.
- **`addw`/`subw`** become one `add`/`sub` when the constant rotates, else
  two (bits 11..4 and 3..0 each rotate), so they still reach 0..4095.
- **Branches.** Every branch is one word and reaches +-32 MB, so the
  short/long relaxation pass and far mode never trigger; `cbz` is never
  chosen. Branch offsets are measured from `.+8`.
- **Switch tables.** No `tbh`: `add pc, pc, rI, lsl #2` over a table of
  `b` instructions -- code, not data, so no `$d` and no relocation.
- **Division.** Base ARMv7-A has no `sdiv`/`udiv` in ARM state (clang
  defines no `__ARM_FEATURE_IDIV` for armv7a). `IR_DIV`/`IR_MOD` call
  `__aeabi_idiv`, `__aeabi_uidiv`, `__aeabi_idivmod` and
  `__aeabi_uidivmod` (remainder in r1), which `t_op_calls_helper` reports
  as calls so the allocator keeps values out of r0-r3 and lr across them.
  lib/rt provides them in software for this target (the ARMv6-M ones).
- **Function symbols** have no Thumb bit; the object has `$a` mapping
  symbols at each function; jump tables need no `$d`.

## ABI (AAPCS, base standard)

Identical to the Cortex-M targets' (that is the point of the design):
r0-r3 arguments and results, r4-r11 callee-saved, r12 scratch, 8-byte
stack alignment at calls, `long long`/`double` in even-odd register
pairs or 8-aligned stack slots, structs over 4 bytes returned through a
hidden pointer in r0, enums int-sized, plain `char` unsigned,
`wchar_t` `unsigned int`, ILP32. Soft float: a float travels as its bits
in a core register, and every operation is a call to lib/rt
(`__addsf3` ...). Calls are `bl` (R_ARM_CALL); a tail call is `b`
(R_ARM_JUMP24). Return is `pop {..., pc}` (an LDM that interworks on
ARMv7) or `bx lr`.

## ELF

`e_machine` EM_ARM, `e_flags` `EF_ARM_EABI_VER5` (0x05000000), what
clang writes. `.ARM.attributes`: Tag_CPU_name "7-A", Tag_CPU_arch v7 (10),
Tag_CPU_arch_profile 'A', Tag_ARM_ISA_use 1, Tag_THUMB_ISA_use 2 (as
clang), and the same ABI tags as Cortex-M. Relocations (REL, the addend
in the field):

| Type | Where | Value |
| --- | --- | --- |
| `R_ARM_ABS32` (2) | a data word | S + A |
| `R_ARM_CALL` (28) | `bl` | ((S + A) - P) >> 2 in 24 bits; to a Thumb symbol EmbLD makes it `blx` |
| `R_ARM_JUMP24` (29) | `b`, `b<c>` | ((S + A) - P) >> 2; to a Thumb symbol refused (needs a veneer) |
| `R_ARM_MOVW_ABS_NC` (43) | `movw` | (S + A) & 0xffff, A from imm4:imm12 |
| `R_ARM_MOVT_ABS` (44) | `movt` | (S + A) >> 16 |

## The board

QEMU's `virt` machine with `-cpu cortex-a15` (`-M virt` boots an ELF with
`-kernel` at its entry in ARM state, SVC mode, MMU off), RAM at
0x40000000, a PL011 UART at 0x09000000, and `-semihosting` so the image
can end QEMU itself (SYS_EXIT through `svc #0x123456`). The harness prints
`==EXIT n==` before exiting, which `qrun.sh --until` also stops at.

## What is refused (by name)

- `-mfpu=neon` and the Cortex-M units; `-mfloat-abi=softfp|hard`
  without an `-mfpu=` (VFPv3/VFPv4 came after the soft-float backend:
  the Thumb VFP encodings carry over with a condition field).
- `-mcpu=` other than the ARMv7-A cores, `-mthumb` (that is the
  thumbv7 targets), interrupt functions (an A-profile handler returns
  with `subs pc, lr, #4`, which this backend does not emit).
- Hardware divide (`-mcpu=cortex-a15` has it; the code still calls the
  helpers, which is correct on every v7-A core).
- The M-profile system registers in inline asm (`primask`, `basepri`...).

## Tests

| Test | What it checks |
| --- | --- |
| `arm-a32-encoding` | every A32 encoder form against llvm-mc |
| `arm-a32-exec` | `tests/exec` on QEMU virt at -O0 -O1 -O2 -Os |
| `arm-a32-abi` | EmbCC and clang (A32) objects calling each other |
| `arm-a32-refuse` | the object header and attributes; refused options |

## Status

Done:

- Hard float: `armv7a-none-eabihf` (= `-mfpu=vfpv3-d16 -mfloat-abi=hard`),
  and `-mfpu=vfpv3-d16|vfpv3|vfpv4-d16|vfpv4` with `-mfloat-abi=softfp|
  hard` -- the Cortex-M7's double-precision VFP code under a condition
  field, clang's macros and Tag_FP_arch for each unit, an `eabihf`
  runtime, the harness turning the unit on (CPACR, FPEXC). The exec
  corpus and the AAPCS-VFP pairs against clang run on it too.

- `src/arch/thumb/a32.c`, the A32 encoder, behind every `t_*` encoder in
  `emit.c` (one dispatch line each; the Thumb encodings are unchanged and
  their goldens pass). `tools/a32check` + `arm-a32-encoding`: 130,123
  instructions from 57 encoders identical to llvm-mc's, 104 out-of-form
  operands refused with nothing written.
- The target: `--target=armv7a-none-eabi` (`armv7a`, `armv7-none-eabi`,
  `armv7a-unknown-none-eabi`), clang's predefined macros
  (`src/arch/armv7a`), `.ARM.attributes` v7-A, `$a`, even function
  symbols, the four A32 relocation types; `-marm`, the ARMv7-A
  `-mcpu=`s and `-mfloat-abi=soft` accepted.
- Code generation at -O0 (allocator for the temporaries) and
  -O1/-O2/-Os, through the ARMv7-M selection with the differences listed
  above. The exec corpus passes 197 of 197 at every level, also with the
  allocator's attempts forced one at a time (`EMBCC_T_PAIRS=0/1`,
  `EMBCC_T_EXT=1`, `EMBCC_T_LOWREGS=1`) and with the pool shrunk to three
  registers (`EMBCC_RA_MAXPOOL=3`).
- The file assembler and inline asm in ARM state (conditions on any
  instruction; `mrs`/`msr` of `cpsr`, `mrc`/`mcr`, the A32 `svc`/`bkpt`/
  `udf`), `lib/libc`'s `setjmp`/`longjmp` written with it.
- EmbLD: the A32 relocations (REL and RELA), `bl` made `blx` across
  instruction sets in both directions, the A32 `-Tstack` stub.
- `lib/rt` and `lib/libc` build for the target (`make rt-embedded`,
  `make libc-embedded`); `__aeabi_[u]idiv[mod]` in software
  (`lib/rt/aeabidiv.c`, shared with ARMv6-M).
- The board: `tests/harness/arm-a32`, QEMU virt / Cortex-A15, also a board
  of `exec-boards.sh` (`a7`).
- Goldens: `arm-a32-encoding`, `arm-a32-exec`, `arm-a32-abi` (EmbCC and
  clang in all four pairings, in ARM state and across ARM/Thumb, plus
  clang's `__aeabi_*` calls on EmbCC's runtime), `arm-a32-refuse`; and an
  `armv7a` row in `asmout-roundtrip` and `predef`. Each new golden was
  shown to fail against a deliberate mutant.

Known gaps:

- NEON (Advanced SIMD) is refused by name: nothing here emits it, and
  `__ARM_NEON` would promise `arm_neon.h`.
- 8-byte atomics are refused, though ARMv7-A has `ldrexd`/`strexd`.
- No hardware divide even on the cores that have it (A7, A15): the
  helpers are always called.
- Interworking is calls only: a `b` (tail call) between ARM and Thumb
  code needs a veneer, which EmbLD refuses.
- The code assumes unaligned word/halfword accesses are permitted (MMU on,
  Normal memory), as clang's does; `-mno-unaligned-access` is refused.
- C++, computed goto, `__builtin_frame_address`, interrupt functions:
  refused, as on the Cortex-M targets (interrupt functions are accepted
  there).
