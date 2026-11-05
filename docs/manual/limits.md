# What each target compiles

<!-- Generated from tests/golden/limits/expected.txt by
     `sh tests/golden/limits.sh --update`: do not edit by hand. -->

Each construct below was compiled for every target, and this page records
what EmbCC did: compiled it, accepted it with a warning, or refused it with
an error that names it. EmbCC never compiles a construct it refuses into
code that does something else (see
[Status and known limitations](../internals/status.md#how-embcc-refuses)).
A construct that compiles may still have limits on a target; each target's
section of [Targets](targets.md) gives them. `tests/golden/limits.sh` checks
every cell of this page against the compiler.

Where a message names its own target, it is written `TARGET` here.
Every compile for `x86_64-windows-gnu` also warns that the target is not
yet the Microsoft x64 ABI ([Windows](targets.md#windows-coff)).

## `__int128`

Refused on 16 of the 25 targets.

- Refused on `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.base-none-eabi`, `thumbv8m.main-none-eabi`, `armv7a-none-eabi`, `riscv32-unknown-elf`, `avr`, `mipsel-none-elf`, `mips-none-elf`, `xtensa-none-elf`, `tricore-none-elf`, `rx-none-elf`, `powerpc-none-eabi`, `sparc-none-elf`, `m68k-none-elf`:

  ```text
  __int128 does not exist on this target (it needs 64-bit registers; use long long)
  ```

## `long double` arithmetic

Compiles on every target.

## `_Float16`

Compiles on every target.

## `__fp16`

Compiles on every target.

## `__bf16`

Compiles on every target.

## a struct holding a `_Float16`, by value

Refused on 1 of the 25 targets.

- Refused on `thumbv7em-none-eabihf`:

  ```text
  'f': a struct holding a _Float16, passed or returned by value, is not supported yet with the hard-float calling convention, which puts each binary16 member in a floating-point register of its own; pass a pointer to it
  ```

## a `_Float16` through `...`

Refused on 25 of the 25 targets.

- Refused on `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.base-none-eabi`, `thumbv8m.main-none-eabi`, `armv7a-none-eabi`, `riscv32-unknown-elf`, `riscv64-unknown-elf`, `avr`, `mipsel-none-elf`, `mips-none-elf`, `mips64el-none-elf`, `mips64-none-elf`, `loongarch64-unknown-elf`, `xtensa-none-elf`, `tricore-none-elf`, `rx-none-elf`, `powerpc-none-eabi`, `sparc-none-elf`, `m68k-none-elf`, `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-windows-gnu`:

  ```text
  'g': passing a _Float16 through '...' is not supported yet: it is not promoted there, and EmbCC does not yet place an unpromoted 16-bit float among the variable arguments; convert it to double, as va_arg will read it
  ```

## `_BitInt(N)`

Compiles on every target.

## `_Complex int`

Compiles on every target.

## GNU vector types (`vector_size`)

Compiles on every target.

## one-byte atomic read-modify-write

Compiles on every target.

## eight-byte atomic read-modify-write

Compiles on every target.

## sixteen-byte compare-and-swap

Refused on 20 of the 25 targets.

- Refused on `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.base-none-eabi`, `thumbv8m.main-none-eabi`, `armv7a-none-eabi`, `riscv32-unknown-elf`, `avr`, `mipsel-none-elf`, `mips-none-elf`, `xtensa-none-elf`, `tricore-none-elf`, `rx-none-elf`, `powerpc-none-eabi`, `sparc-none-elf`, `m68k-none-elf`:

  ```text
  __int128 does not exist on this target (it needs 64-bit registers; use long long)
  ```

- Refused on `riscv64-unknown-elf`:

  ```text
  the RV64 backend cannot lower a 128-bit value yet (function f) [cas16 w=16 size=16]
  ```

- Refused on `mips64el-none-elf`, `mips64-none-elf`:

  ```text
  the MIPS64 backend cannot lower a 16-byte atomic (MIPS64's lld/scd are a doubleword; there is no 128-bit ll/sc) yet (function f) [cas16 w=16 size=16]
  ```

- Refused on `loongarch64-unknown-elf`:

  ```text
  the LoongArch64 backend cannot lower a sixteen-byte atomic (the LA64 base ISA has no 128-bit ll/sc or am* instruction) yet (function f) [cas16 w=16 size=16]
  ```

## `_Thread_local`

Compiles on every target.

## variable-length arrays

Compiles on every target.

## a local aligned to 32 bytes

Compiles on every target.

## `__builtin_frame_address(0)`

Compiles on every target.

## `__builtin_return_address(1)`

Refused on 19 of the 25 targets.

- Refused on `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.base-none-eabi`, `thumbv8m.main-none-eabi`, `armv7a-none-eabi`, `riscv32-unknown-elf`, `riscv64-unknown-elf`, `avr`, `mipsel-none-elf`, `mips-none-elf`, `mips64el-none-elf`, `mips64-none-elf`, `loongarch64-unknown-elf`, `xtensa-none-elf`, `tricore-none-elf`, `rx-none-elf`, `powerpc-none-eabi`, `sparc-none-elf`:

  ```text
  __builtin_return_address(1) is not supported on TARGET: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found
  ```

## `__builtin_setjmp`

Refused on 1 of the 25 targets.

- Refused on `x86_64-windows-gnu`:

  ```text
  __builtin_setjmp is not supported for TARGET: MinGW's setjmp takes a frame for its unwinder, and EmbCC does not lower onto it
  ```

## computed `goto`

Compiles on every target.

## `asm goto`

Compiles on every target.

## nested functions

Refused on 25 of the 25 targets.

- Refused on `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.base-none-eabi`, `thumbv8m.main-none-eabi`, `armv7a-none-eabi`, `riscv32-unknown-elf`, `riscv64-unknown-elf`, `avr`, `mipsel-none-elf`, `mips-none-elf`, `mips64el-none-elf`, `mips64-none-elf`, `loongarch64-unknown-elf`, `xtensa-none-elf`, `tricore-none-elf`, `rx-none-elf`, `powerpc-none-eabi`, `sparc-none-elf`, `m68k-none-elf`, `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-windows-gnu`:

  ```text
  expected ';' before '{' [E0002]
  ```

## `__attribute__((interrupt))`

Refused on 9 of the 25 targets.

- Refused on `loongarch64-unknown-elf`, `xtensa-none-elf`, `powerpc-none-eabi`, `sparc-none-elf`, `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-windows-gnu`:

  ```text
  __attribute__((interrupt)) is not supported: the handler would return with an ordinary return instead of the interrupt return the CPU needs, and without saving the registers (on ARMv7-M it needs neither, and is accepted; on AVR, RISC-V, MIPS, RX, ColdFire, TriCore and ARMv7-A it is implemented)
  ```

## `__attribute__((naked))`

Refused on 4 of the 25 targets.

- Refused on `x86_64-elf`, `x86_64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-windows-gnu`:

  ```text
  __attribute__((naked)) is not supported: on this target the body could only be assembled by the file-scope assembler's few instructions; it is supported on every target but x86-64 and AArch64 for Darwin or Windows
  ```

## an assembly source (`.s`) with an instruction

Refused on 1 of the 25 targets.

- Refused on `x86_64-windows-gnu`:

  ```text
  no assembly-file support for TARGET yet
  ```

## `-S` (assembly output)

Compiles on every target.

## `-g` (DWARF)

Compiles on every target.

## `--unwind-tables`

Refused on 6 of the 25 targets.

- Refused on `avr`:

  ```text
  unwind tables are not supported for TARGET yet (--unwind-tables, --exceptions): EmbCC writes no AVR .eh_frame
  ```

- Refused on `loongarch64-unknown-elf`:

  ```text
  unwind tables are not supported for TARGET yet (--unwind-tables, --exceptions): EmbCC writes no LoongArch .eh_frame
  ```

- Refused on `rx-none-elf`:

  ```text
  unwind tables are not supported for TARGET yet (--unwind-tables, --exceptions): EmbCC writes no RX .eh_frame
  ```

- Refused on `powerpc-none-eabi`:

  ```text
  unwind tables are not supported for TARGET yet (--unwind-tables, --exceptions): EmbCC writes no PowerPC .eh_frame
  ```

- Refused on `sparc-none-elf`:

  ```text
  unwind tables are not supported for TARGET yet (--unwind-tables, --exceptions): EmbCC writes no SPARC .eh_frame
  ```

- Refused on `m68k-none-elf`:

  ```text
  unwind tables are not supported for TARGET yet (--unwind-tables, --exceptions): EmbCC writes no ColdFire .eh_frame
  ```

## C++ (without exceptions)

Compiles on every target.

## C++ exceptions

Refused on 6 of the 25 targets.

- Refused on `avr`:

  ```text
  'try' with exceptions disabled (-fno-exceptions)
  ```

- Refused on `loongarch64-unknown-elf`:

  ```text
  C++ exceptions are not supported for TARGET yet: EmbCC writes no LoongArch .eh_frame; compile with --no-exceptions
  ```

- Refused on `rx-none-elf`:

  ```text
  C++ exceptions are not supported for TARGET yet: EmbCC writes no RX .eh_frame; compile with --no-exceptions
  ```

- Refused on `powerpc-none-eabi`:

  ```text
  C++ exceptions are not supported for TARGET yet: EmbCC writes no PowerPC .eh_frame; compile with --no-exceptions
  ```

- Refused on `sparc-none-elf`:

  ```text
  C++ exceptions are not supported for TARGET yet: EmbCC writes no SPARC .eh_frame; compile with --no-exceptions
  ```

- Refused on `m68k-none-elf`:

  ```text
  C++ exceptions are not supported for TARGET yet: EmbCC writes no ColdFire .eh_frame; compile with --no-exceptions
  ```

## The whole matrix

| Target | int128 | ldouble | float16 | fp16 | bf16 | float16struct | float16va | bitint | complexint | vector | atomic1 | atomic8 | atomic16 | tls | vla | align32 | frameaddr | retaddr1 | setjmp | cgoto | asmgoto | nested | interrupt | naked | asmfile | dashS | debug | unwind | cxx | cxxeh |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `thumbv6m-none-eabi` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `thumbv7m-none-eabi` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `thumbv7em-none-eabihf` | no | yes | yes | yes | yes | no | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `thumbv8m.base-none-eabi` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `thumbv8m.main-none-eabi` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `armv7a-none-eabi` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `riscv32-unknown-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `riscv64-unknown-elf` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `avr` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | no |
| `mipsel-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `mips-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `mips64el-none-elf` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `mips64-none-elf` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `loongarch64-unknown-elf` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | no | yes | yes | yes | yes | no | yes | no |
| `xtensa-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | no | yes | yes | yes | yes | yes | yes | yes |
| `tricore-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes |
| `rx-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | no |
| `powerpc-none-eabi` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | no | yes | yes | yes | yes | no | yes | no |
| `sparc-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | no | yes | yes | yes | no | no | yes | yes | yes | yes | no | yes | no |
| `m68k-none-elf` | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | no | yes | no |
| `x86_64-elf` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | no | no | no | yes | yes | yes | yes | yes | yes |
| `aarch64-elf` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | no | no | yes | yes | yes | yes | yes | yes | yes |
| `x86_64-linux-gnu` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | no | no | no | yes | yes | yes | yes | yes | yes |
| `aarch64-apple-darwin` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | no | no | no | yes | yes | yes | yes | yes | yes |
| `x86_64-windows-gnu` | yes | yes | yes | yes | yes | yes | no | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | yes | no | yes | yes | no | no | no | no | yes | yes | yes | yes | yes |

The columns are the sections above, in order:
- `int128`: `__int128`
- `ldouble`: `long double` arithmetic
- `float16`: `_Float16`
- `fp16`: `__fp16`
- `bf16`: `__bf16`
- `float16struct`: a struct holding a `_Float16`, by value
- `float16va`: a `_Float16` through `...`
- `bitint`: `_BitInt(N)`
- `complexint`: `_Complex int`
- `vector`: GNU vector types (`vector_size`)
- `atomic1`: one-byte atomic read-modify-write
- `atomic8`: eight-byte atomic read-modify-write
- `atomic16`: sixteen-byte compare-and-swap
- `tls`: `_Thread_local`
- `vla`: variable-length arrays
- `align32`: a local aligned to 32 bytes
- `frameaddr`: `__builtin_frame_address(0)`
- `retaddr1`: `__builtin_return_address(1)`
- `setjmp`: `__builtin_setjmp`
- `cgoto`: computed `goto`
- `asmgoto`: `asm goto`
- `nested`: nested functions
- `interrupt`: `__attribute__((interrupt))`
- `naked`: `__attribute__((naked))`
- `asmfile`: an assembly source (`.s`) with an instruction
- `dashS`: `-S` (assembly output)
- `debug`: `-g` (DWARF)
- `unwind`: `--unwind-tables`
- `cxx`: C++ (without exceptions)
- `cxxeh`: C++ exceptions
