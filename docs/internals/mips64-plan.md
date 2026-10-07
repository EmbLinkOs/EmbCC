# MIPS64 (n64, soft float): the plan

The targets are 64-bit MIPS, MIPS64 Release 2, the n64 ABI with soft
float, in both byte orders: `mips64el-none-elf` (little-endian) and
`mips64-none-elf` (big-endian), with clang's `-unknown-elf` spellings as
aliases. The board they are tested on is QEMU's `malta` with a `5KEc`
core (MIPS64r2, no FPU). Every fact below was read off clang 23
(`--target=mips64el-unknown-elf -mcpu=mips64r2 -msoft-float
-mno-abicalls -G0`), llvm-mc or QEMU 11.1 rather than remembered; there
is no MIPS gcc here.

## One backend for both widths

MIPS64 is the MIPS32 backend at 64 bits, as one RISC-V backend serves
RV32 and RV64 (D-016): `src/arch/mips/` holds one encoder, one inline
assembler and one code generator, and `g_m64` (from the target) selects
the width. The delay slots and their filling, the branches and their
long form, HI/LO, the misaligned paths, the jump tables and the
relocation sites are the same machine and are written once. What the
width changes:

- **A register is eight bytes** (`W`), and an operation at `w == 8` is
  the doubleword instruction: `daddu`, `dsubu`, `daddiu`, `dsll`/`dsll32`,
  `dmult` + `mflo` (Release 2 has no three-operand `dmul`), `ddiv`, `ld`,
  `sd`, `ldl`/`ldr`. A pointer is a doubleword, so address arithmetic
  (`P_ADDU`, `P_ADDIU`) is too.
- **32-bit values are kept sign-extended**, `unsigned` included -- n64's
  invariant for registers, and the 32-bit instructions' precondition
  (`addu` of an operand that is not sign-extended is UNPREDICTABLE). The
  readers that need it ask (`rd32`), and a map of the values already
  sign-extended (`sext_map`, the LoongArch and RV64 backends') makes most
  of them free. A four-byte unsigned load is `lwu` (zero-extending), as on
  every other 64-bit target here, which the optimizer may rely on.
- **The pair machinery is the 128-bit machinery.** MIPS32 keeps a
  `long long` or `double` in a register pair and computes on it with
  carries, double shifts and two-word compares; at 64 bits the same code,
  with the doubleword instructions and `HB = 64`, computes on an
  `__int128` or a binary128 `long double` in two doublewords. Such a
  value lives in its 16-byte slot, never in registers (the pair pass is
  off). Division, remainder and every binary128 operation and conversion
  call lib/rt (`__divti3`, `__addtf3`, ...).
- **The scratch registers move**: n64 makes `$8-$11` argument registers
  (`a4-a7`), so the code generator's scratches are `$12-$15` (n64's
  `t0-t3`), `t8`, `t9` (which doubles as `B_HI`; it is the call register
  only between an indirect call's last argument and the `jalr`), and
  `$at` as FAR (the comparison register too: a far frame slot is never
  addressed between a comparison into `$at` and its branch). The
  allocator's pool is `v0`, `v1`, `a0-a7` and `s0-s7`.

## The n64 calling convention

- **Arguments** take doubleword slots: the first eight in `a0-a7`, the
  rest on the stack from the caller's `sp + 0` -- there is no home area.
  An argument aligned to 16 (a `long double`, a struct with 16-byte
  alignment) starts at an even slot, skipping one; a named `__int128`
  does NOT (clang places it at the next slot). A composite of any size
  travels by value, its bytes as the doublewords `ld` would read --
  a short one left-justified big-endian -- split across `a7` and the
  stack when it straddles them. A 32-bit value travels sign-extended.
- **Variadic arguments** follow the same slots, a `float` promoted to
  `double`. An unnamed `__int128` starts at an even slot: that is where
  clang's `va_arg` (and GCC) read it, although clang's own callers do
  not put it there -- a clang-to-clang variadic `__int128` is misread.
  EmbCC's callers and `va_arg` agree with clang's `va_arg` and GCC.
  A variadic callee stores `a0-a7` into the top 64 bytes of its frame,
  just below the incoming stack words, so `va_list` is a `void *`.
- **Results**: a scalar in `v0`, a 32-bit one sign-extended; an
  `__int128` in `v0:v1`; a **binary128 `long double` in `v0` and `a0`**
  (clang's `RetCC_F128SoftFloat`, after GCC), its first doubleword in
  memory in `v0`. A composite of at most 16 bytes comes back in `v0:v1`
  as the doublewords `ld` reads, except: a structure of one or two
  floating-point fields (the first at offset 0) returns each field in its
  own register, positioned as `ld` at the field would read it -- a float
  in the high half big-endian; a structure of one `long double` as a
  `long double`; and a `_Complex float` or `double` each part in its own
  register as a scalar. A larger composite comes back through a hidden
  pointer in `a0`, returned in `v0`.
- `s0-s7`, `gp`, `sp` and `fp` survive a call. The stack is 16-aligned.

`tests/golden/mips64-abi.sh` and `mips64-be-abi.sh` check every rule above
with EmbCC and clang calling each other on the board, at -O0 and -O2.

## Data model

LP64: `int` 4, `long` and pointers 8, `long long` 8, `__int128` 16,
`long double` IEEE binary128 (16/16). Plain `char` is signed, `wchar_t`
a signed `int`; an unnamed bit-field does not raise a structure's
alignment. `__BIGGEST_ALIGNMENT__` is 16.

## Code model and relocations

Addresses are absolute and 64-bit, clang's default for n64 without
abicalls: `lui %highest`, `daddiu %higher`, `dsll 16`, `daddiu %hi`,
`dsll 16`, `daddiu %lo` -- each piece rounded for the sign extension of
the ones added after it. Calls are `jal` (`R_MIPS_26`), within the
256 MiB region of the delay slot.

Objects are ELFCLASS64 with RELA relocations. n64's `r_info` is not
`ELF64_R_INFO` but a record -- the symbol's 32 bits, then `r_ssym`,
`r_type3`, `r_type2`, `r_type`, a byte each -- in the object's byte
order; EmbCC writes each relocation as one type with `R_MIPS_NONE` in the
other two, as clang does, and EmbLD reads the record (refusing a
composite relocation by name). `e_flags` is `0x80000001`
(`EF_MIPS_ARCH_64R2 | EF_MIPS_NOREORDER`, no ABI field: ELFCLASS64 is
n64). `.MIPS.abiflags` says ISA MIPS64r2, 64-bit GPRs, soft float.

EmbLD applies `R_MIPS_64`, `R_MIPS_32`, `R_MIPS_26`, `R_MIPS_HIGHEST`,
`R_MIPS_HIGHER`, `R_MIPS_HI16`, `R_MIPS_LO16` and `R_MIPS_PC16` with
their RELA addends, reads and writes big-endian ELF64, and drops
`.MIPS.options` with the other MIPS records. `-Tstack` emits `li sp` and
`lui`/`ori`/`jr t9` to the entry, which at MIPS64 requires both addresses
to be sign-extended 32-bit (KSEG0 or KSEG1), and says so otherwise.

## The board: QEMU malta, 5KEc

`qemu-system-mips64el` / `qemu-system-mips64 -M malta -cpu 5KEc`, the
image loaded with `-kernel` at its link address 0xffffffff80100000
(KSEG0 sign-extended), the stack at 0xffffffff80800000.
`tests/harness/mips`'s `boot.c` and `io.c` serve both widths (their
KSEG addresses are written `KSEG(a)`, the sign extension at 64 bits);
`tests/harness/mips64/run.sh` and `link.sh` are the 64-bit runner and
linker.

## Big-endian at 64 bits

`mips64-none-elf` is the first big-endian target with `__int128` and a
binary128 `long double`, and the exec corpus found what assumed
little-endian there outside the backend: lib/rt's `w128` union and
softtf's halves (`__udivti3` swapped them), printf's binary128 decoder,
the `long double` bit builtins (`signbit`, `isinf`, ...) reading the sign
from byte 14, and a 17-byte bit-field unit in static data. Each is now in
memory order. Reading a packed bit-field that spans more than eight bytes
is still refused by name big-endian (irgen's byte-at-a-time forms are
little-endian).

## What is refused, by name

Atomics on 1, 2 and 16 bytes (`ll`/`sc` and `lld`/`scd` are a word and a
doubleword); a computed `goto`; `__builtin_frame_address` and
`__builtin_return_address`; interrupt functions; a scalar local aligned
beyond the 16-byte stack; unwind tables; C++ big-endian (its constant
evaluator is little-endian); every machine option but the one
configuration emitted (`-mcpu`/`-march` `mips64r2`, `5kc`, `5kf`, `5kec`,
`5kef`, `octeon`; `-mabi=64`; `-msoft-float`; `-mno-abicalls`; `-G0`; the
triple's own `-EL`/`-EB`). The inline-assembly vocabulary is MIPS32's:
a doubleword instruction in a template (`daddu`, `ld`) is refused as not
in the vocabulary, and gas's `la` (a 32-bit address) by name. C++ is
refused big-endian; little-endian it compiles with `-fno-exceptions` and
the unwind tables off, and is not yet tested on the board.

## Tests

| Test | What it checks |
| --- | --- |
| `tests/golden/mips64-encoding.sh` | every MIPS64r2 form against `llvm-mc -show-encoding`, both byte orders; `mips_li64` executed; every range check, and every doubleword form refused with the 64-bit switch off |
| `tests/golden/mips64-exec.sh`, `mips64-be-exec.sh` | `tests/exec/*.c` on the board at -O0, -O1, -O2 and -Os; big-endian, the little-endian assumptions against clang's result on the same board |
| `tests/golden/mips64-abi.sh`, `mips64-be-abi.sh` | calls in both directions against clang |
| `tests/golden/mips64-data.sh` | static data byte for byte against clang, in both orders, then on the board |
| `tests/golden/mips64-refuse.sh` | the object's header and flags, the accepted and refused options and constructs |
