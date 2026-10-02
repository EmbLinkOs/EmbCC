# Overview

This page describes what EmbCC is: the programs that make up the
toolchain and how they fit together, the targets it generates code for,
and the language levels it accepts. It is for anyone meeting EmbCC for
the first time, and it points to the page that covers each subject in
full. To build EmbCC and compile a first program, go on to
[Getting started](getting-started.md).

## What EmbCC is

EmbCC is a compiler for C and C++ together with its own preprocessor,
assemblers, linker, debugger and supporting tools. It produces code for:

- EmbLinkOS on x86-64, the primary target, and bare-metal x86-64;
- AArch64, bare metal and EmbLinkOS;
- ARM Cortex-M microcontrollers (ARMv7-M, ARMv7E-M and ARMv8-M Mainline,
  in Thumb state);
- RISC-V microcontrollers, RV32 and RV64;
- the AVR ATmega328P;
- Linux on x86-64 and AArch64, macOS (Mach-O) and Windows (COFF), with
  the limits listed under [Targets at a glance](#targets-at-a-glance).

EmbCC runs on macOS and Linux. It is written in C99, and it compiles its
own sources.

One `embcc` binary contains every code generator. `--target=TRIPLE`
selects the target for one invocation; without it, EmbCC compiles for
`x86_64-elf`, unless the compiler was built with
`make DEFAULT_TARGET=TRIPLE` or the environment sets
`EMBCC_DEFAULT_TARGET`. See [Targets](targets.md#selecting-a-target).

`embcc` starts no other program. Preprocessing, compiling, assembling
and, where the driver links, linking all run inside the one `embcc`
process, so compiling needs no assembler or linker from another
toolchain on the host.

`embcc --version` prints the version and the default target on its first
line:

```text
EmbCC 1.0.0-m2.complete — C compiler for EmbLinkOS, target x86_64-elf
```

## Components

| Program | What it does | Reference |
|---|---|---|
| `embcc` | The compiler driver. Preprocesses, compiles C and C++, assembles `.s`, `.S` and `.asm` files, writes ELF, Mach-O or COFF objects with DWARF debug information, and links x86-64 ELF programs with the linker built into it. | [Invoking EmbCC](invoking.md) |
| `embas` | The standalone assembler for x86-64 in NASM (Intel) syntax. Writes ELF64 objects. `embcc -c FILE.asm` runs the same assembler. | [embas](tools/embas.md) |
| `embld` | The static linker. Links ELF objects and archives for x86-64, Cortex-M, RV32, RV64 and AVR into an ELF executable, or, with `--embx`, into an EMBX image for EmbLinkOS. `embld --doctor` explains why a link fails. | [embld](tools/embld.md) |
| `embread` | Prints an EMBX image and checks it against the rules the EmbLinkOS loader applies. | [embread](tools/embread.md) |
| `embdbg` | The debugger. Symbolizes addresses, lists functions, lines and variables, analyzes crash reports, disassembles, and debugs a running target through the GDB remote protocol. Reads DWARF and EmbCC's `.embdbg` files; does not need `gdb`. | [embdbg](tools/embdbg.md) |
| `embls` | A language server (LSP over standard input and output). Diagnostics come from running `embcc`; completion, hover, go-to-definition and document symbols come from EmbCC's own preprocessor and parser. | [embls](tools/embls.md) |
| `embidx` | A cross-unit index built from `embcc --emit-interfaces`. Its commands are `build`, `stale` (which units must be rebuilt), `check` (declarations that two units saw differently) and `who` (where a declaration is defined and used). | [embidx](tools/embidx.md) |
| EmbBuild manifests | `build.ebm` at the top of the source tree is the EmbBuild manifest that builds EmbCC on EmbLinkOS. `tools/gen-embbuild-manifest.sh` generates it; `tools/embbuild-run.sh` executes a manifest on the host. | [EmbBuild manifests](tools/embbuild.md) |

`make all` builds `embcc`, `embread`, `embld`, `embas`, `embls` and
`embidx`. `embdbg` is a separate target, `make embdbg`. See
[Building from source](getting-started.md#building-from-source).

EmbCC also provides the libraries a program links against:

| Directory | Contents |
|---|---|
| `include/` | EmbCC's freestanding headers, among them `<stddef.h>`, `<stdarg.h>`, `<stdint.h>`, `<stdbool.h>`, `<limits.h>` and `<float.h>`. |
| `lib/libc` | EmbCC's C library, for EmbLinkOS, bare-metal x86-64 and AArch64, and static Linux programs. |
| `lib/libcxx` | EmbCC's C++ runtime and standard library. |
| `lib/rt` | The compiler runtime: the routines generated code calls for operations the target has no instruction for, such as soft-float arithmetic and wide division. |

The compiler finds its headers and libraries relative to its own
location, in a build tree or an installation; `embcc --print-search-dirs`
shows what it found. See [Libraries](libraries.md).

## How the pieces fit together

### Inside `embcc`

`embcc` compiles exactly one input file per invocation. What it does with
the file depends on the file name suffix (see
[Input files](invoking.md#input-files)):

```text
.c                      preprocessor -> parser -> semantic analysis
.cc .cpp .cxx .C ...    preprocessor -> C++ front end, which lowers the
                        unit to C -> parser -> semantic analysis
                                 |
                                 v
                        EmbIR -> optimizer -> code generator for the
                        target -> ELF, Mach-O or COFF object (+ DWARF)

.s .S                   GNU-syntax assembler (.S is preprocessed first);
                        AArch64, Cortex-M, RISC-V and AVR targets
.asm                    NASM-syntax assembler shared with embas; x86-64
```

Without `-c`, `-S` or `-E`, `embcc` compiles the file and then links it
in the same process, adding the target's start-up file and libraries.
The driver links only for the x86-64 ELF targets (`x86_64-elf`,
`x86_64-emblink`, `x86_64-linux-gnu`). For every other target it stops
with `embcc: error: cannot link for TRIPLE` and the program is linked
separately: by `embld` for Cortex-M, RISC-V and AVR, and by another
toolchain's linker for AArch64, macOS and Windows.

`embcc inspect STAGE FILE` prints one stage of this pipeline (tokens,
preprocessed source, syntax tree, symbols, struct layout, EmbIR, control
flow graph or call graph) instead of producing an object, and
`embcc why` reports an optimizer decision. See
[Developer and inspection options](invoking.md#developer-and-inspection-options).
The design of each stage is described in [Architecture](../internals/architecture.md),
[Front end](../internals/front-end.md), [EmbIR](../internals/ir.md),
[Optimizer](../internals/optimizer.md) and
[Back ends](../internals/backends.md).

### Between the programs

```text
source files --embcc -c--> objects (.o) --+
.asm files ----embas-----> objects (.o) --+
                                          |
                    archives (.a) --------+--> embld --> ELF executable
                                                 |         + FILE.embdbg when an
                                                 |           object has -g info
                                                 |
                                                 +--> --embx: EMBX image
                                                              |
                                                              v
                                                           embread

ELF executable, FILE.embdbg, crash report --> embdbg
editor <--LSP--> embls --runs--> embcc -fsyntax-only
embidx --runs--> embcc --emit-interfaces --> index file
```

A firmware image for a Cortex-M part, for example, is built by compiling
each file with `embcc -c` and linking the objects with `embld`:

```sh
embcc --target=thumbv7m-none-eabi -c boot.c -o boot.o
embcc --target=thumbv7m-none-eabi -O2 -g -c main.c -o main.o
embld -e reset -Ttext 0x0 -Tdata 0x20000000 boot.o main.o -o fw.elf
```

Because `main.o` carries debug information, `embld` also writes
`fw.elf.embdbg`, which `embdbg` reads. Start-up code, linker addresses
and interrupt handlers are covered in
[Embedded programming](embedded.md); debugging in
[Debugging](debugging.md).

## Targets at a glance

Every target is little-endian. The triples listed are the canonical
spellings; [Targets](targets.md) lists the accepted aliases and every
detail of each target's ABI, options and predefined macros.

| Target | Canonical triples | Object format | Data model | Status |
|---|---|---|---|---|
| x86-64, bare metal and EmbLinkOS | `x86_64-elf`, `x86_64-emblink` | ELF64 | LP64 | Primary target. `embcc` compiles and links; `embld --embx` produces EmbLinkOS images. |
| x86-64 Linux | `x86_64-linux-gnu` | ELF64 | LP64 | Static executables on EmbCC's own C library, linked by `embcc`. |
| AArch64, bare metal, EmbLinkOS and Linux | `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` | ELF64 | LP64 | Compiles to objects; link with another toolchain's linker. |
| ARM Cortex-M | `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` | ELF32 | ILP32 | Bare metal; linked by `embld`. Soft float; on ARMv7E-M and ARMv8-M, optionally the single-precision FPU. |
| RISC-V RV32 | `riscv32-unknown-elf` | ELF32 | ILP32 | Bare metal, RV32IMAC; linked by `embld`. |
| RISC-V RV64 | `riscv64-unknown-elf` | ELF64 | LP64 | Bare metal, RV64IMAC; linked by `embld`. |
| AVR | `avr` | ELF32 | 16-bit `int` and pointers, 32-bit `long`, 4-byte `double` | Bare metal, ATmega328P; linked by `embld`. |
| macOS | `x86_64-apple-darwin`, `aarch64-apple-darwin` | Mach-O | LP64 | Objects for the system linker. `-g` and `__thread` are refused. |
| Windows | `x86_64-windows-gnu` | COFF | LP64 | Objects only, not yet the Microsoft x64 ABI; every compile warns (`-Wwindows-abi`). No C library. |

`x86_64-elf` and `x86_64-emblink` produce the same code and differ only
in their predefined macros: `x86_64-emblink` defines `__emblink__`,
`__emblink` and `__EmbLinkOS__`. The data models differ in more than the
sizes of `long` and pointers: plain `char` is unsigned on AArch64,
Cortex-M and RISC-V, and `long double` has a different format on almost
every target. The full table is in [Data models](targets.md#data-models).
Windows keeps `long` at 8 bytes, where the Microsoft ABI has 4.

An unknown triple is an error that lists every triple EmbCC accepts:

```text
embcc: error: unknown target 'armv6m-none-eabi'
embcc: the targets it emits for are:
embcc:   x86_64-elf
...
```

## Language levels at a glance

| Language | What EmbCC parses | `-std=` | Predefined level |
|---|---|---|---|
| C | One dialect: C11 with the GNU extensions, plus C23 features such as `nullptr`, `[[...]]` attributes, `auto` type inference, enumerations with a fixed underlying type, and `#embed` | Every C standard name from `c89` to `gnu23` is accepted and changes nothing; one older than C11 draws a warning | `__STDC_VERSION__` is `201710L` |
| C++ | C++20 with GNU extensions and some C++23 features, compiled by lowering it to C | `c++98` to `c++26` and the `gnu++` forms set `__cplusplus` and the feature-test macros; the parser still reads C++20 | `__cplusplus` is `202002L` by default (`-std=gnu++20`) |

`-std=` never makes EmbCC reject a construct of a newer standard. The
exact list of accepted names is under
[`-std=STANDARD`](invoking.md#-stdstandard).

C is supported on every target. C++ is supported on the x86-64 and
AArch64 targets, with restrictions on macOS and Windows. On the
Cortex-M, RISC-V and AVR targets C++ is not supported: EmbCC compiles a
C++ unit there without a diagnostic, but its C++ front end does not use
the target's data model (on `thumbv7m-none-eabi`, `sizeof(long)` is 8 in
C++), and there is no C++ library for those targets.

The language pages give the details:

- [C language](c-language.md): the C standards, feature by feature.
- [Implementation-defined behavior](implementation-defined.md): EmbCC's
  choices, in the order of C17 Annex J.3.
- [Extensions](extensions.md): GNU and Clang extensions, attributes,
  builtins and pragmas.
- [Inline assembly](inline-asm.md): extended `asm`, constraints and
  operand modifiers for each target.
- [C++ support](cxx.md): each C++ feature, the C++ ABI, and the C++
  run-time libraries.

## Where to go next

For people compiling code with EmbCC:

- [Getting started](getting-started.md): building and installing EmbCC,
  a first program, and cross-compiling for each board.
- [Invoking EmbCC](invoking.md): every command-line option.
- [Targets](targets.md): each target's triples, options, ABI, predefined
  macros and limitations.
- [Embedded programming](embedded.md): start-up code, interrupts,
  memory-mapped I/O and linking firmware.
- [Libraries](libraries.md): the C library, the C++ library and the
  compiler runtime.
- [Diagnostics](diagnostics.md): the message format, warnings,
  `--explain`, JSON output and fix-its.
- [Optimization](optimization.md): the `-O` levels and the `-f` options.
- [Debugging](debugging.md): `-g`, the DWARF EmbCC emits, and the
  debuggers that read it.
- The tool reference pages: [embas](tools/embas.md),
  [embld](tools/embld.md), [embread](tools/embread.md),
  [embdbg](tools/embdbg.md), [embls](tools/embls.md),
  [embidx](tools/embidx.md) and [EmbBuild manifests](tools/embbuild.md).

For people changing EmbCC, start with
[Architecture](../internals/architecture.md), then
[Contributing](../internals/contributing.md) and
[Testing](../internals/testing.md). [Decisions](../internals/decisions.md)
records the design decisions, and [Status](../internals/status.md) what
is complete and what is not.
