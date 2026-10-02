# EmbCC documentation

EmbCC is a C and C++ compiler with its own assembler, linker and debugger. It
targets EmbLinkOS on x86-64, Linux, macOS, Windows, and bare-metal AArch64,
ARMv7-M, RISC-V (RV32 and RV64) and AVR. This documentation has two parts:
the manual is for people who compile code with EmbCC, and the internals
reference is for people who change it.

## Using EmbCC

Start with the [overview](manual/overview.md) and
[getting started](manual/getting-started.md).

| Page | Covers |
| --- | --- |
| [Overview](manual/overview.md) | What EmbCC is, its components, the targets at a glance |
| [Getting started](manual/getting-started.md) | Building and installing, first programs, cross builds |
| [Invoking EmbCC](manual/invoking.md) | Every command-line option |
| [Targets](manual/targets.md) | Triples, `-m` options, data models, ABIs, predefined macros, object formats |
| [C language support](manual/c-language.md) | `-std=`, and the status of each C89–C23 feature |
| [Implementation-defined behavior](manual/implementation-defined.md) | EmbCC's choices, in C17 Annex J.3 order |
| [Language extensions](manual/extensions.md) | GNU and Clang extensions, attributes, builtins, pragmas |
| [Inline assembly](manual/inline-asm.md) | Extended asm, and the constraints and modifiers of each target |
| [C++ support](manual/cxx.md) | What the C++ front end accepts |
| [Diagnostics](manual/diagnostics.md) | The diagnostic format, warnings, `--explain`, JSON output |
| [Optimization](manual/optimization.md) | `-O` levels and `-f` options |
| [Debugging](manual/debugging.md) | `-g`, DWARF, and debugging programs |
| [Bare-metal programming](manual/embedded.md) | Freestanding programs on the boards |
| [Libraries](manual/libraries.md) | The C library, the C++ library, the compiler runtime |

### Tools

| Tool | Page |
| --- | --- |
| `embld` | [The linker](manual/tools/embld.md) |
| `embas` | [The NASM-syntax assembler](manual/tools/embas.md) |
| `embdbg` | [The debugger](manual/tools/embdbg.md) |
| `embread` | [The EMBX image reader](manual/tools/embread.md) |
| `embls` | [The language server](manual/tools/embls.md) |
| `embidx` | [The cross-unit interface index](manual/tools/embidx.md) |
| `.ebm` manifests | [EmbBuild manifests](manual/tools/embbuild.md) |

## EmbCC internals

Start with the [architecture](internals/architecture.md) and the
[contributing guide](internals/contributing.md).

| Page | Covers |
| --- | --- |
| [Architecture](internals/architecture.md) | The pipeline from command line to object file, and the source tree |
| [Front end](internals/front-end.md) | Preprocessor, lexer, parser and semantic analysis |
| [EmbIR](internals/ir.md) | The intermediate representation and its text form |
| [Optimizer](internals/optimizer.md) | The passes, their order, and what each may assume |
| [Register allocation](internals/register-allocation.md) | The shared allocator and each target's use of it |
| [Back ends](internals/backends.md) | Code generation for each target |
| [Object files](internals/object-formats.md) | ELF, Mach-O, COFF and EMBX writers |
| [Linker](internals/linker.md) | How `embld` resolves, lays out and relocates |
| [Testing](internals/testing.md) | The test suites, the boards, and how to add a test |
| [Contributing](internals/contributing.md) | Conventions, the review checklist, commit style |
| [Design decisions](internals/decisions.md) | The decision record (D-001 …) |
| [Status](internals/status.md) | Per-target maturity and known limitations |
