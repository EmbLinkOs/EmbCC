# Targets

This page describes each machine EmbCC generates code for: the target
triples it accepts, its machine-specific options, its data model, the
calling convention EmbCC implements for it, the object format, the
predefined macros that identify it, the runtime library it needs, and
what it does not support, with the diagnostic EmbCC gives. It is for
people building code for a particular target and for people checking that
EmbCC's objects will link with another compiler's. General options are in
[Invoking EmbCC](invoking.md); inline assembly per target is in
[Inline assembly](inline-asm.md); bare-metal startup, linking and
interrupt handlers are in [Embedded programming](embedded.md).

## Summary

One `embcc` process compiles for one target. Every target is
little-endian but the big-endian MIPS ones, `mips-none-elf` and
`mips64-none-elf`.

| Family | Canonical triples | Object format | Calling convention | Linked by |
|---|---|---|---|---|
| [x86-64](#x86-64) | `x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu` | ELF64 | System V AMD64 | `embcc` (integrated `embld`) |
| [x86-64 macOS](#macos-mach-o) | `x86_64-apple-darwin` | Mach-O | System V AMD64 | the system linker |
| [x86-64 Windows](#windows-coff) | `x86_64-windows-gnu` | COFF | Microsoft x64, incomplete | an external linker |
| [AArch64](#aarch64) | `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` | ELF64 | AAPCS64 | an external linker |
| [Apple arm64](#apple-arm64) | `aarch64-apple-darwin` | Mach-O | Apple arm64 | the system linker |
| [ARM Cortex-M](#arm-cortex-m) | `thumbv6m-none-eabi`, `thumbv8m.base-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` | ELF32 | AAPCS32, AAPCS-VFP | `embld` |
| [ARMv7-A](#armv7-a) | `armv7a-none-eabi`, `armv7a-none-eabihf` | ELF32 | AAPCS, AAPCS-VFP | `embld` |
| [RISC-V](#risc-v) | `riscv32-unknown-elf`, `riscv64-unknown-elf` | ELF32, ELF64 | RISC-V psABI, `ilp32`/`lp64` and the F and D conventions (`-mabi=`) | `embld` |
| [AVR](#avr) | `avr` | ELF32 | avr-gcc | `embld` |
| [MIPS32](#mips32) | `mipsel-none-elf` | ELF32 | o32, soft float | `embld` |
| [MIPS64](#mips64) | `mips64el-none-elf`, `mips64-none-elf` | ELF64 | n64, soft float | `embld` |
| [LoongArch64](#loongarch64) | `loongarch64-unknown-elf` | ELF64 | LoongArch psABI, LP64S (soft float) | `embld` |
| [TriCore](#tricore) | `tricore-none-elf` | ELF32 | TriCore EABI, soft float | `embld` |
| [Xtensa](#xtensa) | `xtensa-none-elf` | ELF32 | windowed, soft float | `embld` |
| [Renesas RX](#renesas-rx) | `rx-none-elf` | ELF32 | GCC rx-elf, 32-bit doubles, no FPU | `embld` |
| [SPARC](#sparc) | `sparc-none-elf` | ELF32, big-endian | SPARC V8 (register windows), soft float | `embld` |
| [ColdFire](#coldfire) | `m68k-none-elf` | ELF32, big-endian | m68k SVR4 (GCC m68k-elf), soft float | `embld` |

| Target | Status | Floating point | `-g` | Lock-free atomic read-modify-write | `__thread` | C++ |
|---|---|---|---|---|---|---|
| x86-64 ELF, EmbLinkOS, Linux | Primary target | SSE2; x87 for `long double` | DWARF | 1, 2, 4, 8, 16 bytes | Local-exec TLS | Yes, with exceptions |
| x86-64 macOS | Objects for the system linker | SSE2; x87 for `long double` | Refused | 1, 2, 4, 8, 16 bytes | Refused | Yes, with exceptions |
| x86-64 Windows | Objects only; warns on every compile | SSE2 | Refused | 1, 2, 4, 8, 16 bytes | Refused | Refused |
| AArch64 ELF, EmbLinkOS, Linux | Supported | FP/SIMD; `long double` in software | DWARF | 1, 2, 4, 8, 16 bytes | Local-exec TLS | Yes, with exceptions |
| Apple arm64 | Objects for the system linker | FP/SIMD | Refused | 1, 2, 4, 8, 16 bytes | Refused | Yes, with exceptions |
| Cortex-M, soft float | Bare metal | Software | DWARF | 1, 2, 4 bytes | One shared instance | Without exceptions |
| Cortex-M, FPU | Bare metal | Single-precision VFP; `double` in software | DWARF | 1, 2, 4 bytes | One shared instance | Without exceptions |
| ARMv7-A (A32), soft float | Bare metal | Software | DWARF | 1, 2, 4 bytes | One shared instance | Without exceptions |
| ARMv7-A (A32), VFP | Bare metal | VFPv3/VFPv4, single and double | DWARF | 1, 2, 4 bytes | One shared instance | Without exceptions |
| RV32 | Bare metal | Software | DWARF | 1, 2, 4 bytes | One shared instance | Without exceptions |
| RV64 | Bare metal | Software | DWARF | 1, 2, 4, 8 bytes | One shared instance | Without exceptions |
| AVR (ATmega328P) | Bare metal | Software, 4-byte `double` | DWARF, 4-byte addresses | 1, 2, 4, 8 bytes (interrupts masked) | One shared instance | Refused |
| MIPS32r2 (PIC32-class) | Bare metal | Software | DWARF | 4 bytes | One shared instance | Without exceptions |
| MIPS64r2 | Bare metal | Software | DWARF | 4, 8 bytes | One shared instance | Without exceptions (little-endian untested) |
| LoongArch64 | Bare metal | Software | DWARF | 1, 2, 4, 8 bytes | One shared instance | Without exceptions |
| TriCore 1.6.1 (AURIX) | Bare metal | Software | DWARF | 4 bytes | One shared instance | Without exceptions |
| Xtensa (ESP32, ESP32-S3) | Bare metal | Software | DWARF | 4 bytes | One shared instance | Without exceptions |
| Renesas RX (RXv1) | Bare metal | Software, 4-byte `double` | DWARF | 1, 2, 4 bytes (interrupts masked) | One shared instance | Without exceptions |
| SPARC V8 (LEON3) | Bare metal | Software; `long double` binary128 | DWARF | 4 bytes | One shared instance | Without exceptions |
| ColdFire ISA_A (MCF5208-class) | Bare metal | Software | DWARF | 1, 2, 4 bytes (interrupts masked; supervisor mode) | One shared instance | Without exceptions |

"One shared instance" means the object is placed in `.tbss` but
addressed as an ordinary static object: there is one copy, not one per
thread. "Refused" under C++ means a C++ unit is not compiled for that
target (`-fsyntax-only` still checks it); the diagnostic is in the
target's Limitations. "Without exceptions" means C++ is compiled with
`-fno-exceptions`; see [C++](cxx.md#targets). With exceptions on, the
32-bit embedded targets and big-endian MIPS64 refuse the unit by name;
on RV64, LoongArch and little-endian MIPS64 code that needs a landing
pad (a `try` block, or a destructor that must run during unwinding) does
not compile.

## Selecting a target

### `--target=TRIPLE`

Compile for `TRIPLE`. The option may appear anywhere on the command line;
it is applied before every other option, so `--version`, `-dumpmachine`
and `--dump-predef` describe the target that was asked for.

Each triple accepted, and the canonical name it is reported as, is listed
in the section for its family. An unknown triple is an error, followed by
the full list:

```text
embcc: error: unknown target 'armv6m-none-eabi'
embcc: the targets it emits for are:
embcc:   x86_64-elf
embcc:   x86_64
...
```

With no `--target=`, EmbCC compiles for `x86_64-elf`, unless the compiler
was built with `make DEFAULT_TARGET=TRIPLE` or the environment sets
`EMBCC_DEFAULT_TARGET`.

### `EMBCC_DEFAULT_TARGET`

An environment variable naming the default target. It takes precedence
over a default compiled in with `DEFAULT_TARGET`, and `--target=`
overrides both. An invalid value is an error on every invocation:

```text
embcc: error: the default target 'bogus' is not one EmbCC knows
embcc: it came from EMBCC_DEFAULT_TARGET in the environment
```

### `-dumpmachine`

Print the canonical triple of the selected target and exit. It is
answered after all other options are applied, so the ARM float options
are reflected in it:

```sh
embcc --target=thumbv7em-none-eabi -mfpu=fpv4-sp-d16 -mfloat-abi=hard -dumpmachine
# thumbv7em-none-eabihf
```

### `--dump-predef`

Print the target's predefined macros, one `#define` per line, and exit.
The list reflects `--target=`, `-mcpu=`, `-mfpu=` and `-mfloat-abi=`. It
does not include the macros the preprocessor defines for every target
(`__EMBCC__`, `__STDC__`, `__STDC_VERSION__`, `__STDC_HOSTED__`). EmbCC
does not define `__GNUC__` on any target.

## Data models

Size and alignment in bytes, written `size/alignment`. "x86-64" covers
`x86_64-elf`, `x86_64-emblink` and `x86_64-linux-gnu`; "AArch64" covers
`aarch64-elf`, `aarch64-emblink` and `aarch64-linux-gnu`; "Cortex-M"
covers every `thumb*` triple.

| Type | x86-64 | macOS x86-64 | Windows | AArch64 | Apple arm64 | Cortex-M | RV32 | RV64 | AVR | MIPS32 | LoongArch64 | TriCore | Xtensa | PowerPC | RX | SPARC | ColdFire |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| plain `char` | signed | signed | signed | unsigned | signed | unsigned | unsigned | unsigned | signed | signed | signed | signed | unsigned | unsigned | unsigned | signed | signed |
| `short` | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/1 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 |
| `int` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 2/1 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/2 |
| `long` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/4 | 4/4 | 8/8 | 4/1 | 4/4 | 8/8 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/2 |
| `long long` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/1 | 8/8 | 8/8 | 8/4 | 8/8 | 8/8 | 8/4 | 8/8 | 8/2 |
| pointer, `size_t` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/4 | 4/4 | 8/8 | 2/1 | 4/4 | 8/8 | 4/4 | 4/4 | 4/4 | 4/4 `long` | 4/4 | 4/2 |
| `float` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/1 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/2 |
| `double` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/1 | 8/8 | 8/8 | 8/4 | 8/8 | 8/8 | 4/4 | 8/8 | 8/2 |
| `long double` | 16/16 x87 | 16/16 x87 | 16/16 x87 | 16/16 binary128 | 8/8 binary64 | 8/8 binary64 | 16/16 binary128 | 16/16 binary128 | 4/1 binary32 | 8/8 binary64 | 16/16 binary128 | 8/4 binary64 | 8/8 binary64 | 8/8 binary64 | 4/4 binary32 | 16/8 binary128 | 8/2 binary64 |
| `wchar_t` | 4/4 `int` | 4/4 `int` | 4/4 `int` | 4/4 `unsigned int` | 4/4 `int` | 4/4 `unsigned int` | 4/4 `int` | 4/4 `int` | 2/1 `int` | 4/4 `int` | 4/4 `int` | 4/4 `int` | 2/2 `unsigned short` | 4/4 `int` | 4/4 `long` | 4/4 `int` | 4/2 `int` |
| `__int128` | 16/16 | 16/16 | 16/16 | 16/16 | 16/16 | — | — | 16/16 | — | — | 16/16 | — | — | — | — | — | — |
| `enum` (all values fit `int`) | 4 | 4 | 4 | 4 | 4 | 4 | 4 | 4 | 2 | 4 | 4 | 4 | 4 | 4 | 4 | 4 | 4 |
| `__BIGGEST_ALIGNMENT__` | 16 | 16 | 16 | 16 | 16 | 8 | 16 | 16 | 1 | 8 | 16 | 8 | 16 | 16 | 4 | 8 | 2 |
| Stack alignment at a call | 16 | 16 | 16 | 16 | 16 | 8 | 16 | 16 | 1 | 8 | 16 | 8 | 16 | 16 | 4 | 8 | 4 |

Notes on the table:

- `_Bool` is one byte everywhere.
- An enumeration without a fixed underlying type is `int` while every
  value fits `int`. Otherwise it takes, and its enumerators take, the
  first of these that holds every value: `unsigned int` (when no value is
  negative), `long`, `unsigned long` (when `long` is 8 bytes and no value
  is negative), and `long long`, which is `unsigned long long` when no
  value is negative. `enum { G = 0x100000005 }` is 8 bytes on every
  target. Clang chooses only among the unsigned types when no value is
  negative, so a non-negative enumeration that does not fit `unsigned
  int` can differ from Clang's: on the targets where `long` is 8 bytes it
  is `long` where Clang's is `unsigned long`, of the same size; on AVR
  one whose largest value lies between 2^16 and 2^31 - 1 is `long` where
  Clang's is `unsigned long`, and one whose largest value lies between
  2^31 and 2^32 - 1 is an 8-byte `unsigned long long` where Clang's is a
  4-byte `unsigned long`. A fixed underlying type
  (`enum e : unsigned char`) is used as written. `-fshort-enums` is
  refused.
- Plain `char`'s signedness can be changed with `-fsigned-char` and
  `-funsigned-char` (see [Invoking EmbCC](invoking.md)); the
  `__CHAR_UNSIGNED__` macro follows the option.
- "—" means the type does not exist on that target. Using it is an
  error: `__int128 does not exist on this target (it needs 64-bit
  registers; use long long)`.
- On RV32 and RV64, `long double` has the size and the IEEE binary128
  format the psABI specifies, and constant expressions involving it are
  folded exactly, but no operation on a `long double` value compiles:
  `the RV32 backend cannot lower a 128-bit value yet`. RV64's `__int128`
  is the same: it can be declared and measured with `sizeof`, and every
  operation on one is refused with the RV64 form of that message.
- On LoongArch64 `long double` and `__int128` are computed: binary128
  arithmetic and the 128-bit divides and variable shifts call lib/rt.
- On TriCore and RX nothing is aligned beyond 4 bytes. On RX `double` and
  `long double` are binary32 (GCC's `-m32bit-doubles`), and `size_t`,
  `ptrdiff_t` and `wchar_t` are `long` types. SPARC's binary128
  `long double` is 8-aligned. Xtensa's `wchar_t` is a 16-bit
  `unsigned short`. On every target, `-fshort-wchar` makes `wchar_t` an
  `unsigned short` (see [Invoking](invoking.md#-fshort-wchar--fno-short-wchar)).
- MIPS64 has LP64's sizes in the RV64 column: `long` and pointers 8/8,
  `long double` 16/16 binary128, `__int128` 16/16, a signed plain `char`
  and an `int` `wchar_t`, `__BIGGEST_ALIGNMENT__` and the stack 16 (see
  [MIPS64](#mips64)). `long double` and `__int128` are computed, binary128
  and the 128-bit divides through lib/rt.
- On AVR every type has alignment 1, so `struct { char c; int i; }` is
  three bytes.
- Windows uses the LP64 model here, which is not Microsoft's; see
  [Windows](#windows-coff).
- `max_align_t` is declared by `<stddef.h>` in C++ only.

## Options common to all targets

These x86 spellings are accepted on every target so that existing build
systems work. Only the first group changes anything, and only on x86-64
and AArch64.

| Option | Effect |
|---|---|
| `-mno-sse`, `-mno-sse2`, `-mgeneral-regs-only` | On x86-64: never use an SSE register. On AArch64: never use an FP/SIMD register. On every other target: no effect. See the x86-64 and AArch64 sections. |
| `-mno-mmx`, `-mno-80387` | Accepted; no effect. EmbCC emits no MMX code, and these do not imply `-mno-sse`. |
| `-mno-red-zone` | Accepted; no effect. EmbCC does not place data below the stack pointer. |
| `-mcmodel=MODEL` | Accepted with any value; no effect. Each target uses the single code model described in its section. |

Any other `-m` option not listed in a target's section is an error:
`embcc: error: unknown argument '-mfoo'`. In particular `-m32` and `-m64`
are not accepted, `-mmcu=` only on [AVR](#avr), and `-march=` and `-mabi=` only on MIPS
and LoongArch (see [MIPS32](#mips32) and [LoongArch64](#loongarch64)).
and (`-mabi=`) Xtensa (see [MIPS32](#mips32) and [Xtensa](#xtensa)).

## x86-64

### Triples

| Triple | Accepted aliases | Operating system | Object format |
|---|---|---|---|
| `x86_64-elf` | `x86_64`, `x86_64-none-elf` | none (bare metal, the EmbLinkOS kernel) | ELF64 |
| `x86_64-emblink` | | EmbLinkOS user programs | ELF64 |
| `x86_64-linux-gnu` | `x86_64-linux` | Linux, static executables | ELF64 |
| `x86_64-apple-darwin` | `x86_64-darwin` | macOS | Mach-O |
| `x86_64-windows-gnu` | `x86_64-w64-mingw32` | Windows (MinGW) | COFF |

### Options

#### `-mno-sse`, `-mno-sse2`, `-mgeneral-regs-only`

Generate no SSE instructions: no XMM register is saved by a variadic
prologue, and block copies and structure moves use general registers.
This is what a kernel needs before it enables `CR4.OSFXSR`. Any
floating-point operation, `long double` included, is then an error:

```text
embcc: f.c:3: error: floating point needs SSE, which -mno-sse forbids
```

The option does not change the predefined macros: `__SSE__` and
`__SSE2__` remain defined.

#### `-mcmodel=MODEL`, `-mno-red-zone`, `-mno-mmx`, `-mno-80387`

Accepted and ignored. The generated code is position-dependent and
addresses data and calls RIP-relatively (calls carry
`R_X86_64_PLT32`, data references `R_X86_64_PC32`), which also suits a
kernel linked in the top 2 GiB (`-mcmodel=kernel`). The red zone is never
used. Position-independent code is not available: `-fPIC`, `-fpic`,
`-fPIE` and `-fpie` are refused with `embcc: error: -fPIC is not
supported; EmbCC would emit ordinary code and the flag's promise would
not hold`, and `-shared` and `-static-pie` with `embcc: error: -shared
needs position-independent code, which EmbCC does not emit`.

### Calling convention: System V AMD64

`x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu` and
`x86_64-apple-darwin` use the System V AMD64 psABI.

- Integer and pointer arguments go in `rdi`, `rsi`, `rdx`, `rcx`, `r8`,
  `r9`; `float` and `double` in `xmm0`–`xmm7`; the rest on the stack in
  eight-byte slots. Results come back in `rax` (and `rdx`), `xmm0` (and
  `xmm1`).
- `long double` is class X87: it is passed in memory and returned in
  `st(0)`. `__int128` is passed in two general registers and returned in
  `rax:rdx`.
- An aggregate of up to 16 bytes is classified eightbyte by eightbyte as
  INTEGER or SSE and passed in registers. An eightbyte that holds only
  padding takes no register. An aggregate larger than 16 bytes, one that
  contains a `long double`, or a packed one with a member at an offset
  its type's alignment does not divide is class MEMORY and passed on the
  stack. If an aggregate's eightbytes do not all find a free register of
  their class, the whole aggregate goes on the stack and the remaining
  registers stay available to later arguments.
- A MEMORY-class result is written through a hidden pointer passed in
  `rdi` and returned in `rax`.
- A call to a variadic function sets `al` to the number of vector
  registers used. `va_list` is the 24-byte `__va_list_tag` record, and
  `va_arg` of a structure fetches it as the ABI places it.
- The stack is 16-byte aligned at every call. `rbx`, `rbp` and
  `r12`–`r15` are callee-saved.
- An unnamed bit-field does not affect a structure's alignment.

### Operating systems and object formats

#### Bare metal and EmbLinkOS

`x86_64-elf` and `x86_64-emblink` produce ELF64 relocatable objects.
They differ only in the predefined macros. An EmbLinkOS executable is
produced at link time: `embld --embx` turns the linked ELF image into an
EMBX image (see [embld](tools/embld.md)).

#### Linux

`x86_64-linux-gnu` produces ELF64 objects for a static executable linked
against EmbCC's own C library (`lib/libc/os/linux`), which issues system
calls directly; there is no glibc and no dynamic loader. `embcc file.c`
without `-c` links the program with the integrated linker, adding
`crt1.o`, `libc.a` and `librt.a` from the target's library directory.
If they are not built (`make libc-linux-x86_64`):

```text
embcc: error: no crt1.o for x86_64-linux-gnu -- the target's library is not built or not installed
embcc: --print-search-dirs says where it looked
```

#### macOS (Mach-O)

`x86_64-apple-darwin` produces Mach-O objects for the system linker. C
symbol names carry a leading underscore in the object; the predefined
`__USER_LABEL_PREFIX__` is nevertheless empty. The data model and calling
convention are those of `x86_64-elf`. The following are refused for both
Darwin triples, each with its own diagnostic:

| Construct | Diagnostic |
|---|---|
| `-g` | Warning, and the object is compiled without debug information: `-g: no debug information for aarch64-apple-darwin yet; d.c is compiled without it` |
| `__thread` | `__thread is not supported for a Darwin target yet: Mach-O addresses a thread-local through a __thread_vars descriptor, which this writer does not emit` |
| `__attribute__((constructor))`, `destructor` | `__attribute__((constructor)) is not supported for a Darwin target yet: it needs a __DATA,__mod_init_func section this Mach-O writer does not emit` |
| file-scope `__asm__` with labels or symbols | `a file-scope asm block with labels or symbol references is not supported for a Darwin target yet: its bytes would be emitted but its symbols and relocations dropped` |
| `__attribute__((alias))` | `alias attribute on 'g' is not supported for Mach-O output` |
| `__attribute__((section))` on a function | `a function's section attribute is not supported for Mach-O output` |

#### Windows (COFF)

`x86_64-windows-gnu` produces AMD64 COFF objects. No program built this
way has been run on Windows, and EmbCC provides no C library for it.
Every compile prints a warning, which is on by default and can be
silenced with `-Wno-windows-abi`:

```text
embcc: f.c: warning: x86_64-windows-gnu is not yet the Microsoft x64 ABI: `long` is 8 bytes and wchar_t 4 (Windows has 4 and 2), and rsi, rdi, xmm6 and xmm7 are not preserved across a call: objects EmbCC compiles agree with each other and with nothing else [-Wwindows-abi]
```

What EmbCC does implement of the Microsoft x64 convention:

- The first four arguments travel by position: slot *n* uses the *n*th
  of `rcx`, `rdx`, `r8`, `r9` for an integer and `xmm0`–`xmm3` for a
  floating-point value. The caller reserves 32 bytes of shadow space.
- A structure of exactly 1, 2, 4 or 8 bytes is passed in its slot; any
  other structure is passed by reference to a copy the caller makes.
- A variadic `double` among the first four arguments is also copied into
  the integer register of its slot. `va_list` is a `char *` walking
  eight-byte slots; a variadic function stores `rcx`–`r9` into the home
  area, and `va_copy` is an assignment.

What is not Microsoft's, beyond the points the warning names, and is
refused:

| Construct | Diagnostic |
|---|---|
| `long double` in a function's signature | `long double in the signature of 'f' is not supported for a Windows target yet: there it travels by reference and returns through a hidden pointer, and EmbCC passes it on the stack by value` |
| `__int128` in a function's signature | `__int128 in the signature of 'f' is not supported for a Windows target yet: EmbCC lowers its arithmetic to libgcc helpers whose arguments it places in the System V registers` |
| `va_arg` of a structure | `va_arg of a struct is not supported for a Windows target yet` |
| `va_arg` of `long double` or `__int128` | `va_arg of long double is not supported for a Windows target yet: there it travels by reference` |
| `-g` | `-g is not supported for a Windows target yet: its debug information goes in CodeView records this does not write, and emitting DWARF under a COFF name would be worse than refusing` |
| `__thread` | `__thread is not supported for a Windows target yet: Windows reaches a thread-local through a _tls_index and a TLS directory this writer does not emit` |
| `__attribute__((constructor))`, `destructor` | `__attribute__((constructor)) is not supported for a Windows target yet: it needs the .ctors/.dtors sections this COFF writer does not emit` |
| file-scope `__asm__` with labels or symbols | `a file-scope asm block with labels or symbol references is not supported for a Windows target yet` |
| any C++ translation unit | `C++ exceptions are not supported for a Windows target yet: the unwind tables go in .pdata and .xdata and neither is written` |

C++ is refused even with `-fno-exceptions`, because C++ always gets
unwind tables.

### Predefined macros

The architecture macros are those of `x86_64-elf-gcc`: `__x86_64__`,
`__x86_64`, `__amd64__`, `__amd64`, `__LP64__`, `_LP64`, `__SSE__`,
`__SSE2__`, `__SSE_MATH__`, `__SSE2_MATH__`, `__MMX__`, `__FXSR__`,
`__SIZEOF_INT128__` (16), `__SIZEOF_LONG_DOUBLE__` (16),
`__BIGGEST_ALIGNMENT__` (16), `__code_model_small__`,
`__GCC_ASM_FLAG_OUTPUTS__`, and `__ELF__` on the ELF triples. The
operating system adds:

| Triple | Added | Removed |
|---|---|---|
| `x86_64-emblink` | `__emblink__`, `__emblink`, `__EmbLinkOS__` | |
| `x86_64-linux-gnu` | `__linux__`, `__linux`, `__gnu_linux__`, `__unix__`, `__unix` | |
| `x86_64-apple-darwin` | `__APPLE__`, `__MACH__`, `__unix__`, `__unix` | `__ELF__` |
| `x86_64-windows-gnu` | `_WIN32`, `_WIN64`, `__MINGW32__`, `__MINGW64__` | `__ELF__` |

`__GCC_ASM_FLAG_OUTPUTS__` is defined, but flag-output constraints such
as `"=@ccc"` are not implemented; see [Inline assembly](inline-asm.md).

### Runtime

The compiler calls helpers named as in libgcc for operations the machine
has no instruction for: `__int128` multiply, divide and shifts
(`__multi3`, `__divti3`, ...), complex multiply and divide (`__muldc3`,
...), and conversions between `__int128` and floating point.
`lib/rt` implements them; on Linux it is built as `librt.a` by `make
libc-linux-x86_64` and linked automatically. On `x86_64-elf`, link
`lib/rt` or a libgcc. See [Libraries](libraries.md).

### Limitations

- `embcc` without `-c` links the ELF x86-64 triples, and the ARM, RISC-V
  and AVR firmware targets given a memory map (`-T` or
  `-Wl,-Ttext`/`-Tdata`). For AArch64 ELF, macOS and Windows it stops with
  `embcc: error: cannot link for TRIPLE`; compile with `-c` and link
  separately. See [Linking](invoking.md#linking).
- `__thread` uses the local-exec model only (`R_X86_64_TPOFF32`), which
  is correct in a statically linked executable.
- A local aligned beyond 16 bytes, scalar or aggregate, is supported: its
  storage is carved from the stack at function entry and rounded up, and
  a scalar is read and written there.

## AArch64

### Triples

| Triple | Accepted aliases | Operating system | Object format |
|---|---|---|---|
| `aarch64-elf` | `aarch64`, `arm64`, `aarch64-none-elf` | none (bare metal, the EmbLinkOS kernel) | ELF64 |
| `aarch64-emblink` | | EmbLinkOS user programs | ELF64 |
| `aarch64-linux-gnu` | `aarch64-linux` | Linux, static executables | ELF64 |
| `aarch64-apple-darwin` | `arm64-apple-darwin`, `aarch64-darwin` | macOS | Mach-O |

The instruction set is ARMv8-A. `embld` does not link AArch64 objects;
use an AArch64 ELF linker, or the system linker on macOS.

### Options

#### `-mgeneral-regs-only`, `-mno-sse`, `-mno-sse2`

Never touch a floating-point or SIMD register, as a kernel must before
`CPACR_EL1.FPEN` enables them. A variadic prologue then saves no `q`
registers and block copies use general registers. Any floating-point
operation is an error:

```text
embcc: f.c:3: error: floating point used under -mgeneral-regs-only (in 'f')
```

### Calling convention: AAPCS64

`aarch64-elf`, `aarch64-emblink` and `aarch64-linux-gnu` follow the
Procedure Call Standard for the Arm 64-bit Architecture.

- Integer, pointer and small composite arguments go in `x0`–`x7`;
  `float`, `double` and `long double` in `v0`–`v7`. A homogeneous
  floating-point aggregate of up to four members goes in consecutive `v`
  registers. Once a register file is exhausted, later arguments of that
  kind go on the stack; there is no back-filling.
- A composite larger than 16 bytes is passed by reference to a copy the
  caller makes. A composite whose natural alignment is 16 bytes, and an
  `__int128`, start at an even-numbered `x` register. The natural
  alignment is the largest of the members' alignments: an `aligned`
  attribute on a member counts, packing lowers it, and an `aligned`
  attribute on the structure itself does not.
- Stack arguments occupy eight-byte slots, sixteen-byte aligned for
  sixteen-byte-aligned types.
- An indirect result is written through the address passed in `x8`.
- `va_list` is the 32-byte AAPCS64 record; a variadic prologue saves
  `x0`–`x7` and `q0`–`q7`. `va_arg` of a structure follows the same
  rules as argument passing.
- The stack is 16-byte aligned. An unnamed bit-field affects a
  structure's alignment, as AAPCS64 requires.
- `long double` is IEEE binary128 with no hardware support: arithmetic
  calls `__addtf3`, `__multf3` and the rest of that family.

### Apple arm64

`aarch64-apple-darwin` produces Mach-O objects and follows Apple's arm64
convention, which departs from AAPCS64 in these points:

- Plain `char` is signed, `wchar_t` is `int`, and `long double` is the
  same 8-byte format as `double`. The predefined macros follow
  (`__CHAR_UNSIGNED__` is not defined, `__WCHAR_TYPE__` is `int`,
  `__SIZEOF_LONG_DOUBLE__` is 8).
- A named argument passed on the stack takes its own size and
  alignment rather than an eight-byte slot. Homogeneous aggregates keep
  their element alignment; other composites take whole doublewords, and
  there an `aligned(16)` on the structure does count.
- Nothing is rounded to an even register: an `__int128`, or a structure
  holding one, takes the next free pair.
- Every variadic argument goes on the stack, in eight-byte slots.
  `va_list` is a `char *` and `va_copy` is an assignment. `va_arg` of a
  structure reads it from whole doublewords aligned as its type is.
- An unnamed bit-field does not affect a structure's alignment, as on
  x86-64: `struct { char a; int :0; char b; }` is 5 bytes with alignment
  1, where AAPCS64 makes it 8 bytes with alignment 4.
- The address of a symbol not defined in the translation unit is loaded
  from the GOT.

The refusals listed under [macOS](#macos-mach-o) apply.

### Predefined macros

The architecture macros are those of `aarch64-elf-gcc`, among them
`__aarch64__`, `__AARCH64EL__`, `__ARM_64BIT_STATE`, `__ARM_ARCH` (8),
`__ARM_ARCH_8A`, `__ARM_ARCH_PROFILE` (65, `'A'`), `__ARM_PCS_AAPCS64`,
`__ARM_NEON`, `__ARM_FP` (14), `__ARM_FEATURE_FMA`,
`__ARM_FEATURE_IDIV`, `__ARM_FEATURE_UNALIGNED`, `__CHAR_UNSIGNED__`
(not on Darwin), `__SIZEOF_INT128__` (16) and
`__AARCH64_CMODEL_SMALL__`. The operating-system macros are those of the
x86-64 triple with the same OS.

### Runtime

As for x86-64, plus the binary128 `long double` routines (`softtf.c`).
`make libc-linux-aarch64` builds `librt.a` for `aarch64-linux-gnu`.

### Limitations

- Inline assembly has its own vocabulary; see
  [Inline assembly](inline-asm.md#aarch64).
- `__thread` uses the local-exec model only.
- A scalar local aligned beyond 16 bytes is refused, as on x86-64; an
  array or structure local so aligned is supported.

## ARM Cortex-M

The ARMv6-M profile in Thumb-1, ARMv8-M Baseline in Thumb-1 with the
32-bit instructions Baseline adds, and the ARMv7-M, ARMv7E-M and ARMv8-M
Mainline profiles in Thumb-2. Both ARMv8-M profiles support the Secure
side of TrustZone-M ([`-mcmse`](#trustzone-m-cmse)). Every Cortex-M
target is freestanding.

### Triples

| Triple | Accepted aliases | Architecture | Float ABI | Cores |
|---|---|---|---|---|
| `thumbv6m-none-eabi` | `thumbv6m`, `armv6m-none-eabi` | ARMv6-M | soft | Cortex-M0, M0+, M1 |
| `thumbv8m.base-none-eabi` | `thumbv8m.base`, `armv8m.base-none-eabi` | ARMv8-M Baseline | soft | Cortex-M23 |
| `thumbv7m-none-eabi` | `thumbv7m`, `armv7m-none-eabi`, `arm-none-eabi` | ARMv7-M | soft | Cortex-M3 |
| `thumbv7em-none-eabi` | `thumbv7em`, `armv7em-none-eabi` | ARMv7E-M | soft | Cortex-M4, M7 |
| `thumbv7em-none-eabihf` | | ARMv7E-M | hard, FPv4-SP-D16; FPv5-D16 with `-mcpu=cortex-m7` | Cortex-M4F; Cortex-M7 |
| `thumbv8m.main-none-eabi` | `thumbv8m.main`, `thumbv8m-none-eabi`, `armv8m.main-none-eabi` | ARMv8-M Mainline | soft | Cortex-M33 |
| `thumbv8m.main-none-eabihf` | | ARMv8-M Mainline | hard, FPv5-SP-D16 | Cortex-M33 with FPU |

`arm-none-eabi` means ARMv7-M; there is no target for the A- or R-profile
or for the ARM instruction set.

The architecture changes the object's build attributes, the predefined
macros and `-dumpmachine`. The instructions generated for ARMv7-M and
ARMv7E-M are the same: the code generator does not use the DSP extension.
Where the part has it -- ARMv7E-M, and ARMv8-M Mainline with
`-mcpu=cortex-m33` or `-march=armv8-m.main+dsp` -- its instructions are
accepted in inline asm and `.s` files and its macros are defined, which is
what CMSIS's `cmsis_gcc.h` and CMSIS-DSP select their SIMD code on
([Inline assembly](inline-asm.md#the-dsp-extension), `<arm_acle.h>`).
`thumbv8m.main-none-eabi` alone has no DSP extension, as with clang.

### Options

#### `-mthumb`

Accepted; no effect. Thumb is the only instruction set.

#### `-marm`

Refused: `-marm is not supported: a Cortex-M has no ARM instruction set,
only Thumb`.

#### `-mcpu=CPU`

Select the architecture variant by core. `cortex-m0`, `cortex-m0plus`
and `cortex-m1` select ARMv6-M on any ARM triple, as
`thumbv6m-none-eabi` does, and `cortex-m23` selects ARMv8-M Baseline, as
`thumbv8m.base-none-eabi` does. `cortex-m3` selects ARMv7-M; `cortex-m4`,
`cortex-m7` and `cortex-m33` select ARMv7E-M. Otherwise the option does
not change the architecture level: `--target=thumbv7m-none-eabi
-mcpu=cortex-m33` is `thumbv7em-none-eabi`, and ARMv8-M is selected only
by a `thumbv8m.main` triple. On a `thumbv6m` triple, an ARMv7-M or
ARMv8-M part raises the level. Any other value is an error:

```text
embcc: error: -mcpu=cortex-m55 is not a part EmbCC knows: it emits ARMv6-M (cortex-m0, m0plus, m1), ARMv8-M Baseline (cortex-m23), ARMv7-M and ARMv7E-M (cortex-m3, m4, m7) and ARMv8-M Mainline (cortex-m33)
```

#### `-mfpu=FPU`

Name the floating-point unit. EmbCC knows three: `fpv4-sp-d16`, the
Cortex-M4F's unit, which requires ARMv7E-M (a `thumbv7em` triple, or
`-mcpu=cortex-m4`, `m7` or `m33`); `fpv5-d16`, the Cortex-M7's
double-precision unit, which requires ARMv7E-M and, when `-mcpu=` is
given, `-mcpu=cortex-m7`; and `fpv5-sp-d16`, the Cortex-M33's, which
requires a `thumbv8m.main` triple. `none`, `soft` and `auto` name no
unit. The FPU only takes effect with `-mfloat-abi=softfp` or `hard`.
`-mcpu=cortex-m7` with `thumbv7em-none-eabihf` and no `-mfpu=` means
`fpv5-d16`.

| Mistake | Diagnostic |
|---|---|
| any other unit | `-mfpu=fpv5-sp-d16 is not supported on thumbv7em-none-eabi: EmbCC emits VFP for the Cortex-M4F's unit (-mfpu=fpv4-sp-d16) and the Cortex-M7's (-mfpu=fpv5-d16) and nothing else: another unit's instruction set and attributes are unchecked here` |
| `fpv5-d16` on ARMv8-M | `-mfpu=fpv5-d16 is not supported on thumbv8m.main-none-eabi: EmbCC emits VFP for the Cortex-M33's unit (-mfpu=fpv5-sp-d16) and nothing else: another unit's instruction set and attributes are unchecked here` |
| `fpv4-sp-d16` or `fpv5-d16` on ARMv7-M | `-mfpu=fpv4-sp-d16 is an ARMv7E-M unit, and the part is ARMv7-M (a Cortex-M3 has no FPU); add -mcpu=cortex-m4` |
| `fpv5-d16` on another part | `-mfpu=fpv5-d16 is the Cortex-M7's double-precision unit, and -mcpu=cortex-m4 does not have it; the Cortex-M4F's is -mfpu=fpv4-sp-d16` |
| any unit, or `softfp` or `hard`, on ARMv6-M | `-mfpu=fpv5-d16 on thumbv6m-none-eabi: an ARMv6-M core (Cortex-M0, M0+, M1) has no FPU, so floating point is soft and travels in the core registers` |
| the same on ARMv8-M Baseline | `-mfpu=fpv5-sp-d16 on thumbv8m.base-none-eabi: an ARMv8-M Baseline core (Cortex-M23) has no FPU, so floating point is soft and travels in the core registers` |

#### `-mfloat-abi=ABI`

`soft`, `softfp` or `hard`. The default is `soft`, or `hard` for an
`-eabihf` triple, which also implies that architecture's FPU.
`-mfpu=` and `-mfloat-abi=` are combined after all options are read, so
their order does not matter, and an explicit value overrides what an
`-eabihf` triple implies.

| `-mfloat-abi=` | Arithmetic | Floating-point arguments and results | `-dumpmachine` |
|---|---|---|---|
| `soft` | Library calls, even if an FPU is named | Core registers | `-eabi` |
| `softfp` | `float` on the FPU; `double` on the FPU with `fpv5-d16`, else by library call | Core registers; links with `soft` objects | `-eabi` |
| `hard` | `float` on the FPU; `double` on the FPU with `fpv5-d16`, else by library call | VFP registers (AAPCS-VFP) | `-eabihf` |

`fpv4-sp-d16` and `fpv5-sp-d16` are single-precision, so with them
`double` arithmetic is a call to the soft-float helpers under every float
ABI. `fpv5-d16` computes in double precision as well:

- `double` `+`, `-`, `*`, `/`, negation, `fabs`, `__builtin_sqrt`, the
  comparisons, and the conversions to and from `float`, `int` and
  `unsigned` are `vadd.f64`, `vsub.f64`, `vmul.f64`, `vdiv.f64`,
  `vneg.f64`, `vabs.f64`, `vsqrt.f64`, `vcmp.f64`/`vcmpe.f64` and
  `vcvt`. libc's `sqrt` is `vsqrt.f64` too.
- The conversions between `double` or `float` and `long long` or
  `unsigned long long` stay calls (`__fixdfdi`, `__fixunsdfdi`,
  `__floatdidf`, `__floatundidf` and the four `float` ones): VFP has no
  instruction for a 64-bit integer. Built for this unit,
  `lib/rt/softfp.c` contains those eight routines only.
- With the register allocator on (`-O1` and up), a double gets one of
  `d8`-`d15`, the callee-saved half of the file, which a function saves
  with `vpush {d8-...}`. `d0` and `d1` are scratch, and `d0`-`d7` carry
  the hard-float arguments and results. A double the allocator cannot
  place stays in an eight-byte stack slot.
- `__ARM_FP` is `0xc` and `__ARM_FPV5__` is defined. clang says `0xe`
  and defines `__ARM_FEATURE_FMA`; EmbCC emits neither half-precision
  conversions nor fused multiply-add, so it promises neither.
- The object's attributes are clang's for the unit: `Tag_FP_arch` is
  FPv5-D16 (`ARMv8-a FP-D16`), and `Tag_ABI_HardFP_use` is left at its
  default, both precisions.

| Mistake | Diagnostic |
|---|---|
| an unknown value | `-mfloat-abi=foo is not an ARM float ABI: it is one of soft, softfp and hard` |
| `softfp` or `hard` with no FPU | `-mfloat-abi=hard needs an FPU to use: add -mfpu=fpv4-sp-d16 (Cortex-M4F), -mfpu=fpv5-d16 (Cortex-M7) or -mfpu=fpv5-sp-d16 (Cortex-M33)` |
| any ARM option on another target | `-mcpu=cortex-m3 is an ARM option, and the target is riscv32-unknown-elf` |

#### `-mcmse`

Compile for the Secure state of an ARMv8-M part with the security
extension: see [TrustZone-M](#trustzone-m-cmse).

### ARMv8-M Baseline

`thumbv8m.base-none-eabi` (or `-mcpu=cortex-m23`) is the ARMv6-M code
generator with what ARMv8-M Baseline adds where it replaces a call:

- a 32-bit `/` is `sdiv` or `udiv`, and `%` is the quotient multiplied
  back and subtracted (`muls`, `subs`; Baseline has no `mls`), as clang
  does it. `__aeabi_idiv` and its family are not called. A 64-bit
  division is still `__divdi3` and the others.
- a 1-, 2- or 4-byte atomic read-modify-write or compare-and-swap is a
  `ldrex`/`strex` (`b`, `h`) loop between two `dmb`s, as on ARMv7-M. The
  `__GCC_ATOMIC_*_LOCK_FREE` values are 2. Baseline has no `ldrexd`, so
  an 8-byte atomic is a call to `__atomic_*_8` (`lib/rt`), as on every
  32-bit target.

Everything else is ARMv6-M's: Thumb-1 data processing on r0-r7, literal
pools for constants and addresses (clang keeps them for this core too),
no IT block, and no unaligned access -- the Cortex-M23 faults on one, as
the M0 does. A function is scanned when it is finished, and any 32-bit
encoding Baseline lacks -- or an IT block -- is refused by name (an
inline asm template's included):

```text
embcc: f.c:3: error: the ARMv8-M Baseline backend cannot lower an instruction ARMv8-M Baseline does not have (a 32-bit Thumb-2 encoding, from inline asm or the backend) yet (function f)
```

The 32-bit instructions Baseline has are `bl`, `b.w`, `mrs`, `msr`,
`dmb`, `dsb`, `isb`, `sdiv`, `udiv`, `movw`, `movt`, `ldrex`, `strex`
(with an offset), `ldrexb`, `ldrexh`, `strexb`, `strexh`, `clrex`, the
load-acquire and store-release family (`lda`, `ldab`, `ldah`, `ldaex`,
`ldaexb`, `ldaexh`, `stl`, `stlb`, `stlh`, `stlex`, `stlexb`,
`stlexh`), `tt`, `ttt`, `tta`, `ttat` and `sg`; it adds `cbz`, `cbnz`,
`bxns` and `blxns` to the 16-bit set. The inline and file assemblers
accept exactly these at this level
([Inline assembly](inline-asm.md#armv8-m-security-and-acquirerelease-instructions)).
`tests/golden/thumbv8mbase-encoding.sh` checks every one against llvm-mc
and the refusals line for line, and `tests/golden/thumbv8mbase-exec.sh`
runs the exec corpus at `-O0` to `-Os`, with `lib/libc` and `lib/rt`
built for the triple, and scans every object it builds.

QEMU has no Cortex-M23, so that suite runs Baseline code on the
mps2-an505's Cortex-M33 (`tests/harness/thumb-m23`), which executes every
Baseline instruction, with CCR.UNALIGN_TRP set so that an unaligned
access faults as on the M23. That the code contains nothing else is the
scan's to show.

The object's build attributes are clang's: `Tag_CPU_arch` 16 (v8-M
Baseline), `Tag_THUMB_ISA_use` 3, `Tag_CPU_unaligned_access` 0.

### TrustZone-M (CMSE)

`-mcmse` compiles for the Secure state of an ARMv8-M part with the
security extension (ACLE's CMSE), on `thumbv8m.main-none-eabi` and
`thumbv8m.base-none-eabi`. `__ARM_FEATURE_CMSE` is 3 with it, and 1 (the
`tt` instruction) on every ARMv8-M target without it.

`__attribute__((cmse_nonsecure_entry))` makes a function the Non-secure
state may call:

- its object has a second global symbol at the same address,
  `__acle_se_NAME`. `embld` sees the pair and makes the secure gateway
  veneer, `sg; b.w __acle_se_NAME`, in `.gnu.sgstubs`, and `NAME` names
  the veneer ([embld](tools/embld.md#armv8-m-secure-gateway-veneers)).
- it returns with `bxns lr`, and before that overwrites `r0`-`r3` (those
  the result does not occupy) and `r12` with `lr`, and the flags with
  `msr apsr_nzcvq, lr` (`apsr_nzcvqg` on Mainline, whose DSP extension
  has the GE bits; `apsr` on Baseline). `r4`-`r11` hold the caller's
  values again after the epilogue. This is clang's soft-float sequence.
- it is kept and not inlined, has no tail calls, and must have external
  linkage.

A call through a pointer to a function type with
`__attribute__((cmse_nonsecure_call))` enters the Non-secure state. The
attribute is written among the specifiers, before the declarator:

```c
#include <arm_cmse.h>
typedef int __attribute__((cmse_nonsecure_call)) ns_fn(int, int);

int call_ns(void *p)
{
    ns_fn *f = cmse_nsfptr_create((ns_fn *)p);
    return f(1, 2);
}
```

The call saves `r4`-`r11`, clears bit 0 of the target, overwrites every
register that holds no argument with it, and the flags, branches with
`blxns`, and restores `r4`-`r11`; on Mainline it also saves and clears
the floating-point context with `vlstm`/`vlldm`, as clang does. A
narrow result is extended again after the call, since the Non-secure
callee is not trusted to have done it. `tests/golden/thumbv8m-cmse.sh`
compares these sequences with clang's and runs a Secure and a
Non-secure image on the mps2-an505, reading the registers on the other
side of every crossing.

`<arm_cmse.h>` provides `cmse_address_info_t`, `cmse_TT`, `cmse_TTT`,
`cmse_TTA`, `cmse_TTAT` and their `_fptr` forms,
`cmse_check_address_range`, `cmse_check_pointed_object`,
`cmse_nsfptr_create` and `cmse_is_nsfptr`, as ACLE defines them;
`tt`/`ttt`/`tta`/`ttat` are inline assembly there.

Without `-mcmse`, both attributes are ignored with a warning, as clang
and GCC do: `__attribute__((cmse_nonsecure_entry)) is ignored without
-mcmse (the Secure side of an ARMv8-M build)`.

| Construct | Diagnostic |
|---|---|
| `-mcmse` below ARMv8-M | `-mcmse is the Secure side of ARMv8-M's security extension, and thumbv7m-none-eabi is not ARMv8-M: use thumbv8m.main-none-eabi or thumbv8m.base-none-eabi` |
| `-mcmse` with an FPU | `-mcmse with an FPU (-mfpu=, -mfloat-abi=softfp or hard, or an -eabihf triple) is not supported: EmbCC does not clear the floating-point registers a cmse_nonsecure_entry function must clear; build the Secure side with -mfloat-abi=soft` |
| an entry function with stack arguments | `cmse_nonsecure_entry function 'f' requires arguments on the stack, which is the Non-secure caller's (CMSE allows r0-r3 only)` |
| an entry function returning through memory | `cmse_nonsecure_entry function 'f' would return its value through memory the Non-secure caller owns (CMSE allows a result in r0-r3 only)` |
| a static entry function | `cmse_nonsecure_entry function 'f' has internal linkage: the Non-secure state enters it through a veneer the linker makes from its global symbol` |
| a variadic entry function | `cmse_nonsecure_entry function 'f' is variadic, and its unnamed arguments would be on the Non-secure stack` |
| a Non-secure call with stack arguments or a result through memory | `a call through a cmse_nonsecure_call pointer in 'g' passes arguments on the stack, which is not supported: CMSE passes r0-r3 only` |
| `cmse_nonsecure_call` with no function type to attach to | `cmse_nonsecure_call applies to a function type, written among the specifiers before its declarator: ...` |
| `cmse_nonsecure_caller()` | an undeclared `__cmse_nonsecure_caller_is_not_supported_by_EmbCC` (it needs `lr` as the function was entered) |

The Secure image is linked with `embld`, normally with a linker script
that puts `.gnu.sgstubs` in a region the SAU (and on parts like the
mps2-an505, the IDAU) marks Non-secure Callable; `--cmse-implib
--out-implib=FILE` writes the import library the Non-secure image links
against. `tests/golden/thumbv8m-cmse/` holds a complete example: the
Secure boot that programs the SAU, NSCCFG and the memory protection
controller and enters the Non-secure reset handler, both images, and the
script.

### Calling convention: AAPCS32

The base standard applies with `-mfloat-abi=soft` and `softfp`:

- Arguments fill `r0`–`r3`, then the stack. An argument with 8-byte
  alignment (`long long`, `double`, or a composite whose natural
  alignment is 8) starts at an even-numbered register and at an
  8-byte-aligned stack offset, so it is never split.
- A composite may be split between the last core registers and the
  stack, but only while nothing has been placed on the stack yet. After a
  split, nothing back-fills a register.
- A scalar result comes back in `r0`, or `r0:r1` for 64-bit values. A
  composite of 4 bytes or fewer comes back in `r0`; a larger one is
  written through a hidden pointer passed in `r0`, and the real arguments
  start at `r1`.
- A variadic argument is passed as a named one would be. `va_list` is a
  `void *`; a variadic function stores `r0`–`r3` immediately below the
  caller's stack arguments, so one pointer walks both. `va_copy` is an
  assignment, and `va_arg` of a structure reads it from that walk where
  the caller placed it.
- The stack is 8-byte aligned at every public interface. An unnamed
  bit-field affects a structure's alignment.

With `-mfloat-abi=hard` (AAPCS-VFP), floating-point arguments travel in
the VFP registers instead:

- A `float`, a `double`, or a homogeneous aggregate of one to four
  `float`s or `double`s (including `_Complex float` and
  `_Complex double`) is a candidate. Candidates fill `s0`–`s15` /
  `d0`–`d7` lowest first, with back-filling: `f(float, double, float)`
  passes `s0`, `d1` and `s1`.
- A candidate that does not fit goes on the stack, and every VFP register
  still free becomes unusable for the rest of the call. Other arguments
  use `r0`–`r3` and the stack exactly as in the base standard.
- A floating-point result comes back in `s0` or `d0`; a homogeneous
  aggregate in `s0`–`s3` or `d0`–`d3`, whatever its size.
- A variadic function uses the base standard for all of its arguments and
  its result. The runtime helpers keep the base convention too.

`__attribute__((pcs("aapcs")))` and `__attribute__((pcs("aapcs-vfp")))`
on a function declaration select the convention for calls to that
function:

| Mistake | Diagnostic |
|---|---|
| on a non-ARM target | `__attribute__((pcs)) names an ARM calling convention, and this is not an ARM target` |
| another value | `pcs wants "aapcs" or "aapcs-vfp"` |
| `aapcs-vfp` without an FPU | `pcs("aapcs-vfp") passes floating point in VFP registers, and this part has no FPU: add -mfpu=` |
| on something other than a function | `pcs is only supported on a function declaration` |
| two different values | `two different pcs attributes on one declaration` |

### Object format

ELF32, `EM_ARM`, `EF_ARM_EABI_VER5`, with `RELA` relocations. Calls use
`R_ARM_THM_CALL`, tail calls `R_ARM_THM_JUMP24`, and addresses are built
with `movw`/`movt` (`R_ARM_THM_MOVW_ABS_NC`, `R_ARM_THM_MOVT_ABS`), so
code is position-dependent. Each object carries an `.ARM.attributes`
section stating the architecture (`Tag_CPU_arch`), the profile, the FPU
(`Tag_FP_arch`) and the float ABI (`Tag_ABI_HardFP_use`,
`Tag_ABI_VFP_args`), so a linker that checks attributes refuses to mix
hard- and soft-float objects.

### Predefined macros

The tables are those of `clang -target thumbv7m-none-eabi` and
`clang -target thumbv8m.main-none-eabi`, adjusted for the FPU:

| Macro | ARMv7-M, ARMv7E-M | ARMv8-M Mainline |
|---|---|---|
| `__arm__`, `__thumb__`, `__thumb2__`, `__ARM_EABI__`, `__ARMEL__` | 1 | 1 |
| `__ARM_ARCH` | 7 | 8 |
| `__ARM_ARCH_7M__` / `__ARM_ARCH_7EM__` / `__ARM_ARCH_8M_MAIN__` | `__ARM_ARCH_7M__`; `__ARM_ARCH_7EM__` on ARMv7E-M | `__ARM_ARCH_8M_MAIN__` |
| `__ARM_FEATURE_DSP`, `__ARM_FEATURE_SIMD32` | ARMv7E-M only | with the DSP extension (`-mcpu=cortex-m33`, `+dsp`) |
| `__ARM_FEATURE_SAT`, `__ARM_FEATURE_QBIT` | yes | yes |
| `__ARM_ARCH_PROFILE` | `'M'` | `'M'` |
| `__ARM_FEATURE_IDIV`, `__ARM_FEATURE_CLZ`, `__ARM_FEATURE_LDREX` (0x7) | yes | yes |
| `__CHAR_UNSIGNED__`, `__WCHAR_UNSIGNED__` | 1 | 1 |
| `__BIGGEST_ALIGNMENT__` | 8 | 8 |

ARMv7E-M (`thumbv7em-*`, `-mcpu=cortex-m4` or `cortex-m7`,
`-march=armv7e-m`) defines `__ARM_ARCH_7EM__` in place of
`__ARM_ARCH_7M__`, and `__ARM_FEATURE_DSP` and `__ARM_FEATURE_SIMD32`, as
clang does; ARMv8-M Mainline adds the last two with the DSP extension.
`-mcpu=` names the part, and the part has one architecture whatever the
triple said: `-mcpu=cortex-m33` on a `thumbv7em` triple is ARMv8-M
Mainline, `-mcpu=cortex-m4` on `thumbv8m.main` ARMv7E-M.
`tests/golden/predef.sh` compares every `__ARM_ARCH*` and
`__ARM_FEATURE_*` macro with clang's for each part and `-march=`. clang's
`__ARM_FEATURE_FMA` is left out (below).

ARMv8-M Baseline's is that of `clang -target thumbv8m.base-none-eabi
-mcpu=cortex-m23`: `__ARM_ARCH` 8, `__ARM_ARCH_8M_BASE__`,
`__ARM_ARCH_ISA_THUMB` 1, `__ARM_FEATURE_IDIV`, `__ARM_FEATURE_LDREX`
(0x7) and `__ARM_FEATURE_CMSE` (1), lock-free values of 2, and no
`__thumb2__`. clang also defines `__ARM_FEATURE_CLZ`, `__ARM_FEATURE_SAT`,
`__ARM_FEATURE_QBIT`, `__ARM_FEATURE_NUMERIC_MAXMIN` and
`__ARM_FEATURE_DIRECTED_ROUNDING` there, which name instructions a
Cortex-M23 does not have (GCC defines none of them for
`armv8-m.base`); EmbCC leaves them out (`tools/gen-predef.sh`).
`__ARM_FEATURE_CMSE` is 1 on both ARMv8-M tables and 3 with `-mcmse`.

ARMv6-M's table is that of `clang -target thumbv6m-none-eabi`:
`__ARM_ARCH` is 6, `__ARM_ARCH_6M__` and `__ARM_ARCH_ISA_THUMB` 1 are
defined, and `__thumb2__`, `__ARM_FEATURE_IDIV`, `__ARM_FEATURE_CLZ`,
`__ARM_FEATURE_LDREX`, `__ARM_FEATURE_UNALIGNED` and
`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_N` are not. The `__GCC_ATOMIC_*_LOCK_FREE`
values are 1: an atomic read-modify-write is a call (see Runtime).

The float ABI changes these:

| Macro | `soft` | `softfp` | `hard` |
|---|---|---|---|
| `__SOFTFP__` | 1 | — | — |
| `__ARM_PCS` | 1 | 1 | — |
| `__ARM_PCS_VFP` | — | — | 1 |
| `__ARM_FP` | — | 0x4 | 0x4 |
| `__ARM_VFPV2__`, `__ARM_VFPV3__`, `__ARM_VFPV4__` | — | 1 | 1 |
| `__ARM_FPV5__` (ARMv8-M only) | — | 1 | 1 |

`__ARM_FP` is 0x4 (single precision only), and `__ARM_FEATURE_FMA` is
not defined, because EmbCC emits neither half-precision conversions nor
fused multiply-add.

### Runtime

Every floating-point operation the hardware lacks, every 64-bit
division (`__divdi3`, `__udivdi3`, `__moddi3`, `__umoddi3`) and complex
multiply and divide are calls to helpers with libgcc's names.
`make rt-embedded` builds `librt.a` for each of the six Cortex-M
triples (one archive per float ABI, because hard- and soft-float objects
do not link together); link it after your objects. On ARMv7-M and
ARMv8-M EmbCC does not provide the ARM run-time ABI's `__aeabi_*`
routines, so an object from another compiler that calls them must bring
its own.

ARMv6-M has no divide, no 64-bit multiply and no exclusive loads and
stores, so more operations are calls there. `librt.a` for
`thumbv6m-none-eabi` (`lib/rt/armv6m.c`) provides them under the names
clang and GCC use for the triple, all weak, so a program's own
definition wins:

| Operation | Routine |
|---|---|
| 32-bit `/`; `%` and a paired `/` | `__aeabi_idiv`, `__aeabi_uidiv`; `__aeabi_idivmod`, `__aeabi_uidivmod` |
| 64-bit `*`; shift by a variable | `__aeabi_lmul`; `__aeabi_llsl`, `__aeabi_llsr`, `__aeabi_lasr` |
| block copy or clear of more than 8 bytes | `__aeabi_memcpy`, `__aeabi_memclr` |
| atomic read-modify-write, compare-and-swap (1, 2, 4 bytes) | `__atomic_exchange_N`, `__atomic_fetch_OP_N`, `__atomic_compare_exchange_N`, `__sync_val_compare_and_swap_N` |

ARMv8-M Baseline uses the same archive for the 64-bit multiply, the
shifts and the block routines; its 32-bit divides and its atomics are
instructions, so neither the division routines nor the atomic ones are
called (`lib/rt/armv6m.c` builds no atomics for it).

The atomic routines mask interrupts with PRIMASK around the access. That
is atomic on a single core running privileged code; CPSID is ignored in
unprivileged Thread mode, so an RTOS whose tasks run unprivileged, or a
part with another bus master, defines its own.

### Limitations

| Construct | Diagnostic |
|---|---|
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| on ARMv6-M, an inline asm template that uses a Thumb-2 instruction | `the ARMv6-M backend cannot lower an instruction ARMv6-M does not have (a 32-bit Thumb-2 encoding, from inline asm or the backend) yet (function f)` |
| C++ with exceptions (on by default), or unwind tables (`-funwind-tables`, `-fasynchronous-unwind-tables`) | `C++ exceptions are not supported for thumbv7m-none-eabi yet: EmbCC writes no ARM EHABI unwind tables (.ARM.exidx); compile with -fno-exceptions`, and `unwind tables are not supported for thumbv7m-none-eabi yet (...)`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |

An array or structure local aligned beyond 8 bytes is supported: its
storage is carved from the stack at function entry and rounded up.

## ARMv7-A

ARMv7-A in ARM state: the A32 instruction set of a Cortex-A5, A7, A8, A9,
A12, A15 or A17, little-endian, with the base AAPCS (soft float) or, with
a VFP unit, AAPCS-VFP.
Freestanding only. It is the Cortex-M backend's instruction selection
writing A32 encodings; the design notes are in
[the ARMv7-A plan](../internals/arm-a32-plan.md).

### Triples

| Triple | Accepted aliases | ISA | ABI |
|---|---|---|---|
| `armv7a-none-eabi` | `armv7a`, `armv7-none-eabi`, `armv7a-unknown-none-eabi` | ARMv7-A, ARM state | AAPCS, soft float |
| `armv7a-none-eabihf` | `armv7a-unknown-none-eabihf` | ARMv7-A, ARM state, VFPv3-D16 | AAPCS-VFP |

### Options

| Option | Accepted values | Refused with |
|---|---|---|
| `-marm` | (no value) | `-mthumb is not supported on armv7a-none-eabi: EmbCC emits ARM (A32) code for a Cortex-A` |
| `-mcpu=CPU` | `cortex-a5`, `cortex-a7`, `cortex-a8`, `cortex-a9`, `cortex-a12`, `cortex-a15`, `cortex-a17`, `generic` | `-mcpu=cortex-r5 is not supported on armv7a-none-eabi` (a Cortex-M or Cortex-R core) |
| `-mfloat-abi=ABI` | `soft`, `softfp`, `hard` (the last two with an `-mfpu=`) | `-mfloat-abi=hard needs an FPU to use: add -mfpu=vfpv3-d16 (or vfpv3, vfpv4-d16, vfpv4)` |
| `-mfpu=FPU` | `vfpv3-d16`, `vfpv3`, `vfpv4-d16`, `vfpv4`, `none` | `-mfpu=neon is not supported on armv7a-none-eabi: EmbCC emits VFPv3 or VFPv4 ... and no NEON (Advanced SIMD) instruction` |
| `-mabi=ABI` | `aapcs`, `aapcs-linux` | as for Cortex-M |
| `-munaligned-access`, `-mthumb-interwork` | (no value) | `-mno-unaligned-access is not supported` |

No divide instruction is used, so the code runs on every ARMv7-A core:
`/` and `%` call `__aeabi_idiv`, `__aeabi_uidiv`, `__aeabi_idivmod` and
`__aeabi_uidivmod`, which `lib/rt` provides. `armv7a-none-eabihf` is
`-mfpu=vfpv3-d16 -mfloat-abi=hard`; with `softfp` the FPU computes and
floating point travels in the core registers, as with `soft`. Either way
the code is the Cortex-M7's single- and double-precision VFP on `d0`–`d15`
(`s16`–`s31`/`d8`–`d15` preserved); with `-mfloat-abi=soft` no VFP
instruction is emitted even with an `-mfpu=`, as GCC reads it. No NEON
instruction is ever emitted.

### Calling convention: AAPCS, soft float

The Cortex-M targets' base standard ([Calling convention:
AAPCS32](#calling-convention-aapcs32)), unchanged: `r0`–`r3` for
arguments and results, an 8-byte value in an even register pair or an
8-aligned stack slot, a composite larger than 4 bytes returned through a
hidden pointer in `r0`, `r4`–`r11` preserved, the stack 8-byte aligned at
every call, `char` unsigned and enums `int`-sized. `float` and `double`
travel as `int` and `long long` do. `tests/golden/arm-a32-abi.sh` checks
it with EmbCC and clang calling each other, in ARM state and across ARM
and Thumb.

### Code generation

Every instruction is one A32 word. Constants and addresses are
`movw`/`movt` (`R_ARM_MOVW_ABS_NC`, `R_ARM_MOVT_ABS`); calls are `bl`
(`R_ARM_CALL`) and tail calls `b` (`R_ARM_JUMP24`), reaching ±32 MiB. A
dense `switch` is `add pc, pc, rI, lsl #2` over a table of branches.
Comparisons that produce a value use conditional `mov`s. The code assumes
unaligned `ldr`/`str`/`ldrh`/`strh` work (`__ARM_FEATURE_UNALIGNED`, as
clang does): with the MMU on and the memory Normal, which is how an
A-profile system runs; with the MMU off every access is Strongly-ordered
and an unaligned one faults.

### Object format

ELF32, little-endian, `EM_ARM`, `e_flags` `EF_ARM_EABI_VER5`. Function
symbols are even (ARM state) and each function starts with an `$a`
mapping symbol. `.ARM.attributes` says `Tag_CPU_arch` v7, profile
`A`, the ARM ISA permitted and Thumb-2 permitted (as clang writes it), and
`Tag_ABI_VFP_args` base standard.

`embld` links A32 objects -- its own and clang's, REL or RELA -- and
interworks: a `bl` to a Thumb function becomes `blx`, as does a Thumb
`bl` to an ARM function. A jump (`b`) between the two instruction sets
needs a veneer and is refused. `-Tstack` writes an A32 stub that sets `sp`
and branches to the entry, since an A-profile core comes out of reset
with no stack.

### Predefined macros

clang's for `--target=armv7a-none-eabi -mfloat-abi=soft`: `__arm__`,
`__ARM_ARCH 7`, `__ARM_ARCH_7A__`, `__ARM_ARCH_PROFILE 'A'`,
`__ARM_ARCH_ISA_ARM`, `__ARM_EABI__`, `__SOFTFP__`, `__ARM_FEATURE_DSP`,
`__ARM_FEATURE_UNALIGNED`, `__ARM_FEATURE_LDREX 0xf`; no `__thumb__`, no
`__ARM_FEATURE_IDIV`, no `__ARM_FP`. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8`
is left out: an 8-byte atomic is a call to `__atomic_*_8` (`lib/rt`),
where clang inlines an `ldrexd`/`strexd` loop.

### Runtime

`make rt-embedded` and `make libc-embedded` build `lib/rt` and `lib/libc`
for `armv7a-none-eabi`. `lib/libc` has `setjmp`/`longjmp` for ARM state.
`tests/harness/arm-a32` boots QEMU's `virt` board (a Cortex-A15): the
MMU on with a flat map, the exception vectors, the PL011 UART and a
semihosting exit.

### Limitations

Refused by name: NEON (`-mfpu=neon`), the Cortex-M FPUs, Thumb state (`-mthumb`, `.thumb` and `.thumb_func`), `__builtin_frame_address` and
`__builtin_return_address` above level 0, `__attribute__((interrupt))` (an A-profile
handler returns with `subs pc, lr, #4`), a scalar local aligned past 8,
and C++ exceptions and unwind tables (C++ compiles with
`-fno-exceptions`). Inline assembly and `.s` files take the Cortex-M
vocabulary in ARM state -- the DSP extension's instructions (which every
ARMv7-A part has) included, with `<arm_acle.h>` -- with a condition on any
instruction, and add `mrs`/`msr` of `cpsr`, `mrc`/`mcr`, `ldrexd`/`strexd`
and the A32 ranges of `svc`, `bkpt` and `udf`; the M-profile special
registers, `cbz`, `tbb` and `tbh` are refused
([Inline assembly](inline-asm.md#arm-state-armv7-a)).

## RISC-V

### Triples

| Triple | Accepted aliases | ISA | ABI |
|---|---|---|---|
| `riscv32-unknown-elf` | `riscv32`, `riscv32-elf`, `rv32` | RV32IMAC | `ilp32` |
| `riscv64-unknown-elf` | `riscv64`, `riscv64-elf`, `rv64` | RV64IMAC | `lp64` |

Both are freestanding. One code generator serves both widths. The ISA
and ABI columns are the defaults; `-march=` and `-mabi=`
([invoking](invoking.md#risc-v-options)) select the F and D extensions
and their calling conventions.

### Extensions

| Extension | Use |
|---|---|
| I | The base integer instruction set. |
| M | Multiply and divide. At RV32, 64-bit division is a call (`__divdi3` and family). Required. |
| A | Atomic read-modify-write: `amoadd`, `amoor` and the other AMOs, and `lr`/`sc` loops for compare-and-swap, on 4-byte (and at RV64, 8-byte) objects; a 1- or 2-byte object is an AMO or an `lr.w`/`sc.w` loop on the aligned word around it. The memory order sets `.aq` and `.rl` as clang's does (seq_cst by default). Memory barriers are `fence rw, rw`. Required. |
| F | With `-march=...f...`: `float` add, subtract, multiply, divide, square root, comparisons and conversions to and from the integers a register holds are instructions (`fadd.s`, `feq.s`, `fcvt.w.s` ... ); a float lives in an f register (`ft3`-`ft11`, and `fs0`-`fs11` where the ABI preserves them). |
| D | With `-march=...d...` (or `g`): the same for `double`, and the conversions between the two. At RV32 a double crosses to an integer register pair through eight bytes of frame. |
| C | Compressed instructions, emitted wherever an encoding allows (unless `-march=` leaves out `c`). The object's `e_flags` has `EF_RISCV_RVC` set. |

Without F a floating-point operation is a call to a soft-float helper
(`__addsf3`, `__adddf3`, ...); with F alone so is every `double` one; and
the 64-bit integer conversions at RV32 and everything on `long double`
are calls with D too. No fused multiply-add is emitted: C rounds `a * b +
c` twice, and EmbCC does not contract. The objects carry a
`.riscv.attributes` section (`rv32i2p1_m2p0_a2p1_c2p0` or the RV64
equivalent, stack alignment 16), and `e_flags` the float ABI.

### Calling convention: the hardware-float ABIs

`-mabi=ilp32f`/`lp64f` and `ilp32d`/`lp64d` follow the psABI's hardware
floating-point convention, with ABI_FLEN 32 or 64, checked against clang
across the call (`tests/golden/riscv-hf-abi.sh`):

- a `float` (and with the `d` ABIs a `double`) goes in the next of
  `fa0`-`fa7`, and once those are gone by the integer rules below;
- a structure that flattens -- nested structures and arrays opened up --
  to one or two floating-point fields no wider than ABI_FLEN, or one such
  field and one integer no wider than XLEN in either order, goes field by
  field in `fa` and `a` registers when enough of both are left, and
  whole by the integer rules otherwise; a complex number counts as two
  fields. Unions, pointers, two integers, three fields and wider types
  never flatten. A zero-width bit-field is ignored beside a lone float
  and ends a two-field structure, as clang has it;
- variadic arguments always take the integer rules;
- results come back the same way in `fa0`/`fa1` and `a0` -- a structure
  of two doubles at RV32 too, rather than through a hidden pointer;
- `fs0`-`fs11` are callee-saved, as wide as the ABI (`fsw` under the
  `f` ABIs, `fsd` under the `d` ones).

The runtime helpers (`__extendsfdf2`, `__floatdisf`, `__trunctfdf2` ...)
take and return their floating-point values the same way.

### Calling convention: RISC-V psABI, soft float

- Arguments go in `a0`–`a7`, then on the stack. A scalar of up to XLEN
  bits takes one register; one of 2×XLEN bits (a `long long` or a
  `double` at RV32) takes two, in any two consecutive registers for a
  named argument and in an even-aligned pair for a variadic one. An
  argument may be split between `a7` and the stack.
- An aggregate of up to 2×XLEN bits is passed in up to two registers; a
  larger one is passed by reference to a copy.
- `float` and `double` are passed and returned in integer registers, as
  the `ilp32` and `lp64` soft-float ABIs require -- with an FPU too
  (`-march=rv32imafc -mabi=ilp32`), whose f registers are then all
  caller-saved.
- At RV64, a 32-bit integer in a register is kept sign-extended to 64
  bits, `unsigned int` included, as the psABI requires: arguments and
  results of 32-bit type are passed that way, in registers and on the
  stack, and EmbCC extends such a value wherever all 64 bits are read
  (a compare, a branch, a call).
- An argument wholly on the stack is aligned to the larger of its type's
  alignment and XLEN, but never more than 16 bytes.
- Results come back in `a0` and `a1`. A composite larger than 2×XLEN is
  written through a hidden pointer passed in `a0`.
- `va_list` is a `void *`; `va_copy` is an assignment. A variadic
  `va_arg` of a structure larger than two registers reads it by
  reference.
- The stack is 16-byte aligned. An unnamed bit-field does not affect a
  structure's alignment.

### Code model

`medany`: addresses are formed PC-relatively with `auipc` and an `addi`
or load (`R_RISCV_PCREL_HI20` / `R_RISCV_PCREL_LO12_I`), so code and data
may sit anywhere within ±2 GiB of each other. Calls use the `auipc`/`jalr`
pair with one `R_RISCV_CALL_PLT` relocation.

### Predefined macros

From `clang -target riscv32-unknown-elf` and `riscv64-unknown-elf`:
`__riscv`, `__riscv_xlen` (32 or 64), `__riscv_i`, `__riscv_m`,
`__riscv_a`, `__riscv_c`, `__riscv_mul`, `__riscv_div`,
`__riscv_muldiv`, `__riscv_atomic`, `__riscv_compressed`,
`__riscv_float_abi_soft`, `__riscv_cmodel_medany`, `__CHAR_UNSIGNED__`,
`_ILP32`/`__ILP32__` or `_LP64`/`__LP64__`. `__SIZEOF_INT128__` is
defined at RV64 only. `-march=` and `-mabi=` change them as clang's do:
`__riscv_f`, `__riscv_d`, `__riscv_flen`, `__riscv_fdiv`,
`__riscv_fsqrt`, `__riscv_zicsr`, `__riscv_zcf` (RV32), `__riscv_zcd`,
`__riscv_zifencei`, `__riscv_float_abi_single` or `_double` in place of
`_soft`, and no compressed macros without C (`tests/golden/predef.sh`
checks sixteen combinations).

### Runtime

`make rt-embedded` builds `librt.a` for `riscv32-unknown-elf`, holding
the soft-float, 64-bit division and complex helpers. There is no
archive for `riscv64-unknown-elf`, because `lib/rt/int128.c` and
`lib/rt/fp128.c` do not compile for it; compile the other `lib/rt` files
individually.

### Limitations

| Construct | Diagnostic (RV32 shown; RV64 names itself) |
|---|---|
| `__int128` at RV32 | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| C++ with exceptions (on by default), or unwind tables (`-funwind-tables`, `-fasynchronous-unwind-tables`) | RV32: `C++ exceptions are not supported for riscv32-unknown-elf yet: EmbCC writes no RISC-V .eh_frame; compile with -fno-exceptions`. RV64: `unwind tables are not supported for riscv64-unknown-elf yet (-funwind-tables, -fasynchronous-unwind-tables, -fexceptions): EmbCC writes no RISC-V .eh_frame`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |
| `__attribute__((interrupt("user")))` | `__attribute__((interrupt("user"))) is not supported: user-mode interrupts (the N extension and its uret) were never ratified and are gone from the privileged spec, and GCC and clang no longer accept them` |
| an interrupt handler with parameters, or with a result | `interrupt handler 'h' takes parameters: ...`, `interrupt handler 'h' returns a value: ...` |
| `__attribute__((interrupt))` in C++ | `__attribute__((interrupt)) is not supported in C++ yet: ...` (write the handler in C) |

An array or structure local aligned beyond 16 bytes is supported: its
storage is carved from the stack at function entry and rounded up.

## Renesas RX

RXv1 -- the RX600/RX610 cores, and RX100/RX200 -- little-endian, in GCC's
rx-elf default configuration without the FPU: `double` and `long double`
are binary32 (GCC's `-m32bit-doubles`), plain `char` is unsigned,
`long long` is 4-aligned, `size_t`, `ptrdiff_t` and `wchar_t` are `long`
types, bit-fields use the Microsoft layout, and every C symbol carries
an underscore in the object (`main` is `_main`). Freestanding only. The
calling convention is GCC's (r1-r4 by whole words, nothing split, the
hidden result pointer in r15); EmbCC and rx-elf-gcc objects call each
other (tests/golden/rx-abi.sh). The design notes, the convention in full
and what is refused are in [the RX plan](../internals/rx-plan.md).

| Triple | Accepted aliases |
|---|---|
| `rx-none-elf` | `rx-elf`, `rx-unknown-elf`, `rx` |

Accepted options: `-mcpu=rx600|rx610|rx200|rx100`, `-m32bit-doubles`,
`-nofpu`, `-mlittle-endian-data`, `-mrx-abi`, `-msmall-data-limit=0`,
`-mno-pid`, `-mint-register=0`, `-mallow-string-insns`, `-mrelax`.
Refused by name: `-m64bit-doubles`, `-fpu`, `-mbig-endian-data`,
`-mgcc-abi`, a nonzero `-msmall-data-limit`, `-mpid`, a nonzero
`-mint-register`, `-mno-allow-string-insns`, `-mas100-syntax`, unwind
tables, `-S`, and C++ exceptions (C++ compiles with `-fno-exceptions`; see
[C++](cxx.md#targets)).

EmbCC assembles RX itself, in GNU syntax: inline `__asm__` with GCC's
operand constraints, naked functions, file-scope blocks and `.s`/`.S`
files, through one assembler for RXv1's integer instructions -- every form
encoded as rx-elf-as encodes it (tests/golden/rx-asm.sh compares them
byte for byte, and rx-elf-objdump reads each back), branches relaxed as
GNU as relaxes them, `mov.l #sym` and branches to symbols elsewhere
relocated (`R_RX_DIR32`, `R_RX_DIR24S/16S/8S_PCREL`). A C name is `_name`
in assembly, as with GCC. The vocabulary, the constraints and what is
refused (the FPU's and RXv2's instructions among them) are in
[Inline assembly](inline-asm.md#renesas-rx) and
[embas](tools/embas.md#rx-specifics).

## MIPS32

MIPS32 Release 2, little-endian, with the o32 ABI and soft float: the
core of Microchip's PIC32 parts. Freestanding only. The design notes are
in [the MIPS32 plan](../internals/mips32-plan.md).

### Triples

| Triple | Accepted aliases | ISA | ABI |
|---|---|---|---|
| `mipsel-none-elf` | `mipsel-unknown-elf`, `mipsel-elf`, `mipsel` | MIPS32r2 | o32, soft float |

There is no big-endian target (`mips-none-elf` is unknown).

### Options

EmbCC emits one configuration: MIPS32 Release 2, little-endian, o32, soft
float, without abicalls and without small data. The options a MIPS build
passes are accepted when they ask for exactly that and refused by name
otherwise.

| Option | Accepted values | Refused with |
|---|---|---|
| `-mcpu=CPU`, `-march=CPU` | `mips32r2`, `m4k`, `m14k`, `m14kc`, `24kc`, `24kf`, `24kec`, `24kef`, `34kc`, `74kc` | `-mcpu=mips32r6 is not a MIPS32 Release 2 core: EmbCC emits MIPS32r2 (mips32r2, m4k, m14k, m14kc, 24kc, 24kf, 24kec, 24kef, 34kc, 74kc)` |
| `-mabi=ABI` | `32` | `-mabi=n32 is not supported: EmbCC emits the o32 ABI (-mabi=32) only` |
| `-msoft-float` | (no value) | `-mhard-float is not supported: EmbCC emits soft-float o32, which passes floating point in the integer registers` |
| `-EL` | (no value) | `-EB is not supported: the MIPS target is little-endian (mipsel) only` |
| `-mno-abicalls` | (no value) | `-mabicalls is not supported: EmbCC's MIPS code takes addresses absolutely (lui/addiu) and keeps no $gp; it is -mno-abicalls code` |
| `-G0` | (no value) | `-G8 is not supported: EmbCC puts no data in .sdata and addresses nothing through $gp (-G0)` |

The code runs on any MIPS32 Release 2 or later core that keeps the
Release 2 encodings (`lwl`/`lwr` among them), which Release 6 does not.
A floating-point unit, if the core has one, is not used.

### Calling convention: o32, soft float

- The arguments are laid out as a block in memory: each at its offset
  rounded up to its alignment (at least 4, at most 8), in whole words.
  The first 16 bytes travel in `a0`–`a3` and the rest on the stack at
  the same offset from the caller's `sp`. So after one `int`, a `long
  long` or `double` skips `a1` for `a2:a3`; after three it goes on the
  stack at `sp+16`.
- A structure or union of any size is passed by value in that layout,
  its bytes packed into the words (the low address in the low bits) and
  split between `a3` and the stack when it straddles them.
- The caller always reserves the first 16 bytes of its outgoing area, the
  home area, for the callee to store `a0`–`a3` into.
- `float` and `double` travel exactly as `int` and `long long` do.
- A scalar result comes back in `v0`, or `v0:v1` (low word in `v0`) for a
  `long long` or `double`. Every structure and union is returned through
  a hidden pointer the caller passes in `a0`, and the callee hands it back
  in `v0`. A `_Complex float` comes back in `v0` (real) and `v1`
  (imaginary), a `_Complex double` in `v0:v1` and `a0:a1`, as clang
  returns them.
- A variadic argument follows the same layout. `va_list` is a `void *`;
  a variadic function stores `a0`–`a3` into its home area, so the named
  and unnamed arguments are one block, and `va_arg` of an 8-byte type
  rounds the pointer up to 8 first.
- `s0`–`s7`, `fp`, `gp` and `sp` survive a call. `at`, `k0` and `k1` are
  never used by compiled code except `at` as a scratch.
- The stack is 8-byte aligned. An unnamed bit-field does not affect a
  structure's alignment.

`tests/golden/mips-abi.sh` checks every rule above with EmbCC and clang
calling each other on the board.

### Code generation

Addresses are absolute: `lui` and `addiu` with `R_MIPS_HI16` and
`R_MIPS_LO16`. Calls, inside the unit as well, are `jal` with
`R_MIPS_26`, so caller and callee must share a 256 MiB region, as they
always do in a PIC32 or a KSEG0 image. Branches reach ±128 KiB; a branch
in a function larger than that becomes an inverted branch over a `j`.
A delay slot holds the instruction before the branch, call or return
when that keeps the program -- the classic fill -- and otherwise a `nop`;
a return's holds the release of the function's frame (`jr $ra` then
`addiu $sp, $sp, N`), as clang's does. Under `-g` every slot but the
return's is a `nop`. A dense `switch` dispatches through a jump table of
offsets from the address a `bal` returns, so it needs no relocation. A load or store the compiler
cannot prove aligned (a packed structure's member) uses `lwl`/`lwr` and
`swl`/`swr`, because a misaligned word access traps.

### Object format

ELF32, little-endian, `EM_MIPS`, with REL relocations (`.rel.text`, the
addend stored in the field) as o32 requires. `e_flags` is `0x70001001`:
`EF_MIPS_ARCH_32R2`, `EF_MIPS_ABI_O32` and `EF_MIPS_NOREORDER`; not
`EF_MIPS_CPIC`. Each object has a `.MIPS.abiflags` section saying ISA
MIPS32r2, 32-bit registers and the soft-float ABI.

`embld` links these objects and clang's (`--target=mipsel-unknown-elf
-msoft-float`, in its default non-PIC mode), applying `R_MIPS_32`,
`R_MIPS_26`, `R_MIPS_HI16`/`R_MIPS_LO16` (a HI16's addend completed by the
`R_MIPS_LO16` that follows it) and `R_MIPS_PC16`. It refuses objects
with a different floating-point ABI in their `.MIPS.abiflags`, and the
GOT and gp-relative relocations of PIC and small-data code by name, and
drops `.MIPS.abiflags`, `.reginfo` and `.pdr` from the image.
`-Tstack ADDR` makes it emit an entry stub that sets `sp` and jumps to
the entry symbol.

### Assembly

`embcc -c` assembles `.s` and `.S` files for MIPS32, and file-scope
`asm` blocks and `__attribute__((naked))` functions are assembled the
same way, in GNU as's syntax: `$`-spelt registers, `jal sym`, `%hi(sym)`
and `%lo(sym)`, `la`, `.word sym`, labels and numeric locals, `.set
reorder`/`noreorder`/`push`/`pop`, and the `.ent`/`.end`/`.frame` markers.
A file starts in `.set reorder`, where the assembler fills each delay
slot with a `nop`, as GNU as does; so does an inline-asm template, as
GCC's and clang's do. PIC and small-data code (`.abicalls`, `.cpload`,
`%got`, `%call16`, `%gp_rel`) is refused. The vocabulary and its rules
are in [Inline assembly](inline-asm.md#mips32).

### Predefined macros

From `clang --target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float
-mno-abicalls`: `__mips__`, `__mips` (32), `mips`, `_mips`, `__MIPSEL__`,
`_MIPSEL`, `MIPSEL`, `__mips_isa_rev` (2), `_MIPS_ARCH_MIPS32R2`,
`__mips_o32`, `_ABIO32`, `_MIPS_SIM`, `_MIPS_SZINT`, `_MIPS_SZLONG` and
`_MIPS_SZPTR` (32), `__mips_soft_float`, `__mips_fpr` (0). Not
`__mips_abicalls`: the code is not abicalls code. `__CHAR_UNSIGNED__` is
not defined. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4` is, and the 1- and
2-byte forms are not.

### Runtime

`make rt-embedded` builds `librt.a` and `make libc-embedded` builds
`libc.a` for `mipsel-none-elf` (soft float, 64-bit division, the C
library on its bare-metal backend). `tests/harness/mips` runs programs on
QEMU's `malta` board: the image is linked at 0x80100000 in KSEG0 and
loaded with `-kernel`, the FPGA UART at 0xbf000900 is the console, and an
exception prints its cause and address.

### Atomics

Every atomic is an `ll`/`sc` loop bracketed by `sync`. A one- or two-byte
atomic works on the aligned word around it, as GCC's and clang's do: the
loop rewrites only its lane, `old ^ ((new ^ old) & mask)`, so it is atomic
against the neighbouring bytes too (a store to any of them fails the
`sc`). Big-endian, the lane of offset `a & 3` is counted from the top of
the word. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2` and `_4` are defined.

### Limitations

| Construct | Diagnostic |
|---|---|
| `__builtin_frame_address(N)` or `__builtin_return_address(N)` with N above 0 (level 0 is supported; see [Extensions](extensions.md)) | `__builtin_return_address(1) is not supported on mipsel-none-elf: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `__attribute__((interrupt, use_shadow_register_set))` | `__attribute__((use_shadow_register_set)) is not supported: EmbCC does not switch register sets: ...` |
| `__attribute__((interrupt, use_debug_exception_return))` | `__attribute__((use_debug_exception_return)) is not supported: the handler would return with eret where the debug exception needs deret, and save DEPC as EPC` |
| an interrupt handler with parameters, or with a result | `interrupt handler 'h' takes parameters: ...`, `interrupt handler 'h' returns a value: ...` |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for mipsel-none-elf yet (-funwind-tables, -fasynchronous-unwind-tables, -fexceptions): EmbCC writes no MIPS .eh_frame` |
| C++ with exceptions (on by default) | `C++ exceptions are not supported for mipsel-none-elf yet: EmbCC writes no MIPS .eh_frame; compile with -fno-exceptions`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |

## MIPS64

MIPS64 Release 2 with the n64 ABI and soft float, in either byte order.
Freestanding only. It is the MIPS32 backend at 64 bits; the design notes
are in [the MIPS64 plan](../internals/mips64-plan.md).

### Triples

| Triple | Accepted aliases | ISA | ABI |
|---|---|---|---|
| `mips64el-none-elf` | `mips64el-unknown-elf`, `mips64el-elf`, `mips64el` | MIPS64r2, little-endian | n64, soft float |
| `mips64-none-elf` | `mips64-unknown-elf`, `mips64-elf`, `mips64` | MIPS64r2, big-endian | n64, soft float |

### Options

EmbCC emits one configuration: MIPS64 Release 2, n64, soft float, without
abicalls and without small data, in the triple's byte order. The options
a MIPS64 build passes are accepted when they ask for exactly that and
refused by name otherwise.

| Option | Accepted values | Refused with |
|---|---|---|
| `-mcpu=CPU`, `-march=CPU` | `mips64r2`, `5kc`, `5kf`, `5kec`, `5kef`, `octeon` | `-mcpu=mips64r6 is not a MIPS64 Release 2 core: EmbCC emits MIPS64r2 (mips64r2, 5kc, 5kf, 5kec, 5kef, octeon)` |
| `-mabi=ABI` | `64` | `-mabi=n32 is not supported: EmbCC emits the n64 ABI (-mabi=64) only` |
| `-msoft-float` | (no value) | `-mhard-float is not supported: EmbCC emits soft-float n64, which passes floating point in the integer registers` |
| `-EL` (`mips64el`), `-EB` (`mips64`) | (no value) | `-EB contradicts --target=mips64el-none-elf, which is little-endian: big-endian MIPS64 is --target=mips64-none-elf` |
| `-mno-abicalls` | (no value) | `-mabicalls is not supported: EmbCC's MIPS64 code takes addresses absolutely (%highest..%lo) and keeps no $gp; it is -mno-abicalls code` |
| `-G0` | (no value) | `-G8 is not supported: EmbCC puts no data in .sdata and addresses nothing through $gp (-G0)` |

A floating-point unit, if the core has one, is not used.

### Data model

LP64, as RV64's column above: `long` and pointers 8/8, `long long` 8/8,
`long double` 16/16 IEEE binary128, `__int128` 16/16. Plain `char` is
signed and `wchar_t` is a signed `int`. `__BIGGEST_ALIGNMENT__` is 16 and
the stack is 16-byte aligned.

### Calling convention: n64, soft float

- Arguments take doubleword slots: the first eight in `a0`–`a7`
  (`$4`–`$11`), the rest on the stack from the caller's `sp`, with no home
  area. A `long double`, or a structure aligned to 16, starts at an even
  slot; an `__int128` takes the next slot whatever it is (clang's rule).
- A structure or union of any size is passed by value, its bytes as the
  doublewords `ld` would read -- a short one left-justified big-endian --
  split between `a7` and the stack when it straddles them.
- A 32-bit value, `unsigned` included, travels and is returned
  sign-extended to 64 bits; a `float` as its bits, a `double` as a `long`
  does. A `float` on the stack is the exception: GCC pads it upward, so
  big-endian its four bytes are its slot's first, where an `int`'s are
  its last.
- A scalar result comes back in `v0`, an `__int128` in `v0:v1`, and a
  `long double` in `v0` and `a0` (its first doubleword in memory in
  `v0`). A structure or union of at most 16 bytes comes back in `v0:v1` as
  its doublewords, except that a structure of one or two floating-point
  fields returns each field in its own register (a `float` field in the
  register's high half big-endian), a structure of one `long double` as a
  `long double`, and a `_Complex float` or `_Complex double` each part in
  its own register. A larger one comes back through a hidden pointer the
  caller passes in `a0`, handed back in `v0`.
- A variadic argument takes the same slots; an unnamed `__int128` or
  `long double` starts at an even one, where `va_arg` rounds the pointer
  up to 16. `va_list` is a `void *`: a variadic function stores `a0`–`a7`
  just below its incoming stack words.
- `s0`–`s7`, `fp`, `gp` and `sp` survive a call.

`tests/golden/mips64-abi.sh` and `mips64-be-abi.sh` check every rule above
with EmbCC and clang calling each other on the board.

### Code generation

Addresses are absolute and 64-bit, as clang takes them for n64 without
abicalls: `lui`, `daddiu`, `dsll`, `daddiu`, `dsll`, `daddiu` with
`R_MIPS_HIGHEST`, `R_MIPS_HIGHER`, `R_MIPS_HI16` and `R_MIPS_LO16`. Calls
are `jal` with `R_MIPS_26`, within one 256 MiB region. Branches, delay
slots, jump tables and misaligned accesses are as on [MIPS32](#mips32),
with `ldl`/`ldr` and `sdl`/`sdr` for a doubleword. An `__int128` or
`long double` is computed in two doublewords in memory; 128-bit division,
remainder and every `long double` operation call lib/rt.

### Object format

ELF64, `EM_MIPS`, in the triple's byte order, with RELA relocations whose
`r_info` is n64's record (symbol, then three type bytes; EmbCC writes one
type each, as clang does). `e_flags` is `0x80000001`:
`EF_MIPS_ARCH_64R2` and `EF_MIPS_NOREORDER`. Each object has a
`.MIPS.abiflags` section saying ISA MIPS64r2, 64-bit registers and the
soft-float ABI. `embld` links these objects and clang's
(`--target=mips64el-unknown-elf -mcpu=mips64r2 -msoft-float -mno-abicalls
-G0`), applying `R_MIPS_64`, `R_MIPS_32`, `R_MIPS_26`, `R_MIPS_HIGHEST`,
`R_MIPS_HIGHER`, `R_MIPS_HI16`, `R_MIPS_LO16` and `R_MIPS_PC16`; it
refuses a composite relocation (PIC and gp-relative code) by name.
`-Tstack ADDR` emits the entry stub, both addresses sign-extended 32-bit.

### Predefined macros

From `clang --target=mips64el-unknown-elf` (and `mips64-unknown-elf`)
`-mcpu=mips64r2 -msoft-float -mno-abicalls`: `__mips__`, `__mips` (64),
`__mips64`, `__mips_n64`, `_ABI64`, `_MIPS_SIM`, `_MIPS_SZLONG` and
`_MIPS_SZPTR` (64), `__mips_isa_rev` (2), `__mips_soft_float`, `__LP64__`,
`__SIZEOF_INT128__`, and `__MIPSEL__` or `__MIPSEB__` by the byte order.
`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4` and `_8` are defined; the 1- and
2-byte forms are not.

### Runtime

`make rt-embedded` and `make libc-embedded` build `librt.a` and `libc.a`
for both triples (soft float, binary128, 128-bit integer arithmetic).
`tests/harness/mips64` runs programs on QEMU's `malta` board with a 5KEc
core: the image is linked at 0xffffffff80100000 (KSEG0) and loaded with
`-kernel`, with tests/harness/mips's startup and UART output.

### Atomics

Every atomic is an `ll`/`sc` loop (`lld`/`scd` for eight bytes) bracketed by `sync`. A one- or two-byte
atomic works on the aligned word around it, as GCC's and clang's do: the
loop rewrites only its lane, `old ^ ((new ^ old) & mask)`, so it is atomic
against the neighbouring bytes too (a store to any of them fails the
`sc`). Big-endian, the lane of offset `a & 3` is counted from the top of
the word. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2`, `_4` and `_8` are defined.

### Limitations

| Construct | Diagnostic |
|---|---|
| a 16-byte atomic | `the MIPS64 backend cannot lower a 16-byte atomic (MIPS64's lld/scd are a doubleword; there is no 128-bit ll/sc) yet (function f) [cas16 w=16 size=16]` |
| `__builtin_frame_address(N)` or `__builtin_return_address(N)` with N above 0 (level 0 is supported; see [Extensions](extensions.md)) | `__builtin_return_address(1) is not supported on mips64el-none-elf: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found` |
| `__attribute__((interrupt))` | `__attribute__((interrupt)) is not supported: ...` |
| a doubleword instruction in inline assembly (`daddu`, `ld`, ...) | `asm instruction "daddu $a4, $a5, $a5" is not in the MIPS vocabulary` |
| `la` in a `.s` file or file-scope `asm` | `la loads a 32-bit address, and a MIPS64 address is 64 bits (nor are %highest and %higher assembled here): load it from a .dword holding the symbol` |
| reading a packed bit-field over more than 8 bytes, big-endian | `a packed bit-field 'v' across 9 bytes is not supported on a big-endian target (mips64-none-elf)` |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for mips64el-none-elf yet (...): EmbCC writes no MIPS .eh_frame` |
| C++ with exceptions (on by default) | `C++ exceptions are not supported for mips64-none-elf yet: EmbCC writes no MIPS .eh_frame; compile with -fno-exceptions`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |

## LoongArch64

LA64, little-endian, with the LoongArch psABI's LP64S convention (soft
float): `loongarch64-unknown-elf`. Freestanding only. The design notes
are in [the LoongArch64 plan](../internals/loongarch64-plan.md).
## TriCore

Infineon TriCore 1.6.1, the core of the AURIX TC2xx microcontrollers and
a subset of the TC3xx's TriCore 1.6.2: 32-bit, little-endian, soft float.
Freestanding only. The design notes, and where each fact below comes
from, are in [the TriCore plan](../internals/tricore-plan.md): there is
no TriCore compiler on the machine EmbCC is developed on, so the calling
convention, the data layout, the relocation numbers and the predefined
macros are the TriCore EABI and GCC for TriCore **as remembered, and
unverified** against a reference compiler. The instruction encodings are
checked against QEMU's TriCore translator, and every test runs on QEMU's
`tricore_testboard`.
## ColdFire

Motorola/NXP ColdFire, ISA_A with the hardware divide (an MCF5208 and
every later core), big-endian, soft float: GCC's `m68k-elf` with
`-mcpu=5208`. Freestanding only. The design notes, and which facts are
not yet checked against a real m68k compiler, are in
[the ColdFire plan](../internals/coldfire-plan.md).

EmbCC assembles ColdFire itself, in GNU as's Motorola syntax (`%` optional,
MIT's `An@(d)` accepted): inline `__asm__` with GCC's operand constraints,
naked functions, file-scope blocks and `.s`/`.S` files, through one
assembler for the MCF5208's ISA_A+ -- tests/golden/coldfire-asm.sh reads
every form back with QEMU's m68k disassembler, assembles FreeRTOS's
ColdFire V2 port, and runs a program that uses it against a host model.
Branches relax to .s or .w (the MCF5208 has no 32-bit branch); a `bra` or
`bsr` to a symbol defined elsewhere is a `jmp`/`jsr` to its address
(`R_68K_32`). The vocabulary, the constraints and what is refused are in
[Inline assembly](inline-asm.md#coldfire) and
[embas](tools/embas.md#coldfire-specifics).

### Triples

| Triple | Accepted aliases | ISA | ABI |
|---|---|---|---|
| `loongarch64-unknown-elf` | `loongarch64-none-elf`, `loongarch64-elf`, `loongarch64` | LA64 base integer | LP64S |

There is no LA32 target (`loongarch32-unknown-elf` is unknown).

### Options

EmbCC emits one configuration: the LA64 base integer ISA, LP64S, the
normal code model. The options a LoongArch build passes are accepted when
they ask for that, or for something it is a valid part of, and refused by
name otherwise.

| Option | Accepted values | Refused with |
|---|---|---|
| `-march=ARCH` | `loongarch64`, `la64v1.0`, `la64v1.1`, `la464`, `la664` | `-march=la32v1.0 is not an LA64 architecture: EmbCC emits the LA64 base integer ISA (loongarch64, la64v1.0, la64v1.1, la464, la664)` |
| `-mtune=CPU` | any | -- |
| `-mabi=ABI` | `lp64s` | `-mabi=lp64d is not supported: EmbCC emits the soft-float LP64S convention (-mabi=lp64s), which passes floating point in the integer registers` |
| `-mfpu=FPU` | `none`, `0` | `-mfpu=64 is not supported: EmbCC's LoongArch code uses no FPU (-mfpu=none)` |
| `-msoft-float` | (no value) | `-mdouble-float` / `-msingle-float is not supported: EmbCC emits soft-float LP64S code (-msoft-float)` |
| `-mcmodel=MODEL` | `normal`, `medium` | `-mcmodel=extreme is not supported: EmbCC emits the normal code model (bl, pcalau12i + addi.d)` |
| `-mrelax`, `-mno-relax` | (no value) | -- |
| `-mno-strict-align` | (no value) | `-mstrict-align is not supported: EmbCC's LoongArch code may access a packed member unaligned, as LA64 permits` |
| `-mno-lsx`, `-mno-lasx` | (no value) | `-mlsx is not supported: EmbCC emits no LSX or LASX vector instructions` |

The code runs on every LA64 core; a floating-point or vector unit, if
the core has one, is not used.

### Calling convention: LP64S

The LoongArch psABI's integer convention, which is RISC-V's LP64 rule for
rule:

- Arguments go in `a0`–`a7` (`$r4`–`$r11`), then on the stack, each
  stack argument in an 8-byte slot (16-aligned for a 16-aligned type).
  A 16-byte scalar (`__int128`, `long double`) takes two registers with
  no alignment when named -- `f(int, __int128)` uses `a1:a2` -- and an
  EVEN-aligned pair when variadic; with one register left it is split
  between `a7` and the stack.
- A structure of at most 16 bytes travels packed in one or two registers
  (or split across `a7` and the stack); a larger one is passed by
  reference to a copy the caller makes.
- Results come back in `a0` (and `a1`); a structure larger than 16 bytes
  through a hidden pointer in `a0`. `_Complex` values are small
  structures.
- A 32-bit value is kept sign-extended in its 64-bit register, `unsigned
  int` included.
- `float` and `double` travel in the integer registers as their bits,
  and a variadic `float` is promoted to `double`. Floating-point
  operations call lib/rt (`__adddf3`, ...).
- `va_list` is a `void *`.
- `fp` (`$r22`) and `s0`–`s8` survive a call. `tp` (`$r2`) and `$r21`
  are never used by compiled code.
- The stack is 16-byte aligned. An unnamed bit-field does not affect a
  structure's alignment.

`tests/golden/loongarch-abi.sh` checks these rules with EmbCC and clang
calling each other on the board.

### Code generation

Calls are `bl` with `R_LARCH_B26` (+-128 MiB); addresses are `pcalau12i`
and `addi.d` with `R_LARCH_PCALA_HI20` and `R_LARCH_PCALA_LO12`.
Conditional branches reach +-128 KiB (`beqz`/`bnez` +-4 MiB); one that
does not reach becomes an inverted branch over a `b`. A dense `switch`
dispatches through a table of offsets from a `pcaddi`. One- and two-byte
atomics are `ll.w`/`sc.w` loops on the word that holds them; four- and
eight-byte ones are the `am*_db` instructions and `ll`/`sc` loops.

### Object format

ELF64, little-endian, `EM_LOONGARCH`, RELA relocations, `e_flags` `0x41`
(soft-float ABI, object ABI v1), as clang writes for `-mabi=lp64s`.

`embld` links these objects and clang's (`--target=loongarch64-unknown-elf
-msoft-float`, its default medium code model included): the data and
ADD/SUB relocations, `B16`/`B21`/`B26`, `CALL36`, the PCALA pair, the ABS
sequence and `PCREL20_S2`. It builds no GOT: clang's GOT access
(`GOT_PC_HI20`/`GOT_PC_LO12`) is rewritten to the direct address.
`R_LARCH_RELAX` and `R_LARCH_ALIGN` are ignored. It refuses objects for
the hard-float ABIs, and the extreme code model's and TLS relocations, by
name. `-Tstack ADDR` makes it emit an entry stub that sets `sp` and
jumps to the entry symbol.

### Assembly

`embcc -c` assembles `.s` and `.S` files for LoongArch64, and file-scope
`asm` blocks and `__attribute__((naked))` functions are assembled the same
way, in GNU as's syntax with `$`-spelt registers. The vocabulary is the
base integer ISA, the AM* atomics, the barriers and the privileged
instructions (`csrrd`, `csrwr`, `csrxchg`, `ertn`, `idle`, `cpucfg`,
`rdtime*`, `iocsr*`), llvm-mc's pseudos (`nop`, `move`, `li.w`, `li.d`,
`jr`, `ret`, `bgt`, `ble`, `bgtu`, `bleu`, `bltz`, `bgez`, `bgtz`,
`blez`), and with a symbol `b`/`bl`, `call36`/`tail36`, `la.pcrel`,
`la.local`, `la`/`la.global` and the `%pc_hi20`/`%pc_lo12`,
`%got_pc_hi20`/`%got_pc_lo12`, `%abs_hi20`/`%abs_lo12`/`%abs64_lo20`/
`%abs64_hi12` and `%call36` operators. In inline asm a register operand
is written `$a0`, an `"m"` operand `$a0, 0`; the constant letters are
`i`, `n`, `I`, `J` and `K`. `la.abs`, the TLS operators and
floating-point and vector instructions are refused.

### Predefined macros

From `clang --target=loongarch64-unknown-elf -msoft-float`:
`__loongarch__`, `__loongarch64`, `__loongarch_grlen` (64),
`__loongarch_frlen` (0), `__loongarch_soft_float`, `__loongarch_lp64`,
`__loongarch_arch` and `__loongarch_tune` (`"loongarch64"`), `_LP64` and
`__LP64__`. Not `__loongarch_sx` (no LSX). `__CHAR_UNSIGNED__` is not
defined. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2`, `_4` and `_8` are.
## Xtensa

Little-endian Xtensa with the windowed-register ABI: the instruction set
of Espressif's ESP32 (Xtensa LX6) and ESP32-S3 (LX7). Freestanding only.
The design notes are in [the Xtensa plan](../internals/xtensa-plan.md).

### Triples

| Triple | Accepted aliases | Cores | ABI |
|---|---|---|---|
| `xtensa-none-elf` | `xtensa-esp32-elf`, `xtensa-esp32s3-elf`, `xtensa-esp-elf`, `xtensa-elf`, `xtensa` | ESP32 (LX6), ESP32-S3 (LX7) | windowed, soft float |

There is no big-endian target (`xtensaeb-none-elf` is unknown).

### Options

EmbCC emits one configuration: the windowed ABI, little-endian, literal
pools in `.text` before each function, direct `call8`s, `memw` before every
volatile access. GCC's Xtensa options that ask for that, or that only tune
GCC's own placement and costs, are accepted; the rest are refused by name.

| Option | Accepted | Refused with |
|---|---|---|
| `-mabi=` | `windowed` | `-mabi=call0 is not supported: EmbCC emits the windowed ABI (call8/entry/retw), which ESP-IDF uses` |
| `-mlongcalls`, `-mno-longcalls` | both: calls are direct; `embld` refuses one beyond `call8`'s 512 KiB | |
| `-mtext-section-literals`, `-mauto-litpools` (and `-mno-`) | yes | |
| `-mserialize-volatile`, `-mno-serialize-volatile` | yes; `memw` is emitted either way | |
| `-mlittle-endian` | yes | `-mbig-endian is not supported: every target EmbCC emits for is little-endian` |
| `-mdynconfig=` | `xtensa_esp32.so`, `xtensa_esp32s3.so` | `-mdynconfig=xtensa_esp32s2.so is not supported for xtensa-none-elf: ...` |
| `-mtarget-align`, `-mforce-no-pic`, `-mstrict-align`, `-mextra-l32r-costs=`, `-mno-const16` | yes | |
| `-mconst16`, `-mforce-l32`, `-mfix-esp32-psram-cache-issue` | | refused by name |

The code uses the core instruction set with windowed registers, MUL32
(`mull`), DIV32 (`quos`, `quou`, `rems`, `remu`), MINMAX, SEXT, NSA, ABS,
ADDX and L32R, and S32C1I for atomics -- what the ESP32, the ESP32-S3 and
QEMU's de212 core share. It does not use the FPU, MUL32_HIGH, the loop
instructions, MAC16, the boolean registers or the density option's
16-bit instructions. The ESP32-S2 has no S32C1I, so an atomic
read-modify-write is not for it.

### Atomics

Every atomic is an `s32c1i` loop (the store that happens only while the
word still equals `SCOMPARE1`) bracketed by `memw`. A one- or two-byte
atomic works on the aligned word around it, as GCC's does: the loop
rewrites only its lane, `old ^ ((new ^ old) & mask)`, so it is atomic
against the neighbouring bytes too (a store to any of them makes the
`s32c1i` fail and the loop go round), and a compare-and-swap compares only
its lane. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2` and `_4` are defined.

### Calling convention: windowed, soft float

- A `call8` rotates the register window by eight: the caller puts the
  arguments in `a10`-`a15` and the callee finds them in `a2`-`a7`; the
  caller's `a0`-`a7` survive the call and `a8`-`a15` do not. `a0` holds
  the return address, `a1` is the stack pointer. A function begins with
  `entry a1, N` and returns with `retw`.
- Arguments are counted in words, six in registers. A type aligned beyond
  4 (`long long`, `double`, a structure containing one) starts at an even
  word. An argument that does not fit entirely in the words left goes on
  the stack, and so does every argument after it. Stack arguments are at
  the caller's `sp`, each at its own alignment (4 to 16).
- A structure or union of any size is passed by value. A `_Complex` is
  passed as its two parts, each placed as an argument of its own.
- `float` and `double` travel exactly as `int` and `long long` do: in the
  address registers, whether or not the core has an FPU, so EmbCC's code
  links with GCC's hard-float ESP32 code.
- A result of up to 16 bytes comes back in `a2`-`a5` (the caller's
  `a10`-`a13`), a structure in its memory order; a larger one through a
  hidden pointer passed as the first argument, which the callee returns.
- `char` and `short` arguments and results are extended by the side that
  produces them.
- `va_list` is GCC's 12-byte record, `{ int *__va_stk; int *__va_reg; int
  __va_ndx; }`, held and passed by value, so a `va_list` passes between
  EmbCC's and GCC's code.
- The stack is 16-byte aligned. Every frame is at least 32 bytes, its top
  32 bytes reserved for the window spill handlers. An unnamed bit-field
  does not affect a structure's alignment.

`tests/golden/xtensa-abi.sh` checks these rules with EmbCC and Espressif's
GCC calling each other on the board.

### Code generation

Constants that `movi` (or `movi` and a shift) cannot build, and every
address, are loaded with `l32r` from a literal pool placed right before
the function, since `l32r` reaches only backwards; the function's symbol
is at its entry, after the pool. Calls are `call8` with
`R_XTENSA_SLOT0_OP`, even within the unit. A comparison's value is a
branch over a `movi`. A conditional branch that does not reach (±128 bytes,
±2 KiB against zero) becomes the inverse branch over a `j`, and one beyond
`j`'s ±128 KiB an `l32r` of the label's address and a `jx`; a function more
than 256 KiB from its pool reads a literal from an island in the code. A
frame larger than `entry` can allocate, and every `alloca`, moves `sp` with
`movsp`. A load or store the compiler cannot prove aligned (a packed
structure's member) is done a byte at a time, because a misaligned access
raises an exception. A dense `switch` stays a decision tree.

### Object format

ELF32, little-endian, `EM_XTENSA` (94), RELA relocations, `e_flags`
`0x300` (`EF_XTENSA_XT_INSN | EF_XTENSA_XT_LIT`), as GNU as writes for the
ESP32. `embld` links these objects and GCC's, applying `R_XTENSA_32` and
`R_XTENSA_SLOT0_OP` (on `callN`, `j`, the branches, the loop
instructions, `beqz.n`/`bnez.n` and `l32r`) and ignoring
`R_XTENSA_ASM_EXPAND` and the `DIFF` types, which only a relaxing linker
changes. It does not relax and mints no trampolines: a `call8` beyond 512
KiB, or an `l32r` whose literal is not before it, is refused (compile GCC's
side with `-mtext-section-literals`). `-Tstack ADDR` makes it emit an entry
stub that sets `sp` over a valid bottom frame, sets `PS` (window
exceptions on, level 0) and `WINDOWSTART`, and calls the entry with
`callx8`.

### Assembly

`embcc -c` assembles `.s` and `.S` files for Xtensa, and file-scope `asm`
blocks and `__attribute__((naked))` functions are assembled the same way,
in GNU as's Xtensa syntax. The vocabulary is the ESP32's: the core ALU,
shift and SAR instructions, the MUL32/DIV32 and MIN/MAX options, the
loads and stores, `movi`, `mov`, `addi`, `addmi`, every branch form (two
registers, against zero, against a `b4const`, `bbci`/`bbsi` and their
`.l` spellings), the zero-overhead loops, `j`, `jx`, `call0/4/8/12`,
`callx0/4/8/12`, `ret`, `retw`, `entry`, `movsp`, `rotw`, `rsr`/`wsr`/`xsr`
by the ESP32's special-register names (`ps`, `epc1`-`epc7`,
`excsave1`-`excsave7`, `eps2`-`eps7`, `intenable`, `interrupt`, `intset`,
`intclear`, `ccount`, `ccompare0`-`2`, `vecbase`, `sar`, `windowbase`,
`windowstart`, `lbeg`, `lend`, `lcount`, `scompare1`, `atomctl`,
`exccause`, `excvaddr`, `depc`, `prid`, `cpenable`, `misc0`-`3`, ...) with
GNU's read/write rules, or by number, or as `rsr.ps`; `rur`/`wur` of
`threadptr`, `rsil`, `waiti`, the syncs, `memw`, `extw`, `break`, `ill`,
`rfe`, `rfde`, `rfi`, `rfwo`, `rfwu`, `syscall`, `simcall`, `s32c1i`,
`l32ai`, `s32ri`, `l32e` and `s32e`. GNU's `_` prefix is accepted. The
density option's `.n` forms are refused by name (no instruction here is
16 bits), as is anything else outside the list.

A symbol is a target of a branch, a loop, `j`, `callN` or `l32r` with
`R_XTENSA_SLOT0_OP`, and a `.word` with `R_XTENSA_32`. `movi aN, sym` -- or
a constant `movi` cannot hold -- is an `l32r` of a literal, and
`.literal NAME, X, ...` names literals of its own; the literals go where
GNU as's `--text-section-literals` puts them: in the latest pool placed
before the code, at the start of the section, at each
`.literal_position`, and before the labels of each function's `entry`.
`.align` counts bytes, as GNU as's does for Xtensa. `.begin`/`.end` blocks
that only restrict relaxation (`no-transform`, `literal_prefix`,
`schedule`, ...) are accepted; `.begin longcalls` and
`absolute-literals` would change the code and are refused.
`tests/golden/xtensa-asm.sh` checks every form against QEMU's de212
disassembler and Espressif's GNU as, and a file's layout against GNU as's.

In inline asm a register operand is written `a10`, an `"m"` operand
`a10, 0`; the constraint letters are `r`, `a`, `g`, `m`, `i`, `n` and
GCC's `I`-`P`. No operand is put in `a0`/`a1` (the return address and the
stack pointer), `a7` or `a14`/`a15`; a clobber list may not name `a0` or
`a1`; a template's `call4`/`call8`/`call12` (or `callx`) clobbers the
callee's window -- `a4`/`a8`/`a12` up to `a15` -- whatever the clobber list
says, and `call0`/`callx0`, which would write `a0`, are refused. A
template may use numeric labels (`1:`, `1b`, `1f`); it cannot name a
symbol, since its bytes carry no relocation.

### Predefined macros

From Espressif's `xtensa-esp32-elf-gcc` 16.1: `__xtensa__`, `__XTENSA__`,
`__XTENSA_EL__`, `__XTENSA_WINDOWED_ABI__`, the ESP32's `__XCHAL_*`
configuration, `__CHAR_UNSIGNED__`, `__WCHAR_TYPE__` `short unsigned int`,
`__INT32_TYPE__` `long int`. Not `__XTENSA_SOFT_FLOAT__`: the ESP32's GCC
does not define it, and the float ABI is the same either way.
`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2` and `_4` are defined.
## SPARC

SPARC V8, big-endian, as Gaisler's LEON3 implements it -- the processor
of many spacecraft -- with soft float and LEON3's integer multiply and
divide. The design and the facts behind it (read off clang 23) are in
[the SPARC plan](../internals/sparc-plan.md).

### Triples

| Triple | Accepted aliases | Core | ABI |
|---|---|---|---|
| `sparc-none-elf` | `sparc-unknown-elf`, `sparc-elf`, `sparc-gaisler-elf`, `sparc` | SPARC V8 with MUL/DIV (LEON3) | SPARC V8 ABI, soft float |

### Options

EmbCC emits one configuration: SPARC V8 with `umul`/`smul`/`udiv`/`sdiv`,
register windows, soft float, `%g2`-`%g4` used as scratch. The flags that
say so are accepted; the rest are refused by name.

| Option | Accepted | Refused, for example |
|---|---|---|
| `-mcpu=CPU`, `-march=CPU`, `-mtune=CPU` | `leon3`, `leon4`, `v8`, `gr712rc`, `gr740`, `ut699` | `-mcpu=v9 is not a SPARC V8 core with hardware multiply and divide: EmbCC emits LEON3 code (...)` |
| `-msoft-float`, `-mno-fpu` | (no value) | `-mhard-float is not supported: EmbCC emits soft-float SPARC code, which passes floating point in the integer registers` |
| `-mno-flat`, `-mapp-regs`, `-mv8`, `-m32`, `-mcmodel=medlow`, `-mbig-endian` | (no value) | `-mflat is not supported: EmbCC's SPARC code uses register windows (save and restore)`; `-mno-app-regs`, `-m64`, `-mcmodel=medany`, `-mfix-*` (no errata workarounds), `-mlittle-endian` likewise |

### Calling convention

Every function opens a register window (`save`) and returns with `ret;
restore`. Arguments are words with no padding: the first six in
`%o0`-`%o5` (the callee's `%i0`-`%i5`), the rest at `%sp+92`; a `long
long` or `double` is two words, high first, and may straddle `%o5` and the
stack. Every structure, union, `_Complex` and `long double` is passed by
reference to a copy the caller makes. Results: `%o0`, or `%o0:%o1` for 8
bytes; a `_Complex float` in `%o0:%o1` and a `_Complex double` in
`%o0`-`%o3`; a structure, union, `_Complex long double` or `long double`
through a buffer whose address the caller stores at `%sp+64`, with an
`unimp` holding its size after the call, which the callee returns past.
`va_list` is a pointer over the argument words. `tests/golden/sparc-abi.sh`
checks this with EmbCC and clang calling each other.

clang calls the binary128 helpers (`__addtf3`, ...) with their operands
in registers, which no SPARC runtime implements; EmbCC calls them as it
calls any function taking a `long double`, by reference, and its
`lib/rt` defines them that way. Code compiled by clang that does
`long double` arithmetic therefore does not link correctly against
EmbCC's runtime. clang's own callee of a `_Complex long double` returns to
`%i7+8`, not past its caller's `unimp` -- a clang bug; EmbCC's follows the
ABI.

### Objects and linking

ELF32, big-endian, `EM_SPARC`, `e_flags` 0, RELA relocations: an address
is `sethi`/`or` with `R_SPARC_HI22`/`R_SPARC_LO10`, a call is `call` with
`R_SPARC_WDISP30`, data words `R_SPARC_32`. `embld` links these objects and
clang's (non-PIC), and refuses the GOT and PC-relative-address relocations
of PIC code by name. `-Tstack` emits a stub that sets `%sp` and jumps to
the entry.

### Atomics

An exchange of a word is `swap`; every other atomic is LEON3's `casa`
(ASI 10), after a `stbar`, and a read-modify-write is a `casa` loop. A
one- or two-byte atomic works on the aligned word around it, as GCC's and
clang's do: the loop rewrites only its lane, `old ^ ((new ^ old) & mask)`,
so it is atomic against the neighbouring bytes too (a store to any of them
makes the `casa` fail and the loop go round with the word it saw), and a
compare-and-swap compares only its lane. The lane of offset `a & 3` is
counted from the top of the word (big-endian). Test-and-set stores 1, as
`__GCC_ATOMIC_TEST_AND_SET_TRUEVAL` says, not `ldstub`'s 0xff. No
`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_N` is defined: the table is clang's for
`-mcpu=leon3`, which defines none.

### Assembly

`embcc -c` assembles `.s` and `.S` files for SPARC, and inline `asm`,
file-scope `asm` blocks and `__attribute__((naked))` functions are
assembled the same way, in GNU as's SPARC syntax (registers `%g0`-`%i7`,
`%r0`-`%r31`, `%sp`, `%fp`). The vocabulary is the LEON3's integer unit:
the ALU in its register and 13-bit immediate forms, with the
condition-code, tagged and `mulscc` forms, `umul`/`smul`/`udiv`/`sdiv`,
`save`/`restore`; every load and store (`ld`, `ldub`, `lduh`, `ldsb`,
`ldsh`, `ldd`, `st`, `stb`, `sth`, `std`, `ldstub`, `swap`) and its
alternate-space form (`lda [rs1 + rs2] ASI, rd`), LEON's `casa`; `sethi`
with `%hi()` and `%lo()`; every `Bicc` with its `,a` annul bit; `call`,
`jmpl`, `rett`; `rd`/`wr` of `%y`, `%psr`, `%wim`, `%tbr` and
`%asr1`-`%asr31`; every `Ticc`; `flush`, `stbar`, `unimp`, `nop`; and
GNU's synthetic instructions -- `mov` (to and from the state registers
too), `cmp`, `tst`, `not`, `neg`, `inc`/`dec`(`cc`), `clr`/`clrb`/`clrh`,
`btst`, `bset`, `bclr`, `btog`, `set`, `jmp`, `ret`, `retl`, `b` and the
condition synonyms (`bnz`, `bz`, `bgeu`, `blu`). Delay slots are the
programmer's: nothing is reordered or filled. Floating-point and
coprocessor instructions and SPARC V9's forms (`,pt`, `%xcc`, `ldx`,
`membar`...) are refused by name.

A call or a branch to a symbol defined elsewhere carries
`R_SPARC_WDISP30`/`WDISP22`; `%hi(sym)`, `%lo(sym)` and `set sym, rd`
carry `R_SPARC_HI22`/`LO10` (for a label of the same file too, whose
address the linker decides), and `.word sym` `R_SPARC_32`. `.align`
counts bytes, `!` starts a comment and `#` one at a line's start, as GNU as
has them for SPARC. `tests/golden/sparc-asm.sh` checks every form against
llvm-mc byte for byte and against llvm-objdump's mnemonic, a file's layout
against clang's assembler, and runs C and assembly together on QEMU's
`leon3_generic`.

In inline asm a register operand is written `%o0`, an `"m"` operand
`[%o0]`; the constraint letters are `r`, `g`, `m`, `i`, `n`, GCC's `I`-`P`
and a digit (an input tied to an output). Operands go in `%o0`-`%o5`,
`%l0`-`%l5` and `%i0`-`%i5` -- never a global, `%sp`/`%fp`, `%o7`, `%i7` or
`%l6`/`%l7` -- and a clobber list may not name `%sp`, `%fp` or `%i7`. A
value live across an asm keeps out of every register the asm changes: its
operands', its clobbers', the ones its text names and, when it calls
(`call`, or `jmpl` into `%o7`), the outs and `%g1`-`%g4`. `-S` writes the
instructions as `.byte` and their relocations as `.reloc` with SPARC's
names, which llvm-mc assembles back into the same object.

### Predefined macros

From `clang --target=sparc-none-elf -mcpu=leon3 -msoft-float`:
`__sparc__`, `__sparc`, `sparc`, `__sparcv8`, `__sparcv8__`,
`__BIG_ENDIAN__`, `__BYTE_ORDER__` big-endian, `SOFT_FLOAT`,
`__SIZEOF_LONG_DOUBLE__` 16 and `__LDBL_MANT_DIG__` 113.

### Runtime

`make rt-embedded` builds `librt.a` and `make libc-embedded` builds
`libc.a` for `loongarch64-unknown-elf` (soft float, binary128 and the
128-bit integer routines, the C library on its bare-metal backend).
`tests/harness/loongarch` runs programs on QEMU's `virt` board: the image
is linked at 0x1000000 and loaded with `-kernel`, the UART at 0x1fe001e0
is the console, an exception prints its code and address, and the ACPI
GED's sleep register powers the board off at the end.
| `tricore-none-elf` | `tricore-elf`, `tricore-unknown-elf`, `tricore` | TriCore 1.6.1 | TriCore EABI, soft float |

### Options

| Option | Accepted values | Refused with |
|---|---|---|
| `-mcpu=CPU`, `-march=CPU`, `-mCPU` | `tc16`, `tc161`, `tc162`, `tc1.6`, `tc1.6.1`, `tc1.6.2`, `tc16x`, `tc2xx`, `tc22xx`, `tc23xx`, `tc26xx`, `tc27xx`, `tc29xx`, `tc3xx`, `tc33xx`, `tc36xx`, `tc37xx`, `tc38xx`, `tc39xx` | `-mcpu=tc1797 is not a TriCore 1.6 core: EmbCC emits TriCore 1.6.1 code, for the AURIX TC2xx and TC3xx (...)` |
| `-msoft-float`, `-mlittle-endian` | (no value) | `-mhard-float is not supported: EmbCC emits soft float for TriCore (the TC3xx FPU is not used yet)` |

### Calling convention (TriCore EABI, unverified)

- A pointer argument travels in the next free of `A4`–`A7`; any other
  scalar of 32 bits or fewer in the lowest free of `D4`–`D7`; a `long
  long` or `double` in the next free even pair, `E4` (`D4:D5`) or `E6`,
  low word in the even register -- a register skipped on the way is
  filled by a later 32-bit argument.
- A structure or union of 8 bytes or fewer travels as an integer of its
  size; a larger one by reference, the caller passing the address of its
  own copy as a pointer argument.
- What finds no register goes on the stack in whole words from the
  caller's `A10`, 4-aligned whatever its size. Every unnamed argument of
  a variadic call goes there too, so `va_list` is a `char *`.
- A pointer result comes back in `A2`; another of 32 bits or fewer in
  `D2`; a 64-bit one, or a struct of 5-8 bytes, in `E2`; a larger struct
  through a hidden pointer the caller passes in `A4`.
- `CALL` saves the upper context (`D8`–`D15`, `A10`–`A15`) in a
  context-save area and `RET` restores it, so those registers and the
  stack pointer survive every call with no save code. `A0`, `A1`, `A8`
  and `A9` are the system's and never touched.
- The stack is 8-byte aligned; `long long` and `double` are 4-aligned in
  memory.

### Code generation

Every value lives in a data register or a frame slot; address registers
are loaded just before a load or store, and for a call's pointer
arguments and results. Addresses are absolute: `movh` and `addi` with
`R_TRICORE_HIADJ` and `R_TRICORE_LO`. Calls are `CALL` with
`R_TRICORE_24REL` (±16 MiB). Conditional branches reach ±32 KiB; a branch
in a function larger than that becomes an inverted branch over a `J`. A
dense `switch` is a tree of compares (no jump tables yet). A load or store
the compiler cannot prove aligned goes a byte at a time. Only the 32-bit
encodings are emitted.

### Atomics

An exchange of a word is `SWAP.W`, a compare-and-swap `CMPSWAP.W`, and a
read-modify-write a `CMPSWAP.W` loop, each bracketed by `DSYNC`. A one- or
two-byte atomic works on the aligned word around it: the loop rewrites
only its lane, `old ^ ((new ^ old) & mask)`, so it is atomic against the
neighbouring bytes too (a store to any of them makes the `CMPSWAP.W` fail
and the loop go round). A compare-and-swap hands `CMPSWAP.W` the word last
seen with the expected value in the lane; when it fails because a
neighbour moved, the loop goes round, and when the lane differs, it
fails. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2` and `_4` are defined.

### Object format and linking

ELF32, little-endian, `EM_TRICORE` (44), RELA relocations, `e_flags`
`0x00200000`. `embld` links these objects, applying `R_TRICORE_32ABS`,
`R_TRICORE_24REL`, `R_TRICORE_HIADJ`, `R_TRICORE_LO` and `R_TRICORE_LO2`,
and refuses the small-data relocations by name. `-Tstack ADDR --csa
START:END` makes it emit an entry stub that sets `A10`, links the
context-save areas in [START, END) into the free list every `CALL` draws
from, turns call-depth counting off and jumps to the entry symbol; the
areas must be 64-byte aligned and within the first 4 MiB of one 256 MiB
segment.

### Assembly

Inline `asm` is assembled by EmbCC's TriCore vocabulary
(`src/arch/tricore/asm.c`): the system instructions (`mtcr`/`mfcr` with
core registers by name or number, `isync`, `dsync`, `syscall`,
`enable`, ...), the moves, the ALU forms, the loads and stores,
`swap.w`, `cmpswap.w` and the indirect jumps and calls. Constraints:
`d`/`r` a data register, `a` an address register, `m` an address register
holding the operand's address (written `[%0]`), `i` a constant; register
variables bound to `d0`-`d7` or `a2`-`a7`.

The same vocabulary has the control transfers: `j`, `jl` and `call` (+-16
MiB), the conditional branches `jeq`, `jne`, `jlt`, `jlt.u`, `jge`,
`jge.u` against a register or a 4-bit constant, `jz`/`jnz` (jeq/jne
against 0), `jeq.a`, `jne.a`, `jz.a`, `jnz.a` and `loop` (+-32 KiB), each
to `.+N`/`.-N` or, in a template, a numeric label (`1:`, `1b`, `1f`).
`embcc -c` assembles `.s` and `.S` files, and file-scope `asm` blocks and
`__attribute__((naked))` functions are assembled the same way, in GNU
syntax with optional `%` on registers; there `j`/`jl`/`call sym` carry
`R_TRICORE_24REL`, an address is `movh`/`movh.a` with `hi:sym` or
`%hi(sym)` (`R_TRICORE_HIADJ`) and `addi` with `lo:sym` or `%lo(sym)`
(`R_TRICORE_LO`) or `lea`, a load or a store with `[aB]lo:sym`
(`R_TRICORE_LO2`), and `.word sym` is `R_TRICORE_32ABS`. A conditional
branch or `loop` reaches only a label of its own section (`embld` does not
apply `R_TRICORE_15REL`), and is refused by name with a symbol defined
elsewhere. `tests/golden/tricore-gas.sh` runs every transfer, assembled
from its text, through QEMU's TriCore translator (tricore-encoding.sh's
referee), and a `.S` file with C on the board.

### Predefined macros

`__tricore__`, `__TRICORE__`, `__TC161__`, `__TRICORE_CORE__` and
`__TRICORE_NAME__` (`0x161`), with the ILP32 set; `__CHAR_UNSIGNED__` is
not defined. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1`, `_2` and `_4` are.

### Runtime

`make rt-embedded` and `make libc-embedded` build `librt.a` and `libc.a`
for `tricore-none-elf`. `tests/harness/tricore` runs programs on QEMU's
`tricore_testboard` (`-cpu tc27x`): the image is linked at 0x80000000 and
loaded with `-kernel`; the board has no UART, so a TCG plugin
(`tests/harness/tricore/putc.c`) prints what the harness stores to its
output word; a trap prints its class, number and address.
`libc.a` for `xtensa-none-elf` (soft float, 64-bit multiply and division,
the C library on its bare-metal backend). `tests/harness/xtensa` runs
programs on QEMU's `sim` machine with the de212 core (an LX6 with no FPU
or MMU): the generic loader starts the image at its entry, the harness
installs the window overflow and underflow handlers and the Alloca
exception's, output and exit are the sim machine's simcalls, and any other
exception prints its cause, `EPC1` and `EXCVADDR`.
`libc.a` for `sparc-none-elf` (soft float, 64-bit division, binary128,
the C library on its bare-metal backend). `tests/harness/sparc` runs
programs on QEMU's `leon3_generic` board: the image is linked at
0x40000000 and loaded with `-kernel`, the APBUART at 0x80000100 is the
console, and the harness installs the window overflow and underflow trap
handlers a windowed program needs (any other trap prints its type and
address). A program for real LEON3 hardware needs the same handlers in its
startup, as BCC's provides.
| `m68k-none-elf` | `m68k-unknown-elf`, `m68k-elf`, `m68k` | ColdFire ISA_A | m68k SVR4, soft float |

The 68000 family proper (68000-68060, CPU32) is not a target.

### Options

| Option | Accepted values | Refused |
|---|---|---|
| `-mcpu=CPU`, `-mtune=CPU`, `-mCPU` | `5208`, `5207`, `5206e`, `5211`-`5216`, `5235`, `5249`, `5271`-`5282`, `5307`, `5329`, `5373`, `5407`, `54455` and their kin | a 68000-family CPU; `5206`, `5202`, `5204` (no divider); `547x`/`548x` (an FPU) |
| `-march=ISA` | `isaa`, `isaaplus`, `isab`, `isac` | anything else |
| `-msoft-float`, `-mdiv`, `-mno-align-int`, `-mno-short`, `-mno-rtd`, `-m[no-]strict-align`, `-mbig-endian` | (no value) | `-mhard-float`, `-mno-div`, `-malign-int`, `-mshort`, `-mrtd`, `-mpcrel`, `-mid-shared-library`, `-msep-data`, `-mxgot`, `-mlittle-endian` |

### Calling convention

- Every argument is on the stack in whole 4-byte words, the first at
  `4(%sp)` on entry: a scalar of four bytes or fewer promoted to a word
  (a `char`'s byte is its word's last), a `long long` or `double` two
  words, high first, a structure or union by value, its size rounded up
  to a word -- right-justified when smaller than a word. The caller pops.
- A variadic function's unnamed arguments are laid out as named ones;
  `va_list` is a `char *`.
- A scalar result comes back in `d0`, or `d0:d1` (high word in `d0`); a
  pointer in `a0` and `d0`; a `_Complex float` in `d0`/`d1`, a `_Complex
  double` in `d0:d1`/`d2:d3`. Every other structure and union is returned
  through the buffer whose address the caller passes in `a1`.
- `d2`-`d7` and `a2`-`a6` survive a call; `a6` is the frame pointer, which
  every function links.
- Nothing is aligned beyond 2 bytes, so `struct { char c; int i; }` is 6
  bytes; the stack is 4-aligned at a call.

`tests/golden/coldfire-abi.sh` checks that caller and callee agree, EmbCC
to EmbCC at every optimization pairing.

### Code generation

Operands are effective addresses, so a frame slot is read in place. The
allocator's data class is `d2`-`d7` and its address class `a2`-`a5`.
Addresses and calls are 32-bit absolute (`move.l #sym`, `lea sym`,
`jsr sym`, `R_68K_32`). Branches are `bcc.b`/`bcc.w`; one beyond 32 KiB
becomes a position-independent `lea`/`adda.l`/`jmp`. A 64-bit multiply,
divide and variable shift call `lib/rt`. The atomics mask interrupts
around a plain read-modify-write -- ISA_A has no compare-and-swap -- so
they need supervisor mode, where bare-metal code runs; in user mode they
trap.

### Object format

ELF32, big-endian, `EM_68K`, RELA, `e_flags` `0x2` (ISA_A). `embld` links
them, applying `R_68K_32`/`16`/`8` and `R_68K_PC32`/`PC16`/`PC8`, refuses
GOT and PLT relocations, a little-endian, FPU or non-ColdFire object by
name, and with `-Tstack ADDR` emits a stub that sets `%sp` and jumps to
the entry.

### Limitations

| Construct | Diagnostic |
|---|---|
| a 16-byte atomic | `the LoongArch64 backend cannot lower a sixteen-byte atomic (the LA64 base ISA has no 128-bit ll/sc or am* instruction) yet (function f) [cas16 w=16 size=16]` |
| `__builtin_frame_address(N)` or `__builtin_return_address(N)` with N above 0 (level 0 is supported; see [Extensions](extensions.md)) | `__builtin_return_address(1) is not supported on loongarch64-unknown-elf: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found` |
| `__attribute__((interrupt))` | `__attribute__((interrupt)) is not supported: ...` (write the exception entry in a `.S` file or a naked function) |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions`, C++ without `-fno-exceptions` | `unwind tables are not supported for loongarch64-unknown-elf yet (...): EmbCC writes no LoongArch .eh_frame` |
| `__builtin_frame_address(N)` or `__builtin_return_address(N)` with N above 0 (level 0 is supported; see [Extensions](extensions.md)) | `__builtin_return_address(1) is not supported on tricore-none-elf: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found` |
| `__int128` | `__int128 does not exist on this target ...` |
| `__attribute__((interrupt))` | `__attribute__((...)) is not supported: ...` |
| a conditional branch or `loop` to a symbol defined elsewhere (`.s`, `.S`, file-scope `asm`) | `a conditional branch or loop reaches only a label of its own section: its 15-bit displacement (R_TRICORE_15REL) is not one embld applies; branch over a j` |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for tricore-none-elf yet ...` |
| C++ with exceptions (on by default) | `C++ exceptions are not supported for tricore-none-elf yet: EmbCC writes no TriCore .eh_frame; compile with -fno-exceptions`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |
| `__builtin_frame_address(N)` or `__builtin_return_address(N)` with N above 0 (level 0 is supported; see [Extensions](extensions.md)) | `__builtin_return_address(1) is not supported on xtensa-none-elf: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found` |
| `call0` or `callx0` in inline asm | `call0 in Xtensa asm writes a0, which holds this function's return address under the windowed ABI: call a windowed function with call8/callx8` |
| a density (`.n`) instruction | `ret.n is a 16-bit instruction of the density option, which this assembler does not emit: write ret, its 24-bit form` |
| `.begin longcalls` | `.begin longcalls is not supported: a long call is an l32r and a callx, which this assembler does not make of a call; write them` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `__attribute__((interrupt))` | `__attribute__((interrupt)) is not supported: ...` |
| `-S` | `-S is not supported for xtensa-none-elf yet: compile with -c (there is no Xtensa assembler here to check the text against)` |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for xtensa-none-elf yet (...): EmbCC writes no Xtensa .eh_frame` |
| C++ with exceptions (on by default) | `C++ exceptions are not supported for xtensa-none-elf yet: EmbCC writes no Xtensa .eh_frame; compile with -fno-exceptions`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |
| `__builtin_frame_address(N)` or `__builtin_return_address(N)` with N above 0 (level 0 is supported; see [Extensions](extensions.md)) | `__builtin_return_address(1) is not supported on sparc-none-elf: code for this target keeps no frame-pointer chain, so only level 0 (this function's own frame) can be found` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `__attribute__((interrupt))` | `__attribute__((interrupt)) is not supported: ...` |
| a floating-point, coprocessor or V9 instruction in asm | `asm instruction "faddd" is a floating-point instruction: EmbCC compiles soft float, and this assembler has no floating-point vocabulary` / `asm instruction "ldx" is SPARC V9's, and the LEON3 is a V8` |
| `%sp`, `%fp` or `%i7` in an asm's clobber list | `SPARC asm clobbers '%sp', which holds this function's stack pointer; EmbCC does not save it around an asm` |
| a V9 relocation operator (`%hh`, `%lm`, `%gdop_*`...) | `this relocation operator is SPARC V9's or position-independent code's; EmbLD applies %hi/%lo (R_SPARC_HI22/LO10), call and branch displacements and data words` |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for sparc-none-elf yet (...): EmbCC writes no SPARC .eh_frame` |
| C++ with exceptions (on by default) | `C++ exceptions are not supported for sparc-none-elf yet: EmbCC writes no SPARC .eh_frame; compile with -fno-exceptions`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |
| a rotate, `dbcc`, `exg`, `cas`, BCD and the FPU's instructions in asm | `rol.l: ColdFire has no rotate` (and so on, by name) |
| ISA_B's `mvs`, `mvz`, `mov3q`, `sats`, and a 32-bit branch (`bra.l`) | `mvs.b is an ISA_B instruction, which the MCF5208 (ISA_A+) does not have: it traps as illegal there` |
| a frame beyond 32 KiB | `a stack frame larger than 32 KiB` |
| unwind tables | `unwind tables are not supported for m68k-none-elf yet` |
| C++ with exceptions (on by default) | `C++ exceptions are not supported for m68k-none-elf yet: EmbCC writes no ColdFire .eh_frame; compile with -fno-exceptions`. C++ itself compiles with `-fno-exceptions`; see [C++](cxx.md#targets) |

## AVR

### Triples

| Triple | Accepted aliases |
|---|---|
| `avr` | `avr-none-elf`, `avr-elf`, `avr-unknown-none` |

The code is for the ATmega328P's core (the AVR5 architecture, as on
the Arduino Uno), and without `-mmcu=` the part is the ATmega328P.

### Options

| Option | What it does |
|---|---|
| `-mmcu=atmega328p`, `atmega328`, `atmega168p`, `atmega168` | Selects the part: its predefined macro (`__AVR_ATmega168__` and so on) replaces `__AVR_ATmega328P__`. These four are the avr5 parts of the ATmega328P's datasheet family; they differ only in their memories. |
| `-mmcu=atmega48`, `atmega88` and their `a`, `p`, `pa` forms | Refused: `-mmcu=atmega88 is not supported: the atmega88 is an avr4 part, with no jmp or call instruction, and EmbCC's AVR code calls with `call` ...` |
| `-mmcu=` any other part | Refused: `-mmcu=atmega2560 is not supported: EmbCC's AVR backend generates code for the ATmega328P's avr5 core, and knows the memories and vectors of atmega328p, atmega328, atmega168p and atmega168 alone` |
| `-mrelax`, `-mno-relax` | Accepted, and declined: EmbLD does not shorten `call` and `jmp` to `rcall` and `rjmp`, so the image is the same program, larger than avr-gcc's relaxed one. |
| `-mcall-prologues`, `-mno-call-prologues` | Accepted, and declined: every prologue and epilogue is written in its function rather than shared through `__prologue_saves__`. The calling convention is the same either way. |
| `-mdouble=32`, `-mlong-double=32` | Accepted: what EmbCC does. |
| `-mdouble=64`, `-mlong-double=64` | Refused: `-mdouble=64 is not supported: double and long double are binary32 on this target, the same type as float, and a 64-bit double changes the ABI of every one` |
| `-mint8` | Refused: `-mint8 is not supported: int is 16 bits on this target, ...` |

| Part | Flash | SRAM | EEPROM | Vectors |
|---|---|---|---|---|
| ATmega328P, ATmega328 | 32 KB | 2 KB, `0x0100`-`0x08FF` | 1 KB | 26 |
| ATmega168P, ATmega168 | 16 KB | 1 KB, `0x0100`-`0x04FF` | 512 bytes | 26 |

`-mmcu=` on another target is refused: `-mmcu=atmega328p is an AVR
option, and the target is x86_64-elf (--target=avr)`.

### Data model

`int` is 16 bits, pointers are 16 bits, and `double` and `long double`
are the same 4-byte IEEE binary32 format as `float`, which is avr-gcc's
default. Nothing is aligned: `_Alignof` of every type is 1. Plain `char`
is signed, matching `clang --target=avr`.

`int` and `unsigned int` arithmetic is carried out modulo 2^16 inside an
expression, not only when the result is stored: `(0xffffu + 1) / 2` is 0,
and `long f(int x) { return x + 1; }` returns -32768 for 32767.

Program memory is a separate, word-addressed space. A function pointer
holds the function's word address (half its byte address), formed with
the `_GS` relocations (`R_AVR_LO8_LDI_GS`, `R_AVR_HI8_LDI_GS`) or, in
data, `R_AVR_16_PM`. String literals and `const` data are placed in RAM
and copied there from flash by the startup code, as with avr-gcc.

### Calling convention: avr-gcc

- Arguments are allocated from `r25` downward to `r8`. Each argument
  occupies an even number of registers (its size rounded up to even),
  with its low byte in the lowest register of the run: the first `char`
  or `int` argument is in `r24` (`r24:r25`), a first `long` in
  `r22`–`r25`. An argument that does not fit above `r8` goes on the
  stack, and so does every later one.
- Stack arguments are packed at their natural size, with no padding.
- A call to a variadic function passes every argument on the stack,
  named ones included. `va_list` is a 2-byte pointer, and `va_arg` of a
  structure reads its bytes from the packed stack arguments.
- A result is returned in registers with its size rounded up to a power
  of two: 1–2 bytes in `r24`(:`r25`), 3–4 bytes in `r22`–`r25`, 5–8
  bytes in `r18`–`r25`. A composite larger than 8 bytes is written
  through a hidden pointer passed as the first argument.
- `r1` is always zero and `r0` is a scratch register. `r2`–`r17` and
  `r28`–`r29` are callee-saved; `Y` (`r28:r29`) is the frame pointer.

clang's AVR target places structure arguments differently (first field
in the highest registers); EmbCC follows avr-gcc and avr-libc's
documentation, not clang, for composites.

### Object format

ELF32, `EM_AVR`, `e_flags` `EF_AVR_ARCH_AVR5`, `RELA` relocations. Calls
and long jumps are `call` and `jmp` with `R_AVR_CALL`; addresses are
loaded a byte at a time (`R_AVR_LO8_LDI`, `R_AVR_HI8_LDI`).

### Predefined macros

From `clang --target=avr -mmcu=atmega328p`: `__AVR__`, `__AVR`, `AVR`,
`__AVR_ATmega328P__`, `__AVR_ARCH__` (5), `__AVR_HAVE_MUL__`,
`__AVR_HAVE_MOVW__`, `__AVR_HAVE_LPMX__`, `__AVR_HAVE_JMP_CALL__`,
`__AVR_2_BYTE_PC__`, `__SIZEOF_INT__` (2), `__SIZEOF_POINTER__` (2),
`__SIZEOF_DOUBLE__` (4). With `-mmcu=` for another part, the part's
macro (`__AVR_ATmega328__`, `__AVR_ATmega168P__`, `__AVR_ATmega168__`)
stands in place of `__AVR_ATmega328P__`, as clang's table for that part
does; nothing else in the table changes.

The table also defines `__flash`, as
`__attribute__((__address_space__(1)))`, which EmbCC implements: `const`
data kept in program memory and read with `lpm` (see
[Data in program memory](embedded.md#data-in-program-memory)). It also
defines `__BUILTIN_AVR_CLI`, `__BUILTIN_AVR_SEI`, `__BUILTIN_AVR_NOP`,
`__BUILTIN_AVR_SLEEP`, `__BUILTIN_AVR_SWAP` and `__BUILTIN_AVR_WDR`, but
the `__builtin_avr_*` functions are undeclared. Use inline assembly
(`cli`, `sei`, `sleep`, `wdr`, `swap`) instead.

### Headers

`<avr/io.h>`, `<avr/interrupt.h>`, `<avr/pgmspace.h>`, `<avr/wdt.h>`,
`<avr/sleep.h>`, `<avr/eeprom.h>`, `<avr/cpufunc.h>`, `<avr/common.h>`,
`<avr/sfr_defs.h>` and `<util/delay.h>`, with avr-libc's interface, are
on the include path for this target alone; see
[avr-libc-compatible headers](embedded.md#avr-libc-compatible-headers).

### Interrupt handlers

`__attribute__((signal))` and `__attribute__((interrupt))` make a
function an interrupt handler (both together are `interrupt`, as with
avr-gcc: avr-libc's `ISR_NOBLOCK` writes them so); it saves `SREG`, `r0`, `r1` and every
register the backend uses, clears `r1`, and returns with `reti`. See
[Embedded programming](embedded.md).

### Runtime

Integer multiplication and division are helper calls with libgcc's names
(`__mulsi3`, `__divsi3`, `__umodsi3`, `__muldi3`, `__divdi3`, ...), from
`lib/rt/avr.c` and `avr64.c`; floating-point arithmetic calls the
binary32 helpers (`__addsf3`, ...) in `lib/rt/avrfp*.c`. `make
rt-embedded` builds `librt.a` for `avr`, and for each `-mmcu=` part its
startup `crt<part>.o` and linker script `<part>.ld`, which the driver
links under `-mmcu=`, and the AVR library, `libc.a` from `lib/avr`
([The AVR library](embedded.md#the-avr-library)) (see
[Startup and the interrupt vector table](embedded.md#startup-and-the-interrupt-vector-table)).

### Debugging

`-g` writes DWARF 4 that `llvm-dwarfdump --verify` accepts and that gdb
(`set architecture avr`) and [EmbDBG](tools/embdbg.md) read, at `-O0`
and above:

- **Addresses are 4 bytes**, as avr-gcc writes them, although a pointer
  is 2. A code address is a byte address in flash (the address in the
  ELF symbol table, twice the word address the core counts in). A data
  address is `0x800000` plus the SRAM address: data memory is a separate
  space that also starts at 0, and GDB and QEMU's gdb stub put it at
  `0x800000` up. A global at SRAM `0x13a` has `DW_OP_addr 0x80013a`.
- **The frame base is `Y`**, `DW_OP_breg28 0`: the prologue points
  `r28:r29` one byte below the frame, and every variable's
  `DW_OP_fbreg` offset is the displacement the code uses for it
  (`std Y+1, r24`). A debugger reads register 28 as the pair. `Y` is
  set by the prologue, so locations are right from the second line-table
  row of a function on, which is where a breakpoint on the function
  stops.
- **Line table.** Each function's first row is its entry, on the line
  the function is declared; the body's rows follow wherever the source
  line changes.
- **Optimized code.** At `-O1` and above, a variable the optimizer took
  out of memory has an empty location, which a debugger shows as
  `<optimized out>`; so does a parameter that is assigned after it was
  taken out of memory. A parameter that is never assigned keeps its
  slot, which the prologue writes.
- **Globals** the unit defines are described with their type.

`-g` turns off this backend's register allocation at `-O1` and above, so
that every variable stays in its slot; `-O2 -g` code can be about twice
the size of `-O2` code. There is no `.debug_frame`: gdb finds a caller by
analyzing the prologue, and EmbDBG's backtrace stops at frame `#0`.

QEMU's arduino-uno has a gdb stub:

```sh
qemu-system-avr -M uno -nographic -bios fw.elf -S -gdb tcp::1234 &
embdbg fw.elf remote 1234
```

The stub's register layout, which EmbDBG uses: `r0`–`r31` one byte each,
`SREG` one byte, `SP` two bytes, and `PC` four bytes, a byte address.

### Limitations

| Construct | Diagnostic |
|---|---|
| an interrupt handler with parameters | `the AVR backend cannot lower an interrupt handler with parameters: the hardware calls it, so there is no caller to pass them and they would be read out of whatever the interrupted code left in those registers yet (function __vector_3)` |
| an interrupt handler that returns a value | ``the AVR backend cannot lower an interrupt handler that returns a value: `reti` goes back to the interrupted instruction, and nothing is there to receive it yet (function __vector_3)`` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| any C++ translation unit, except with `-fsyntax-only`, `-E`, `-M` or `-MM` | `C++ is not yet supported for avr: the C++ lowering assumes a 32-bit int and 4-byte pointers (integral promotions, member pointers, RTTI), and this target's are 16 bits` |

Code compiled at `-O0` is large; an ordinary program may not fit the
part's 32 KB of flash unless built with `-O1` or above.

<!-- UNVERIFIED: the -O0 size remark comes from a commit message ("-O0 does not fit the part" for one test), not from a measurement made for this page. -->

## Running programs under QEMU

EmbCC's test suites run every target's programs on an emulated board.
The harnesses in `tests/harness/` show a complete startup, I/O and link
for each; the machines are:

| Target | Emulator and machine | How the run ends |
|---|---|---|
| `x86_64-elf` | `qemu-system-x86_64 -cpu max`, a Multiboot image, output on `-debugcon stdio` | The guest prints an exit marker; `isa-debug-exit` stops QEMU |
| `aarch64-elf` | `qemu-system-aarch64 -M virt -cpu cortex-a72 -semihosting` | Semihosting exit carries the status |
| `x86_64-linux-gnu`, `aarch64-linux-gnu` | A real Linux kernel booted with the program as PID 1 (`-M q35` or `-M virt`) | The kernel's panic message carries the exit status |
| `thumbv6m-none-eabi` | `qemu-system-arm -M microbit` (nRF51822, Cortex-M0), UART output | The program prints `==EXIT n==`; `qrun.sh --until` stops QEMU |
| `thumbv7m-none-eabi` | `qemu-system-arm -M lm3s6965evb -cpu cortex-m3`, UART output | The program prints a sentinel; the run is bounded by a timeout |
| `thumbv7em-none-eabihf` | `qemu-system-arm -M mps2-an386 -cpu cortex-m4` | As above |
| `thumbv8m.main-none-eabi[hf]` | `qemu-system-arm -M mps2-an505 -cpu cortex-m33` | As above |
| `riscv32-unknown-elf`, `riscv64-unknown-elf` | `qemu-system-riscv32` / `qemu-system-riscv64 -M virt -bios none -m 8` | The startup writes the SiFive test device after `main` returns |
| `avr` | `qemu-system-avr -M uno`, the image passed with `-bios`; `-S -gdb tcp::PORT` for a debugger | The program prints a sentinel; `qrun.sh --until` stops QEMU when it appears |
| `tricore-none-elf` | `qemu-system-tricore -M tricore_testboard -cpu tc27x`, output through a TCG plugin | The program prints `==EXIT n==` and writes n to the board's test device |
| `sparc-none-elf` | `qemu-system-sparc -M leon3_generic`, the image passed with `-kernel`, APBUART output | The program prints `==EXIT n==` and executes `ta 0` with traps disabled, which QEMU's LEON3 takes as a shutdown |

`-bios none` matters on RISC-V: without it QEMU runs OpenSBI first and
enters the image in supervisor mode. On AVR, QEMU refuses an image whose
ELF entry point is not 0.
