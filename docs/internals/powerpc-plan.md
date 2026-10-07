# 32-bit PowerPC (powerpc-none-eabi): the plan

The target is 32-bit PowerPC, big-endian, the embedded EABI with soft
float: `powerpc-none-eabi` (also `powerpc-unknown-eabi`, `powerpc-eabi`,
`powerpc`, `ppc`) -- the e500/e200-class cores of automotive and
industrial controllers. Byte order is the target's own property
(big-endian.md); PowerPC has no little-endian twin here. The board it is
tested on is QEMU's `ppce500`. Every fact below was read off clang 23
(`--target=powerpc-none-eabi -mcpu=e500 -mno-spe -msoft-float
-mlong-double-64`), llvm-mc or QEMU rather than remembered.

## Which configuration

clang's `-mcpu=e500` turns SPE on and with it a 64-bit `long double`;
`-mcpu=ppc` leaves `long double` the 16-byte IBM double-double. EmbCC
emits neither SPE nor double-double, so the reference is `-mcpu=e500
-mno-spe -msoft-float -mlong-double-64`: an e500-class core with no SPE,
soft float, `long double` = `double`. Its predefined macros are clang's
for exactly that (tools/gen-predef.sh ppc32): `_ARCH_PPC`, `__PPC__`,
`__powerpc__`, `__BIG_ENDIAN__`, `_SOFT_FLOAT`, `_SOFT_DOUBLE`,
`__NO_FPRS__`, `__NO_LWSYNC__`, `__CHAR_UNSIGNED__`, and no `__SPE__`.

## The data model

ILP32: int, long and pointers 4 bytes; `long long` and `double` 8 and
8-aligned (in structures and on the stack); `long double` is `double`.
Plain `char` is UNSIGNED; `wchar_t` is a signed `int`. An unnamed
bit-field does not raise a structure's alignment (`struct { char c; int
:4; char d; }` is 3 bytes, aligned 1). The stack is 16-aligned (clang's
frames are multiples of 16 and it places a 16-aligned local at sp+16).

## The calling convention (SVR4 / EABI)

- **Registers.** r0 volatile (and the number 0 as a base or as addi's
  source), r1 the stack pointer, r2 and r13 the small-data anchors (never
  touched), r3-r10 arguments, r3:r4 results, r11-r12 volatile, r14-r31
  callee-saved. LR and CTR volatile; cr0, cr1, cr5-cr7 volatile.
- **Arguments.** Each argument takes the next of r3-r10, a word at a
  time. A `long long` or soft `double` takes an ODD-numbered pair (r3:r4,
  r5:r6, r7:r8, r9:r10) -- an even register is skipped -- with the HIGH
  word in the lower-numbered register. Once an argument does not fit,
  every later one goes on the stack, at 8(r1) upward in the caller's
  frame, 4-aligned (8 for an 8-byte one); `f(int x7, long long)` puts the
  `long long` at 8(r1) and leaves r10 unused.
- **Composites travel by reference.** Every struct and union, of any size,
  and every `_Complex`, is passed as a pointer to a copy the caller makes.
- **Results.** An integer, pointer or float in r3; a `long long` or
  double in r3:r4 (high word in r3). A composite of 8 bytes or fewer comes
  back in r3 (and r4), RIGHT-justified: its bytes read as a big-endian
  integer of its own size, zero-extended to 4 or 8 bytes (`struct {char
  c;}` is `li 3, 7`; a 6-byte one is r3 = bytes 0-1, r4 = bytes 2-5). A
  larger one goes through a hidden pointer the caller passes in r3, which
  moves the first argument to r4.
- **The frame.** 0(r1) holds the back chain (the caller's r1, written by
  `stwu r1, -frame(r1)`), 4(r1) is the word a CALLEE saves its LR in, and
  the outgoing stack arguments start at 8(r1). So a non-leaf function
  stores LR at frame+4 -- in its caller's frame -- and always has a frame.
  There is no red zone.
- **Variadic calls.** The caller clears CR bit 6 (`crxor 6, 6, 6`: no
  floating-point arguments in FPRs) before calling one. A variadic callee
  stores r3-r10 into a 32-byte register save area, and `va_list` is a
  12-byte record: the count of GPRs consumed (a byte), the FPR count (a
  byte, 0 here), the overflow area pointer (the caller's 8(r1) plus the
  named stack arguments) and the save area pointer. `va_arg` rounds the
  count to even for an 8-byte type, reads from the save area while the
  value fits by r10, and otherwise sets the count to 8 and reads the
  overflow area (8-aligned for an 8-byte type). EmbCC's `va_list` is a
  pointer to that record (as on AArch64), which is what clang's
  one-element array decays to when passed, so `vprintf(fmt, ap)` agrees
  across the two compilers.

## Code generation

Shape of the MIPS and RV32 backends: one home per vreg (a register from
the shared allocator, or a frame slot), a pair pass for 64-bit values,
soft float through libgcc's helpers (lib/rt). What PowerPC changes:

- Comparisons set a CR field; a branch tests one of its bits. A value 0/1
  is `mfcr` and an `rlwinm` of the bit (`xori 1` for the inverse).
- A 64-bit comparison is the high words compared (signed or not) and, if
  equal, the low words unsigned -- into the same cr0, so every predicate
  reads one result.
- 64-bit add/sub/neg use the carry (addc/adde, subfc/subfe, subfic/
  subfze); shifts by a variable count use slw/srw's six-bit amount (32-63
  shift everything out), branch-free but for the arithmetic right shift.
- No delay slots. `bc` reaches +-32 KiB: a branch that does not is the
  inverse `bc` over a `b` (+-32 MiB), and the function is generated again
  until nothing new fails.
- Addresses are `lis rX, sym@ha` / `addi rX, rX, sym@l`
  (R_PPC_ADDR16_HA/LO); calls `bl` with R_PPC_REL24, even within the unit.
- Misaligned word and halfword accesses are performed by the hardware (and
  QEMU), and clang uses plain `lwz` for a packed member, so nothing is
  split.
- Scratch registers: r11, r12, r10, r9 (the A and B pairs of a 64-bit
  operation) and r0 (data only -- never a base). The allocator's pool is
  r3-r8 and r14-r31 (r31 is the frame base under alloca).

## Relocations

RELA (`.rela.text`), clang's types:

| Type | Field | Value |
| --- | --- | --- |
| `R_PPC_ADDR32` (1) | a data word | S + A |
| `R_PPC_ADDR16_LO` (4) | addi/load displacement | (S + A) & 0xffff |
| `R_PPC_ADDR16_HA` (6) | lis immediate | ((S + A + 0x8000) >> 16) & 0xffff |
| `R_PPC_REL24` (10) | b/bl LI field | (S + A - P) >> 2, +-32 MiB |
| `R_PPC_REL14` (11) | bc BD field | (S + A - P) >> 2, +-32 KiB |
| `R_PPC_REL32` (26) | a data word | S + A - P |

`R_PPC_ADDR16_HI`, `R_PPC_ADDR16` and `R_PPC_ADDR24` are applied too; the
small-data (`R_PPC_SDAREL16`, `R_PPC_EMB_SDA21`) and GOT/PLT types are
refused by name. `e_machine` is `EM_PPC` (20), class 32, big-endian,
`e_flags` 0, as clang writes.

## The board: QEMU ppce500

- `-M ppce500` with `-kernel` and no `-bios` runs the ELF directly: QEMU
  loads it at its physical addresses, maps the first 64 MiB 1:1 with a
  TLB1 entry and starts at the entry point. Images are linked at
  0x00100000; the stack top is 0x00800000. The default CPU (e500v2) has
  no classic FPU, so a floating-point instruction traps.
- **Output.** The CCSR block is at physical 0xF_E000_0000 (36-bit), which
  nothing maps at reset: the harness writes a TLB1 entry (MAS0-3, MAS7,
  tlbwe) mapping 1 MiB of it, cache-inhibited and guarded, at
  0xE0000000. The first 16550 UART is CCSR+0x4500.
- **Ending a run.** A bare-metal image never exits, so the harness prints
  `==EXIT n ==` and the runner (tests/harness/qrun.sh --until) stops at
  it; the harness then writes HRESET_REQ to the global utilities' RSTCR
  (CCSR+0xE00B0), which with `-no-reboot` ends QEMU at once.

## What is refused, by name

SPE and VLE (`-mspe`, `-mvle`), hard float (`-mhard-float`), the 128-bit
`long double` (`-mlong-double-128`), small data (`-msdata=` other than
none, `-G` other than 0), little-endian, PIC; computed goto; atomics
narrower than a word (`lwarx`/`stwcx.` are word-sized);
`__builtin_frame_address` and `__builtin_return_address`; `__int128`;
C++ (laid out LP64 only); unwind tables.

## Tests

| Test | What it checks |
| --- | --- |
| `tests/golden/ppc-encoding.sh` | every encoder form against `llvm-mc -show-encoding`, bytes in memory order; `ppc_li` executed; every range check |
| `tests/golden/ppc-exec.sh` | `tests/exec/*.c` on the ppce500 board at -O0, -O1, -O2 and -Os; the LP64- and little-endian-dependent programs against clang's result on the same board |
| `tests/golden/ppc-abi.sh` | EmbCC and clang objects calling each other (embedded-abi, mips-abi and ppc-abi pairs) at -O0 and -O2 |
| `tests/golden/predef.sh` | the `ppc32` table against clang's |
