# Renesas RX (rx-none-elf): the plan

The target is the Renesas RX family's first instruction set, RXv1, as the
RX600/RX610 cores implement it (the R5F562N7/N8 of the RX62N line):
32-bit, little-endian, sixteen general registers, `rx-none-elf` (also
`rx-elf`, `rx-unknown-elf`, bare `rx`). The board it is tested on is
QEMU's `gdbsim-r5f562n8`.

There is no clang for RX and none on this machine, so every fact below
was read off **GCC 16.2.0 for `rx-elf` and binutils 2.47**, built from
source on this machine (`~/EmbRef/rx-elf`, `~/EmbRef/rx-src/build.sh`),
off QEMU 11's RX decoder and translator (`target/rx/insns.decode`,
`translate.c`, `disas.c`) and board (`hw/rx/rx-gdbsim.c`, `rx62n.c`,
`hw/char/renesas_sci.c`), and off GCC's `config/rx/rx.h`, `rx.cc` and
`rx.opt`. Nothing is from memory; where two references disagree, this
document says which was believed and why.

## The configuration: GCC's rx-elf default, without the FPU

EmbCC emits exactly what `rx-elf-gcc -nofpu` emits by default:

- **`-m32bit-doubles`.** GCC's rx-elf default (rx.opt: "Stores doubles
  in 32 bits. This is the default.") -- `double` AND `long double` are
  IEEE single precision, four bytes (`rx_c_mode_for_floating_type`
  returns SFmode for both). CC-RX's default is the same. A program that
  needs more precision than a float gives is not portable to this
  target as configured by its own toolchains; `-m64bit-doubles` is
  refused by name (it changes the ABI of every double and would need a
  second runtime). The object says which (`E_FLAG_RX_64BIT_DOUBLES`
  clear), and so does the predefined `__RX_32BIT_DOUBLES__`.
- **No FPU instructions.** GCC's default for RX600 is `-fpu` (the
  RX600's single-precision FPU, `fadd` .. `ftoi`), and with it GCC also
  turns on `-ffinite-math-only` (`__FINITE_MATH_ONLY__ 1`), because the
  RX FPU flushes denormals and does not handle NaN and infinity as IEEE
  says. EmbCC's first backend is soft float through lib/rt, which is
  GCC's `-nofpu` -- exact IEEE single precision, `__FINITE_MATH_ONLY__
  0`, no `__RX_FPU_INSNS__`. The FPU is a later, opt-in step.
- **The RX ABI** (`-mrx-abi`, GCC's default, `__RX_ABI__`,
  `E_FLAG_RX_ABI` in `e_flags`): stacked arguments naturally aligned,
  not every one padded to 4 (`-mgcc-abi`, refused).
- **Little-endian data** (`-mlittle-endian-data`, `__RX_LITTLE_ENDIAN__`);
  `-mbig-endian-data` is refused.
- **CPU** `-mcpu=rx600` (`__RX600__`); `rx610` is accepted and changes
  only its macro; `rx100`/`rx200` (no FPU, otherwise the same RXv1
  instruction set) are accepted too; RXv2/RXv3 cores are refused.

## The data model

From `rx-elf-gcc -dM -E` (with `-nofpu`):

| Type | Size | Align | Notes |
| --- | --- | --- | --- |
| `char` | 1 | 1 | **unsigned** (`__CHAR_UNSIGNED__`) |
| `short` | 2 | 2 | |
| `int`, `long` | 4 | 4 | ILP32 |
| `long long` | 8 | **4** | `__BIGGEST_ALIGNMENT__` is 4 |
| pointer | 4 | 4 | |
| `float`, `double`, `long double` | 4 | 4 | all IEEE single |
| `size_t` | 4 | | `long unsigned int` |
| `ptrdiff_t`, `intptr_t` | 4 | | `long int` |
| `wchar_t` | 4 | | `long int` (signed) |
| `int32_t` | | | `long int` (newlib-stdint) |

No `__int128`. The stack is 4-aligned (`STACK_BOUNDARY` 32).

**Bit-fields use the Microsoft layout** (`TARGET_MS_BITFIELD_LAYOUT_P`
returns true unless the struct is packed): a bit-field shares a storage
unit only with neighbours whose declared types have the same SIZE, a
change of size starts a new unit aligned for the new type, a bit-field's
type raises the struct's alignment even when unnamed, and a `:0` is
ignored unless it follows a bit-field. So `struct { char a; int b:4; }`
is 8 bytes and `struct { int a:4; char b:4; int c:4; }` is 12, where
the SysV layout makes them 4 and 4. EmbCC implements GCC's rules
(`stor-layout.cc`, `place_field`) for this target and checks them
against GCC's `sizeof`/`offsetof`.

## Symbols

**Every C symbol is prefixed with an underscore** in the object
(`__USER_LABEL_PREFIX__` is `_`): `main` is `_main`, `memcpy` is
`_memcpy`, the soft-float helper `__addsf3` is `___addsf3`. EmbCC's ELF
writer adds the prefix for this target, `__asm__("name")` labels are
written as given, and EmbLD's `-e` names the symbol as it is in the
object.

## The calling convention

From `rx.cc` (`rx_function_arg`, `rx_function_value`,
`rx_return_in_memory`, `rx_struct_value_rtx`) and GCC's output:

- **Registers.** r0 is the stack pointer. Arguments r1-r4; result r1
  (r1:r2 for 8 bytes, r1-r4 for up to 16). Callee-saved r6-r13;
  caller-saved r1-r5, r14, r15. r15 carries the hidden result pointer.
- **Arguments** are counted in bytes, each rounded up to whole words.
  An argument goes in the next registers when ALL of it fits in what is
  left of r1-r4; otherwise it goes on the stack -- and the count advances
  past it either way. So `f(int, int, int, long long, int)` puts the long
  long AND the last int on the stack, and `f(struct6, int)` (a 6-byte
  struct is not a whole number of words, so it is stacked) passes the int
  in **r3**.
- **Aggregates** of a whole number of words up to 16 bytes travel in
  registers like any other argument (`struct {int a, b;}` after an int is
  r2:r3, packed little-endian); any other aggregate is copied onto the
  stack. Nothing goes by reference.
- **The stack** arguments are laid out in order from the caller's sp,
  each at its type's natural alignment (at most 4) and occupying its own
  size -- a `char` takes one byte, a `short` two: `f(int, char, short,
  int, char e, short f2, char g, long long h)` stores e at sp+0, f2 at
  sp+2, g at sp+4 and h at sp+8. The callee finds them at its entry sp+4,
  above the return address `bsr` pushed.
- **Narrow arguments are not extended by the caller**: the callee
  extends a `char` or `short` parameter itself (`movu.b r2, r2`). A
  narrow RESULT is extended by the callee (`rx_function_value` promotes
  it to SImode).
- **Variadic calls.** Every unnamed argument AND the last named one go
  on the stack (GCC treats the last named argument as unnamed because RX
  has no `setup_incoming_varargs`). So `va_list` is a `char *` walking
  the caller's stack block, and `va_arg` rounds it up to the type's
  alignment (at most 4) and steps by the size.
- **Results.** Scalars in r1 (r1:r2, low word in r1). A struct or union
  of 1..16 bytes whose size is a multiple of 4 comes back in r1..r4; any
  other is written through the pointer the caller passes in **r15**, and
  the callee returns that pointer in r1. A `_Complex` is returned in
  registers like an aggregate of its size (`_Complex float` in r1, r2;
  `_Complex short` packed in r1).

## The instruction set the backend uses

Encodings: src/arch/rx/emit.c, refereed by tests/golden/rx-encoding.sh.

- **Moves and constants**: `mov.l rs, rd`; `mov.l #imm, rd` in its
  shortest form (#uimm4 2 bytes, #uimm8 3, else the li field: 1-4 bytes
  sign-extended); `mov.l #sym, rd` with a four-byte field for addresses.
- **Loads/stores**: `mov.size dsp[rs], rd` (sign-extending for .b/.w),
  `movu.b/.w` (zero-extending), stores, `mov.size #imm, dsp[rd]`,
  `[ri, rb]` indexed with the index scaled. Displacements are unsigned
  and scaled by the size (0..65535 units); misaligned addresses are
  legal (`STRICT_ALIGNMENT` 0), so packed members need nothing special.
- **Arithmetic**: two-operand `add/sub/and/or/xor/mul/cmp/tst rs, rd`,
  three-operand `add/sub/and/or/mul rs, rs2, rd` and `add #imm, rs, rd`,
  `adc`/`sbb` for 64-bit carries, `neg`/`not`/`abs`, `max`/`min`,
  `div`/`divu` (quotient only; a remainder is `a - q*b`), `emul`/`emulu`
  (32x32 -> 64 into a register pair), shifts by register or constant
  (`shll/shlr/shar`), `rotl`/`rotr`, `revl`/`revw` (byte swaps).
- **Comparisons**: `cmp` sets the flags of `rd - rs`; `scCND.l rd`
  materialises a condition; `bCND` branches. RX's carry after a compare
  is NOT a borrow: C set means unsigned >=.
- **Branches**: `bCND.b` (+-128 bytes from the branch itself),
  `beq.w`/`bne.w`/`bra.w` (+-32 KiB), `bra.a` (+-8 MiB). A function is
  generated with every branch short; one that does not reach takes a
  longer form and the function is generated again until nothing new
  fails.
- **Calls**: `bsr.a sym` (R_RX_DIR24S_PCREL, +-8 MiB) and `jsr rN`;
  the return address is PUSHED (`bsr` is a call like x86's), so a
  callee's incoming stack arguments start at sp+4.
- **Frames**: `pushm r6-rN` (or `push.l`), `add #-frame, r0`, and the
  epilogue's single `rtsd #frame+4n, r6-rN`, which releases the frame,
  pops the saved registers and returns.

### Where QEMU and the manual disagree

- QEMU decodes the 16-bit displacement of `movu.b/.w dsp:16[rs]` and of
  `mov.size #imm, dsp:16[rd]` as SIGNED (the manual's is unsigned, and
  QEMU's other forms read it unsigned). The encoder never emits either
  above 32767 units (`rx_load_ok`, `rx_store_imm_ok`), so the emulator
  and the hardware agree on every address EmbCC writes.
- QEMU's disassembler prints the register operands of the long-form
  store `mov.size rs, dsp[rd]` the wrong way round (its translator, GNU
  as and the manual put the base in the high nibble). rx-encoding.sh
  writes QEMU's text for that form, and GNU as (when installed) checks
  the bytes.

## Relocations and the object file

`e_machine` is `EM_RX` (173), ELFCLASS32, little-endian, RELA
relocations, `e_flags` `E_FLAG_RX_ABI` (0x8) -- what GCC's objects carry
for the same options. EmbCC names its sections `.text`, `.data`,
`.rodata`, `.bss`; GCC names them the Renesas way (`P`, `D`, `D_1`,
`D_2`, `C`, `C_1`, `C_2`, `B`, `B_1`, `B_2`, `W`), and EmbLD maps those
onto the same output sections, so GCC-built objects link with EmbCC's.

| Type | Field | Value |
| --- | --- | --- |
| `R_RX_DIR32` (1) | a data word, or `mov.l #imm32`'s field | S + A |
| `R_RX_DIR24S_PCREL` (9) | `bsr.a`/`bra.a`'s 24 bits | S + A - P + 1 |
| `R_RX_DIR16S_PCREL` (10) | `bsr.w`/`bra.w`'s 16 bits | S + A - P + 1 |
| `R_RX_DIR8S_PCREL` (11) | `bCND.b`'s 8 bits | S + A - P + 1 |

The `+ 1` is binutils' (`elf32-rx.c`): the field is one byte after the
opcode, and the displacement is measured from the opcode. GNU as writes
addend 0 for `bsr.a foo`. The linker-relaxation and complex-expression
relocations (`R_RX_RH_RELAX`, `R_RX_SYM`/`R_RX_OP*`, the GP-relative
ones) are refused by name or handled where GCC's objects need them.

## The board: QEMU `gdbsim-r5f562n8`

- **Memory.** 96 KiB internal RAM at 0, 512 KiB code flash at
  0xfff80000 (the fixed vector table, with the reset vector at
  0xfffffffc, is its top), and the gdbsim's 16 MiB of external SDRAM at
  0x01000000.
- **Loading.** `-kernel FILE` copies a RAW image to the second half of
  the SDRAM (0x01800000 for the default 16 MiB) and starts the CPU
  there; `-bios FILE` puts a raw image in the flash, whose last word is
  the reset vector. Neither reads ELF. The harness links at 0x01800000
  and turns the ELF into a raw image (`embld --oformat binary`, or the
  load segments by hand), with the stack's top at 0x02000000.
- **Output.** SCI0 at 0x00088240 is the first serial port: write the
  byte to TDR (+3) once SSR (+4) bit 7 (TDRE) is set; transmission is
  enabled by setting SCR (+2) bit 5 (TE), and BRR 0 makes QEMU's
  per-byte delay zero.
- **Ending a run.** Nothing in the gdbsim powers it off or resets it
  from software, so the harness prints a sentinel (`==EXIT n ==`) and
  the runner (tests/harness/qrun.sh --until) stops at it; the image then
  waits in a `wait` loop.

## What the first backend refuses

By name: the FPU and `-m64bit-doubles`, big-endian data, `-mgcc-abi`,
small data and PID (`-msmall-data-limit`, `-mpid`), interrupt and fast
interrupt functions until the harness can check them, inline assembly
and `.s` files (no RX assembler yet), C++ (its front end lays out LP64
and SysV bit-fields), `__int128`, and whatever the exec corpus finds.

## Tests

| Test | What it checks |
| --- | --- |
| `tests/golden/rx-encoding.sh` | every encoder form against QEMU's decoder, and GNU as when installed; every range check |
| `tests/golden/rx-exec.sh` | `tests/exec/*.c` on the gdbsim board at -O0, -O1, -O2 and -Os |
| `tests/golden/rx-refuse.sh` | the object's header, the accepted and refused options and constructs |

## Status

1. Plan, encoder and referee: done (9250 forms decode in QEMU as meant,
   9061 of them byte for byte as GNU as 2.47 encodes them; 32 range
   checks). Shown to fail against five mutants.
2. The target (`rx-none-elf`, aliases `rx-elf`, `rx-unknown-elf`, `rx`),
   its data model (size_t/ptrdiff_t/wchar_t `long`, the Microsoft
   bit-field layout), the predefined macros (tools/gen-predef.sh from
   rx-elf-gcc -nofpu), the code generator at -O0 and with the allocator at
   -O1/-O2/-Os, EmbLD for EM_RX (DIR32 and the PC-relative fields, the
   underscored linker symbols, the -Tstack stub), lib/rt (the binary32
   soft float lib/rt/avrfp*.c already had) and lib/libc (printf learns a
   binary32 double), the gdbsim harness, tests/golden/rx-exec.sh: 196 of
   196 at every level, 23 of them judged by the status GCC's code exits
   with, 18 not applicable. Shown to fail against a condition mutant.

Shared fixes the board found: a static initializer of a four-byte double
or long double (AVR's too) was stored as a binary64's low word or 16 bytes
over its neighbours; a four-byte complex now calls the binary32 `s`
helpers; and of two weak definitions EmbLD now keeps the first, as GNU ld
does -- the harness's weak write() lost to lib/libc's.
