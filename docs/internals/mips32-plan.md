# MIPS32 (mipsel, o32): the plan

The target is 32-bit little-endian MIPS, MIPS32 Release 2, the o32 ABI
with soft float: `mipsel-none-elf` (also `mipsel-unknown-elf`). It is the
core of Microchip's PIC32 parts. The board it is tested on is QEMU's
`malta`. Every fact below was read off clang 23
(`--target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float`), llvm-mc or
QEMU rather than remembered; clang with its integrated assembler is the
reference, as there is no MIPS gcc here.

## The o32 calling convention

- **Registers.** `$0` zero, `$1` at (assembler temporary), `$2-$3` v0-v1
  (results), `$4-$7` a0-a3 (arguments), `$8-$15` t0-t7, `$16-$23` s0-s7,
  `$24-$25` t8-t9, `$26-$27` k0-k1 (kernel, never touched), `$28` gp,
  `$29` sp, `$30` fp (s8), `$31` ra. Callee-saved: s0-s7, fp, gp, sp.
- **Arguments** are laid out as if in a structure in memory: each at its
  offset rounded up to its alignment (at least 4, at most 8), occupying
  whole words. The first 16 bytes travel in a0-a3 and the rest on the
  stack at the same offset from the caller's sp. So `f(int, long long)`
  passes the `long long` in a2:a3 and skips a1, and `f(int, double)`
  likewise; `f(int, int, int, double)` puts the double at sp+16. A
  composite of any size is passed by value in the same layout, split
  across a3 and the stack if it straddles them (no by-reference copies).
  A variadic argument follows exactly the same rules (a `float` arrives as
  a `double`).
- **The home area.** The caller always reserves 16 bytes at the bottom of
  its outgoing area, sp+0..15, for the callee to store a0-a3 into. A
  variadic callee does exactly that, so its named and unnamed arguments
  become one contiguous block at its entry sp, and `va_list` is a bare
  pointer walking it; `va_arg` of an 8-byte type rounds the pointer up to
  8 first. So every function that makes a call -- a runtime helper
  included -- has an outgoing area of at least 16 bytes.
- **Results.** An integer or pointer in v0; a `long long` or a (soft)
  `double` in v0:v1, low word in v0; a `float` in v0. Every struct and
  union, whatever its size, is returned through a hidden pointer the caller
  passes in a0 (so the first argument moves to a1), and the callee returns
  that pointer in v0. A `_Complex float` comes back in v0 (real) and v1
  (imaginary); clang returns a `_Complex double` in v0:v1 and a0:a1.
- **Data model.** ILP32: int, long and pointers are 4 bytes; `long long`
  and `double` are 8 and 8-aligned (also in structures and on the stack).
  `long double` is `double`. Plain `char` is signed; `wchar_t` is a signed
  `int`. An unnamed bit-field does not raise a structure's alignment. The
  stack is 8-aligned. Soft float: a float is its bits in a GPR and every
  operation calls lib/rt (`__addsf3`, `__adddf3`, ..., libgcc's names).

## Branch delay slots

Every branch and jump executes the instruction after it, the delay slot,
before the transfer happens. The first backend fills every delay slot with
a `nop` (`sll $0, $0, 0`), so the code is correct by construction and
each transfer costs 8 bytes. Filling the slot -- moving an independent
instruction from before the branch into it, or the epilogue's `addiu sp`
into the `jr ra` slot as clang does -- is an optimization for after the
exec corpus passes, and needs its own proof that the moved instruction
neither feeds the branch condition nor is a branch itself. MIPS32 has no
load delay slots, and Release 1 removed the HI/LO hazards, so there is
nothing else to schedule around.

Branches are PC-relative from the delay slot (target = PC + 4 + off16*4,
+-128 KiB). Inside a function every branch is resolved by the compiler,
with no relocation. One that does not reach takes the long form -- the
inverse branch over a `j` relocated against the function's own section
(`R_MIPS_26`, addend the label's offset) -- and the function is generated
again until nothing new fails. `j`/`jal` carry 26 bits of an absolute word
address within the current 256 MiB region and always take a relocation;
calls within the unit are `jal`s relocated like any other.

## Misaligned accesses

A word or halfword access to an unaligned address traps (Address Error),
where x86, ARMv7-M and QEMU's RISC-V read it. C guarantees alignment
except for a packed structure's member, and irgen marks the accesses it
can promise (`ir_ins.natural`); every other load or store of 2 or 4 bytes
goes through `lwl`/`lwr` (`swl`/`swr`) or two byte accesses, and block
copies whose ends are not known to be word-aligned likewise. Wide string
literals are interned aligned to their element, which the other targets
had never needed.

## HI/LO and `mul`

MIPS32 has the three-operand `mul rd, rs, rt` (SPECIAL2), which writes
the low word directly (and leaves HI/LO undefined). A 64-bit product's
high word comes from `multu`/`mult` and `mfhi`. Division is
`div`/`divu $zero, rs, rt` followed by `mflo` (quotient) or `mfhi`
(remainder); it never traps. HI and LO never hold a value across another
instruction, so nothing is allocated to them.

## Relocations

o32 objects use **REL** relocations (`.rel.text`, the addend stored in
the field), and EmbCC writes them that way:

| Type | Field | Value |
| --- | --- | --- |
| `R_MIPS_32` (2) | a data word | S + A |
| `R_MIPS_26` (4) | `jal`/`j` target, 26 bits | (S + A) >> 2, within P+4's 256 MiB region; A = field << 2 (sign-extended for an external symbol) |
| `R_MIPS_HI16` (5) | `lui` immediate | (AHL + S + 0x8000) >> 16 |
| `R_MIPS_LO16` (6) | `addiu`/load/store immediate | AHL + S, low 16 bits |
| `R_MIPS_PC16` (10) | branch offset | (S + A - P - 4) >> 2; A = sign-extended field << 2 |

**The AHL rule.** A HI16 and its LO16 carry one addend between them:
AHL = (AHI << 16) + (short)ALO, the HI16 field's value shifted up plus
the LO16 field's value sign-extended. So a HI16 relocation must be
followed, in the same relocation section, by a LO16 against the same
symbol, and a linker reads the pair together; the HI16 result is rounded
by 0x8000 because the LO16 half is sign-extended when it is added. EmbCC
writes each address as `lui`+`addiu` with the HI16 immediately followed
by its LO16, and the writer stores AHI = (A + 0x8000) >> 16 and
ALO = A & 0xffff. EmbLD finds the pairing LO16 for each HI16 (the next
LO16 against the same symbol in that section), and refuses an object
whose HI16 has none. The GOT/GP-relative relocations of PIC code
(`R_MIPS_GOT16`, `R_MIPS_CALL16`, `R_MIPS_GPREL16`) are refused by name;
`R_MIPS_JALR` is a hint and ignored.

## ELF header and sections

`e_machine` is `EM_MIPS` (8), class 32, little-endian. `e_flags` is
`EF_MIPS_ARCH_32R2 | EF_MIPS_ABI_O32 | EF_MIPS_NOREORDER` =
`0x70001001`, what clang writes with `-mno-abicalls`: `noreorder` because
the code is scheduled (the delay slots are filled, with `nop`s), no
`EF_MIPS_CPIC`/`EF_MIPS_PIC` because the code is not abicalls/PIC, and
`__mips_abicalls` is not predefined for the same reason. Each object
carries a `.MIPS.abiflags` section as clang's does (ISA MIPS32r2, GPR
32, no FPR, FP ABI soft), so a linker can refuse to mix it with
hard-float code. EmbLD drops `.MIPS.abiflags`, `.reginfo`, `.pdr` and
`.mdebug.*` from images.

## The board: QEMU malta

- The reset vector is 0xbfc00000 in KSEG1 (physical 0x1fc00000, the boot
  flash). QEMU's `-bios` image is loaded there word-swapped on a
  little-endian CPU, and code executing from that flash is re-fetched
  wrongly after an MMIO access (observed: a store to the UART made the
  next instruction read as `0x420`). So the harness uses `-kernel`: QEMU
  loads the ELF into RAM and writes its own reset-vector code at
  0xbfc00000 that jumps to the ELF entry. Images are linked at
  0x80100000 in KSEG0 (cached, unmapped RAM; QEMU keeps its boot
  environment below 0x80100000). `-cpu 24Kc`, a MIPS32r2 core with no
  FPU, makes any floating-point instruction trap.
- **Output.** The malta FPGA has a 16550 UART at physical 0x1f000900,
  KSEG1 0xbf000900, registers 8 bytes apart; a byte written to its THR
  is printed. QEMU wires it to the third serial port, so the board runs
  with `-serial null -serial null -serial stdio`.
- **Ending a run.** A bare-metal image never returns, so the harness
  prints a sentinel (`==EXIT n==` with main's result) and the runner
  (tests/harness/qrun.sh --until) stops at it. The harness then also
  writes 0x42 to the FPGA's SOFTRES register (0xbf000500), which with
  `-no-reboot` makes QEMU exit at once.

## What the first backend refuses

By name, with the IR instruction: computed `goto`; atomics narrower or
wider than a word (`ll`/`sc` are word-sized); `__builtin_frame_address`
and `__builtin_return_address` (no frame-pointer chain); jump tables (a
dense `switch` stays a decision tree, `target_jump_tables()`); `__int128`
(it does not exist on ILP32); naked and interrupt functions, file-scope
assembly with instructions and `.s` files (there is no MIPS file
assembler yet); a scalar local aligned beyond the 8-byte stack; C++ (the
C++ front end lays out LP64 only). Every MIPS flag other than the one
configuration emitted (MIPS32r2, little-endian, o32, soft float,
`-mno-abicalls`, `-G0`) is refused by the driver.

## Tests

| Test | What it checks |
| --- | --- |
| `tests/golden/mips-encoding.sh` | every encoder form against `llvm-mc -show-encoding`; `mips_li` executed; every range check |
| `tests/golden/mips-exec.sh` | `tests/exec/*.c` on the malta board at -O0, -O1, -O2 and -Os; the LP64-dependent programs against clang's result on the same board |
| `tests/golden/mips-abi.sh` | calls in both directions against clang, with the shared embedded ABI programs and the o32-specific ones |
| `tests/golden/mips-link.sh` | EmbLD's REL relocations, the AHL rule over every carry case, EmbCC's own far addends, and the link refusals |
| `tests/golden/mips-asm.sh` | the inline-asm vocabulary against llvm-mc, asm programs on the board, `-S` reassembled by llvm-mc |
| `tests/golden/mips-refuse.sh` | the object's header and flags, the accepted and refused options and constructs |
