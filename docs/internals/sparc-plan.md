# SPARC V8 (LEON3): the plan

The target is 32-bit SPARC V8, big-endian, as Gaisler's LEON3 implements
it (the processor of ESA's and many other space programmes):
`sparc-none-elf` (also `sparc-unknown-elf`, `sparc-elf`, `sparc`), with
soft float first and LEON3's integer multiply and divide (`-mcpu=leon3`).
It is EmbCC's third big-endian target after MIPS and PowerPC, and the
byte-order list in big-endian.md applies to it in full. Every fact below
was read off clang 23 (`--target=sparc-none-elf -mcpu=leon3
-msoft-float`), llvm-mc or QEMU 11 rather than remembered; there is no
SPARC gcc here.

## Registers and windows

- `%g0`-`%g7` (0-7) are global: `%g0` reads zero; `%g1` is a volatile
  temporary; `%g2`-`%g4` are the application's (gcc's default -mapp-regs
  uses them as caller-saved scratch, and so does EmbCC); `%g5`-`%g7` are
  the system's and never touched.
- A function sees a WINDOW of 24 more: `%o0`-`%o7` (8-15, outs), `%l0`-
  `%l7` (16-23, locals) and `%i0`-`%i7` (24-31, ins). `save` slides the
  window so that the caller's outs become the callee's ins, and `restore`
  slides it back. So in a function that has done `save`, its arguments
  are in `%i0`-`%i5`, the return address (of the call instruction) in
  `%i7`, its frame pointer `%fp` = `%i6` is the caller's `%sp` (= `%o6`),
  and the locals and ins survive every call it makes with no saving at
  all. A call clobbers `%o0`-`%o5`, `%o7` (the return address) and the
  globals.
- **Every function EmbCC emits does `save`** (`save %sp, -frame, %sp`;
  a frame beyond 4095 bytes is built in `%g1` first) and returns with
  `ret; restore` (`jmpl %i7+8, %g0` with `restore` in its delay slot).
  clang's leaf functions skip the `save` and use the outs directly
  (`retl`); that is an optimization for later and changes nothing at the
  interface.
- The register allocator's pool is `%o0`-`%o5` (caller-saved), then
  `%l0`-`%l5` and `%i0`-`%i5` (preserved by the window, so they cost
  nothing to save). The scratches are `%g1`-`%g4`, `%l6`, `%l7` and `%o7`.

## The calling convention (clang, sparc-none-elf)

- **The frame.** At `%sp` every frame has the 64-byte window save area
  (where a window-overflow trap stores this window's locals and ins), then
  at `%sp+64` the hidden struct-return word, then at `%sp+68` six words
  where a callee may store `%i0`-`%i5` (the argument home area), then the
  7th and later argument words from `%sp+92`. The minimum frame is 96
  bytes (92 rounded to the stack alignment of 8).
- **Arguments** are a sequence of WORDS with no alignment padding: the
  first six in `%o0`-`%o5`, the rest at `%sp+92`, `%sp+96`, ... A `long
  long` or (soft) `double` is two consecutive words, HIGH word first, and
  may be split between `%o5` and the stack (`f(int x5, long long)` puts
  its high word in `%o5` and the low at `%sp+92`). A `float` is one word.
- **Every aggregate goes by reference**: a structure or union of any size
  (an empty one included), a `_Complex` of any type and a `long double`
  (binary128) are copied by the caller and the copy's address is passed
  as one word. The callee owns the copy.
- **Results**: an integer, pointer or float in `%o0`; a `long long` or
  `double` in `%o0:%o1`, high word in `%o0`; a `_Complex float` in
  `%o0:%o1` and a `_Complex double` in `%o0`-`%o3`. A structure, union or
  `long double` is returned through a buffer the caller allocates and
  whose address it stores at `%sp+64` (the callee reads it at `%fp+64`);
  the caller follows the call's delay slot with an `unimp` word holding
  the structure's size (low 12 bits), and the callee returns PAST it, to
  `%i7+12`, with the buffer's address in `%o0`.
- **Varargs** follow the same word rule; a variadic callee stores `%i0`-
  `%i5` into its caller's home area at `%fp+68`, which makes the named and
  unnamed words one contiguous block, and `va_list` is a pointer walking
  it (no 8-byte rounding: a double may be at any word).
- **Data model.** ILP32 with a SIGNED `char`, a signed-int `wchar_t`,
  `long long` and `double` 8 bytes and 8-aligned in memory, and `long
  double` IEEE binary128: 16 bytes, 8-aligned, `__LDBL_MANT_DIG__` 113.
  The stack is 8-aligned. No `__int128`.
- **Soft float**: a float is its bits in an integer register and every
  operation calls lib/rt under libgcc's names (`__addsf3`, `__adddf3`,
  ...); a binary128 operation calls `__addtf3` and the rest with its
  operands by reference and its result through the struct-return
  convention, as for any function taking and returning `long double`.
  (clang itself calls `__addtf3` with the operands in registers, which no
  libgcc SPARC build implements; EmbCC's runtime is its own and agrees
  with its own calls -- see the gaps below.)

## Condition codes and delay slots

SPARC has integer condition codes (`icc`: N, Z, V, C), set only by the
`cc` forms (`subcc`, `addcc`, ...); `cmp a, b` is `subcc a, b, %g0`.
Branches (`Bicc`) test them, with sixteen conditions including signed
(`bl`, `bge`, `bg`, `ble`) and unsigned (`bcs`, `bcc`, `bgu`, `bleu`) ones,
so a comparison is one `cmp` and one branch. A 64-bit add or subtract is
`addcc`/`addxcc` (`subcc`/`subxcc`), and after `subcc lo; subxcc hi` the
carry and N^V are the whole 64-bit unsigned and signed less-than. A 0/1
result without a branch: unsigned `<` is `addx %g0, 0, d` after the `cmp`
(the carry), signed ones are `mov 0, d; b<cond>,a .+8; mov 1, d` (the
annulled slot runs only when the branch is taken).

Every branch, call and jump has a DELAY SLOT, as on MIPS. The first
backend fills every slot with a `nop` except where an instruction is
part of the idiom (`ret; restore`, the annulled `mov 1`, the `sll` in a
jump table's `call .+8`). A branch's
22-bit displacement is counted in words from the branch itself (+-8 MiB);
a function whose branch does not reach is refused by name. `call` has a
30-bit displacement and reaches the whole address space.

## Multiply and divide

LEON3 has `umul`/`smul` (the high word to `%y`, read with `rd %y`) and
`udiv`/`sdiv`, which divide the 64-bit `%y:rs1`: an unsigned divide
writes 0 to `%y` first, a signed one the dividend's sign word (`sra x,
31`). As clang does for `-mcpu=leon3`, no nops follow the `wr %y`
(LEON3 has no write delay on `%y`). A remainder is `a - (a/b)*b`. A
division by zero traps (tt 0x2a); `INT_MIN / -1` saturates to INT_MAX
(both undefined in C). 64-bit divides call lib/rt (`__divdi3`, ...).

## Switches

A dense `switch` is a table of 32-bit offsets in .text after its
dispatch, measured from a `call .+8` that finds its own address in
`%o7` -- so it needs no relocation; one unsigned compare sends both sides
of the range to the default (`bcc`).

## Misaligned accesses

A halfword, word or doubleword access to an unaligned address traps
(`mem_address_not_aligned`, tt 7), as on MIPS. Accesses irgen cannot
promise aligned (a packed member) go through bytes. `ldd`/`std` need an
even register and an 8-aligned address; EmbCC uses word pairs.

## Relocations (RELA)

| Type | Field | Value |
| --- | --- | --- |
| `R_SPARC_32` (3) | a data word | S + A |
| `R_SPARC_WDISP30` (7) | `call` | (S + A - P) >> 2 |
| `R_SPARC_WDISP22` (8) | `Bicc` | (S + A - P) >> 2, +-8 MiB |
| `R_SPARC_HI22` (9) | `sethi` | (S + A) >> 10 |
| `R_SPARC_LO10` (12) | `or`/`add`/load offset | (S + A) & 0x3ff |

An address is `sethi %hi(sym), r; or r, %lo(sym), r`. Calls are `call`
with WDISP30, even within the unit. `e_machine` is `EM_SPARC` (2),
class 32, big-endian, `e_flags` 0 (what clang writes for V8).

## The board: QEMU leon3_generic

- RAM at 0x40000000; the image is loaded there by `-kernel` and QEMU's
  own little boot loader (it enables the APBUART's transmitter and jumps
  to the ELF entry). Images are linked at 0x40000000.
- **Output**: the APBUART at 0x80000100 (data +0, status +4, control +8).
- **Register windows** need window overflow (tt 5) and underflow (tt 6)
  trap handlers, or the ninth nested `save` stops the processor. The
  harness installs a trap table (TBR) whose overflow and underflow entries
  jump to the classic handlers (save the oldest window to its `%sp` and
  rotate WIM right; restore it and rotate WIM left), with one invalid
  window just behind the two in use (the startup's and its caller's) and
  traps enabled with every interrupt masked (PIL 15). The handlers and every privileged instruction are
  hand-encoded words in tests/harness/sparc/boot.c, each with its
  assembly, refereed by sparc-encoding's encoder through the board tests
  (a deep recursion in the exec corpus overflows and underflows the
  windows many times). Every other trap prints `==FAULT tt pc==`.
  QEMU's LEON3 has 8 windows. The alternative, a flat model with no
  windows, would not call or be called by clang's code.
- **Ending a run**: the harness prints `==EXIT n==`, which qrun.sh
  `--until` stops at, then disables traps and executes `ta 0`; QEMU's
  LEON3 shuts down on a trap with traps disabled.

## What the first backend refuses

By name: computed goto, atomics wider than a word (one and two bytes are
a `casa` loop on the word around them),
`__builtin_frame_address`/`__builtin_return_address`, inline assembly
and `.s` files (there is no SPARC assembler in EmbCC yet), naked and
interrupt functions, C++, a scalar local aligned beyond 8, a branch beyond
+-8 MiB, and every machine flag but `-mcpu=leon3` (or `v8`), `-msoft-float`
and `-mbig-endian`.

## Tests

| Test | What it checks |
| --- | --- |
| `tests/golden/sparc-encoding.sh` | every encoder form against llvm-mc's object bytes; `sparc_li` executed; every range check |
| `tests/golden/sparc-exec.sh` | `tests/exec/*.c` on the leon3 board at -O0, -O1, -O2 and -Os |
| `tests/golden/sparc-abi.sh` | EmbCC and clang objects calling each other |
| `tests/golden/sparc-data.sh` | static data byte for byte against clang |
| `tests/golden/sparc-refuse.sh` | the object's header, the accepted and refused options and constructs |

## Status

Done: the encoder (2039 forms against llvm-mc), the target, the code
generator at -O0 and with the register allocator at -O1/-O2/-Os, EmbLD,
lib/rt and lib/libc, the board, and the goldens above. The exec corpus
passes on leon3_generic at every level (199 of 199: 21 refereed against
clang for an LP64 or little-endian assumption, 18 not applicable, plus
SPARC's own long-double-quad.c), also with the register pool shrunk to 2
and 4 and with the pair pass forced on and off, and 60 programs from the
differential fuzzer (gen2) agree with host clang at -O0, -O1, -O2 and -Os
on the board. lib/libc prints on the
board what it prints on x86-64 (libc-embedded), and -g verifies
(debug-embedded).

Known gaps, none of which miscompiles:

- Every delay slot is a nop (except `ret; restore` and the annulled `mov`
  of a 0/1 result), and every function opens a window even when it is a
  leaf; clang fills slots and leaves leaves windowless. Both are size and
  speed, not correctness.
- A local struct asking for alignment beyond 8 is placed at 8, as on MIPS
  (no SPARC instruction needs more); an over-aligned scalar is refused.
- No assembler: inline assembly with instructions, naked functions and
  `.s` files are refused. The harness writes its privileged instructions
  as words for that reason.
- No hardware floating point (`-mhard-float` refused), no `-mflat`.
- clang-compiled code doing long double arithmetic cannot use EmbCC's
  lib/rt (clang passes binary128 helper operands in registers).
- No WRY nops: as clang does for -mcpu=leon3, a divide follows `wr %y`
  directly (LEON3 has no delay on it; an older V8 part might).
