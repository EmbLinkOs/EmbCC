# Status and known limitations

This page is a snapshot of what EmbCC supports and what it does not: how
complete each target is, which language features, attributes, builtins
and options it refuses (with the exact diagnostic for each), where its
calling conventions differ from the platform's, which defects are known,
which optimizations GCC and Clang perform that EmbCC does not, and how to
report a bug. It is for anyone deciding whether EmbCC can build their
code, and for people changing EmbCC who need the list of open work. The
per-target reference is [Targets](../manual/targets.md); the language
levels are in [C language](../manual/c-language.md) and
[C++ support](../manual/cxx.md).

## Summary

- **x86-64 and AArch64 ELF** (bare metal, EmbLinkOS, Linux) are the
  complete targets: C and C++ with exceptions, the full optimizer,
  register allocation, DWARF debug information, and EmbCC's own C, C++
  and compiler-runtime libraries. Linux programs are static images.
- **Apple arm64** compiles C and C++ for the system linker, and the
  result runs natively. Debug information is refused.
- **Cortex-M, RISC-V and AVR** are bare-metal C targets with EmbCC's
  compiler runtime and linker, and no C library. C++ is not supported
  on them.
- **MIPS32** (`mipsel-none-elf`, MIPS32r2 o32 soft float, a PIC32's
  core) is a bare-metal C target with EmbCC's compiler runtime, C
  library and linker, run on QEMU's malta board. C++ is not supported.
- **LoongArch64** (`loongarch64-unknown-elf`, LA64 LP64S soft float) is
  a bare-metal C target with EmbCC's compiler runtime, C library,
  assembler and linker, run on QEMU's virt board. C++ compiles with
  `-fno-exceptions`.
- **Windows x86-64** produces COFF objects that are not yet compatible
  with the Microsoft x64 ABI. EmbCC warns on every such compile.

## How EmbCC refuses

A construct EmbCC does not implement is an error that names it. EmbCC
does not compile an unsupported construct into code that does something
else. The exceptions to that rule, where something is accepted and
silently has a different effect, are listed under
[Known defects](#known-defects).

A diagnostic has this form:

```text
embcc: FILE:LINE:COL: error: MESSAGE
```

Some diagnostics carry no column, and those about the command line carry
no file (`embcc: error: MESSAGE`). The tables on this page quote
`MESSAGE`. Names such as `f`, `x`, `g` and `'d'` in a quoted message
stand for the function, variable or type in your program.

A backend that cannot lower an operation names itself, the operation and
the IR instruction:

```text
embcc: f.c:3: error: the RV32 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]
```

The backend names are `ARMv7-M` (every Cortex-M target), `RV32`, `RV64`,
`MIPS32`, `LoongArch64` and `AVR`.

An attribute EmbCC does not know is a warning, not an error, and is
ignored (see [Attributes](#attributes)).

## Targets

| Target | Code generation | Optimizer | Register allocation | Debug information | Linking | Run-time libraries |
|---|---|---|---|---|---|---|
| x86-64 ELF: `x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu` | Complete. SSE2 floating point, x87 `long double`, `__int128`. C and C++ with exceptions. | Every pass, including the vectorizer. | Graph colouring; integer and SSE classes. | DWARF 4. | `embcc` links in-process with `embld`. Static images only. EMBX through `embld --embx`. | `libc.a`, `libcxx.a`; `crt1.o` and `librt.a` on Linux. |
| AArch64 ELF: `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` | Complete. FP/SIMD, binary128 `long double` in software, `__int128`. C and C++ with exceptions. | Every pass except vectorization. | Graph colouring; integer and FP/SIMD classes. | DWARF 4. | An external AArch64 linker. `embld` refuses AArch64 objects. | `libc.a`, `libcxx.a`; `crt1.o` and `librt.a` on Linux. |
| Apple arm64: `aarch64-apple-darwin` | AArch64 backend, Apple's arm64 convention, Mach-O. C and C++ with exceptions. | As AArch64. | As AArch64. | Refused. | The system linker. | The system's. EmbCC's C headers do not match it; see [Darwin](#darwin). |
| macOS x86-64: `x86_64-apple-darwin` | x86-64 backend, Mach-O. C++ objects that use exceptions do not link. | As x86-64. | As x86-64. | Refused. | The system linker. | The system's. |
| Windows x86-64: `x86_64-windows-gnu` | x86-64 backend, COFF, part of the Microsoft x64 convention. C++ only without exceptions and unwind tables. | As x86-64. | As x86-64. | Refused. | An external linker. | None. |
| Cortex-M: `thumbv7m-none-eabi`, `thumbv7em-none-eabi[hf]`, `thumbv8m.main-none-eabi[hf]` | Thumb-2 for ARMv7-M, ARMv7E-M and ARMv8-M Mainline. Soft float, or a single-precision FPU with `double` in software. No DSP instructions. C only. | Every pass except vectorization and division by a constant. | Graph colouring; register pairs for 64-bit values; `s16`-`s31` with an FPU. | DWARF 4, with [known problems](../manual/debugging.md#known-problems). | `embld`. | `librt.a`, one per triple. No C library. |
| RV32: `riscv32-unknown-elf` | RV32IMAC, soft float. No operation on `long double`. C only. | Every pass except vectorization and division by a constant. | Graph colouring; register pairs for 64-bit values. | DWARF 4, with known problems. | `embld`. | `librt.a`. No C library. |
| RV64: `riscv64-unknown-elf` | RV64IMAC, soft float. No operation on `long double` or `__int128`. C only. | Every pass except vectorization. | Graph colouring. | DWARF 4, with known problems. | `embld`. | None built (see below). No C library. |
| MIPS32: `mipsel-none-elf` | MIPS32r2, little-endian, o32, soft float. Delay slots filled from the instruction before the transfer where safe, else a `nop`; jump tables. No computed goto or narrow atomics. C only. | Every pass except vectorization. | Graph colouring; register pairs for 64-bit values. | DWARF 4, with known problems. | `embld`, which also links clang's objects. | `librt.a` and `libc.a` (`make rt-embedded libc-embedded`). |
| LoongArch64: `loongarch64-unknown-elf` | LA64 base integer ISA, LP64S soft float, the normal code model; jump tables; `__int128` and binary128 `long double` in software; atomics of every width but 16 bytes. No computed goto. C, and C++ without exceptions. | Every pass except vectorization. | Graph colouring. | DWARF 4, with known problems. | `embld`, which also links clang's objects (its medium code model and GOT accesses included). | `librt.a` and `libc.a` (`make rt-embedded libc-embedded`). |
| AVR: `avr` | ATmega328P (AVR5). 16-bit `int`, 32-bit `double`. No variable-length arrays or computed goto. C only. | Every pass except vectorization and division by a constant; a few more passes do nothing on AVR. | Graph colouring over register runs; each function is generated under several allocation modes and the shortest result kept. Off under `-g`. | Accepted, but not usable by a debugger. | `embld`. | `librt.a`. No C library. |

The register allocator is described in
[Register allocation](register-allocation.md), the passes and their
target dependencies in [The optimizer](optimizer.md#target-dependent-behaviour),
the libraries in [Libraries](../manual/libraries.md), and how each target
is tested (QEMU boards, a Linux kernel, native macOS runs) in
[Testing](testing.md). What the Darwin and Windows targets refuse is
listed under [ABI limitations](#abi-limitations).

### Linking

The driver links x86-64 ELF programs and, for the firmware targets
(ARMv7-M, ARMv8-M, RV32, RV64, MIPS32, LoongArch64, AVR), images whose memory map the build
gives: a linker script (`-T`, ARM and RISC-V) or `-Wl,-Ttext`/`-Tdata`.
A firmware link without one stops with `embcc: error: linking a TRIPLE
image needs its memory map`. Every other target (AArch64 ELF, Mach-O,
COFF) stops with `embcc: error: cannot link for TRIPLE`, because embld
does not read those objects. AArch64 objects are refused by embld with:

```text
embld: FILE: a 64-bit object for machine 183; only x86-64 and RV64 (EM_RISCV) are supported
```

The driver compiles several sources in one command by running itself
once for each (`-j N` at a time), on a host that can run a program
(macOS, Linux). A build with `PROCESS=none` -- EmbLinkOS, which has no
fork/exec -- compiles one source per command and refuses a second:

```text
embcc: error: more than one source file ('a.c' and 'b.c'), and this host cannot run a compiler for each (EmbCC was built with PROCESS=none): compile each with -c and link the objects (embcc a.o b.o -o OUT)
```

EmbCC has no archiver. The library builds use `x86_64-elf-ar`,
`aarch64-elf-ar` or `llvm-ar`.

There is no position-independent code, no shared library and no dynamic
linking. `-fPIC`, `-shared` and the related options are refused (see
[Options](#options)). EmbLD reads GNU ld linker scripts (`embld -T`) for
ARM and RISC-V images, not for x86-64 or AVR; the driver links firmware
with them (`embcc -T board.ld a.o b.o -o fw.elf`). `--gc-sections` drops
the sections nothing reaches, on every machine, for objects built with
`-ffunction-sections -fdata-sections`; `-Map` and
`--print-memory-usage` report the layout as ld does.

### Assembly

| Input or output | x86-64 | AArch64, Cortex-M, RISC-V, MIPS32, AVR |
|---|---|---|
| `.s` and `.S` files (GNU syntax) | Refused: `no assembly-file support for x86_64-elf yet; its instruction encoder exists (inline __asm__ works) but this driver has not been wired to it` | Assembled, with GNU as's directives, macros, conditionals, sections, expressions, literal pools and branch relaxation; ARM's CMSIS and ST's startup files assemble to clang's object ([embas](../manual/tools/embas.md#gnu-syntax-assembly)) |
| `.asm` files (NASM syntax) | Assembled | Assembled as x86-64; see [Known defects](#known-defects) |
| `-S` | `.byte` directives with the disassembly in comments | `.byte` directives without mnemonics; on MIPS a relocated instruction is written symbolically (`jal f`, `lui $2, %hi(g)`), because llvm-mc's MIPS `.reloc` knows none of those relocations |
| File-scope `__asm__` | Labels, `.globl`, `.byte`/`.long`/`.quad` and the instructions `and`, `call`, `jmp`, `ret` | AArch64: labels, `.globl` and data directives, no instruction. Cortex-M, RISC-V, MIPS32 and AVR: the GNU-syntax assembler's whole language, as in a `.S` file, into `.text` |

Any other instruction in a file-scope `asm` block is refused. On x86-64:

```text
file-scope asm instruction not supported: "movq %rdi, %rax" (EmbCC assembles .global/labels/.byte/.long/.quad and and/call/jmp/ret)
```

On AArch64, every instruction is refused, including the four that
x86-64 accepts:

```text
file-scope asm instruction "ret": EmbCC assembles instructions for x86-64 only. On this target write the block as .byte/.long data (see lib/libc/src/setjmp).
```

Inline `asm` inside a function is assembled on every target; its
vocabulary is in [Inline assembly](../manual/inline-asm.md).

On MIPS32 a `.S` file, a file-scope block, a naked function and an
inline-asm template start in `.set reorder` mode, as with GNU as, GCC and
clang: the assembler puts a `nop` in each delay slot unless the code says
`.set noreorder`.

### Thread-local storage

`__thread` and `_Thread_local` use the local-exec model on x86-64 and
AArch64 ELF (`R_X86_64_TPOFF32`, `R_AARCH64_TLSLE_*`). On Cortex-M,
RISC-V, MIPS32 and AVR a thread-local object is placed in `.tbss` or `.tdata`
but addressed with ordinary absolute or PC-relative relocations, so
there is one instance, not one per thread. On Darwin and Windows it is
refused (see [ABI limitations](#abi-limitations)).

### Target-specific refusals

x86-64 and AArch64:

| Construct | Diagnostic |
|---|---|
| A scalar local with `aligned` above 16 | `'x' needs 32-byte alignment and the stack only guarantees 16: supported for an array or a struct, not yet for a scalar` |
| Floating point under `-mno-sse` (x86-64) | `floating point needs SSE, which -mno-sse forbids` |
| Floating point under `-mgeneral-regs-only` (AArch64) | `floating point used under -mgeneral-regs-only (in 'f')` |

Cortex-M (`ARMv7-M` backend):

| Construct | Diagnostic |
|---|---|
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| 8-byte atomic read-modify-write | `the ARMv7-M backend cannot lower this operation at 64 bits yet (function f) [xadd w=8 size=8]` |
| 8-byte atomic load or store | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| Computed `goto` and `&&label` | `the ARMv7-M backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| An `asm` output wider than 4 bytes | `the ARMv7-M backend cannot lower an asm output wider than a register yet (function f) [asm w=4 size=4]` |
| A scalar local with `aligned` above 8 | `'x' needs 32-byte alignment and the stack only guarantees 8: supported for an array or a struct, not yet for a scalar` |

RISC-V (RV32 messages shown; RV64 names itself):

| Construct | Diagnostic |
|---|---|
| Any operation on `long double` | `the RV32 backend cannot lower a 128-bit value yet (function f) [ldvar w=16 size=16]` |
| `__int128` at RV32 | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| Any operation on `__int128` at RV64 | `the RV64 backend cannot lower a 128-bit value yet (function f) [ldvar w=16 size=16]` |
| 8-byte atomic read-modify-write at RV32 | `the RV32 backend cannot lower this operation at 64 bits yet (function f) [xadd w=8 size=8]` |
| 8-byte atomic load or store at RV32 | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| Computed `goto` and `&&label` | `the RV32 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| An `asm` output wider than a register | `the RV32 backend cannot lower an asm output wider than a register yet (function f) [asm w=4 size=4]` |
| A scalar local with `aligned` above 16 | `'x' needs 32-byte alignment and the stack only guarantees 16: supported for an array or a struct, not yet for a scalar` |

`long double` and `__int128` can still be declared and measured with
`sizeof` on RISC-V, and a static initializer such as
`long double g = 1.5L * 2;` is computed at compile time.

MIPS32:

| Construct | Diagnostic |
|---|---|
| Atomic read-modify-write on a 1- or 2-byte object | `the MIPS32 backend cannot lower an atomic narrower than four bytes (ll/sc are word-sized, and a read-modify-write of the containing word is not atomic against its neighbours) yet (function f) [xadd w=4 size=1]` |
| 8-byte atomic read-modify-write | `the MIPS32 backend cannot lower an atomic wider than a register yet (function f) [xadd w=8 size=8]` |
| 8-byte atomic load or store | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| Computed `goto` and `&&label` | `the MIPS32 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| `__builtin_frame_address`, `__builtin_return_address` | `the MIPS32 backend cannot lower __builtin_frame_address or __builtin_return_address (o32 code keeps no frame-pointer chain) yet (function f) [frameaddr w=8 size=4]` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| A scalar local with `aligned` above 8 | `'x' needs 16-byte alignment and the stack only guarantees 8: supported for an array or a struct, not yet for a scalar` |
| `-mhard-float`, `-EB`, `-mabicalls`, `-mabi=n32`, `-G8`, a core that is not MIPS32r2 | each refused by name; see [Invoking EmbCC](../manual/invoking.md#mips-options) |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for mipsel-none-elf yet (-funwind-tables, -fasynchronous-unwind-tables, -fexceptions): EmbCC writes no MIPS .eh_frame` |

LoongArch64:

| Construct | Diagnostic |
|---|---|
| A 16-byte atomic | `the LoongArch64 backend cannot lower a sixteen-byte atomic (the LA64 base ISA has no 128-bit ll/sc or am* instruction) yet (function f) [cas16 w=16 size=16]` |
| Computed `goto` and `&&label` | `the LoongArch64 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| `__builtin_frame_address`, `__builtin_return_address` | `the LoongArch64 backend cannot lower __builtin_frame_address or __builtin_return_address (EmbCC's LoongArch code keeps no frame-pointer chain) yet (function f) [frameaddr w=8 size=4]` |
| A scalar local with `aligned` above 16 | `'x' needs 32-byte alignment and the stack only guarantees 16: supported for an array or a struct, not yet for a scalar` |
| `-mabi=lp64d`, `-mabi=lp64f`, `-mfpu=64`, `-mdouble-float`, `-mcmodel=extreme`, `-mstrict-align`, `-mlsx`, `-mlasx`, an LA32 `-march` | each refused by name; see [Targets](../manual/targets.md#loongarch64) |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions`, C++ without `-fno-exceptions` | `unwind tables are not supported for loongarch64-unknown-elf yet (...): EmbCC writes no LoongArch .eh_frame` |
| In assembly: `la.abs`, the TLS, extreme-model and absolute-GOT operators, floating-point and vector instructions | each refused by name, with the statement |

AVR:

| Construct | Diagnostic |
|---|---|
| An atomic load or store wider than 1 byte | `an atomic access of 2 bytes is not one access on this target (it moves 1 at once): the halves could be split by an interrupt or another core` |
| A variable-length array | `the AVR backend cannot lower a variable-length array yet (function f)` |
| Computed `goto` and `&&label` | `the AVR backend cannot lower labeladdr yet (function f) [labeladdr w=4 size=4]` |
| A local with `__attribute__((aligned))` | `the AVR backend cannot lower a local with __attribute__((aligned)): AVR's stack pointer has no known alignment, so a frame slot cannot be given one yet (function f)` |
| An 8-byte `asm` operand | `an asm operand of 8 bytes needs 8 consecutive registers, which is more than this backend keeps free across an asm` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |

C++ code generation is refused on Cortex-M, RV32, MIPS32 and AVR, and
C++ exceptions at RV64 and on LoongArch64; see [C++](#c).

### Runtime libraries

- EmbCC ships a C library for `x86_64-elf`, `aarch64-elf`, both Linux
  triples and EmbLinkOS, and a C++ library for the x86-64 and AArch64 ELF
  triples. It ships no C library for Cortex-M, RISC-V or AVR.
- The compiler runtime (`librt.a`) is built for both Linux triples
  (`make libc-linux-x86_64`, `make libc-linux-aarch64`) and for every
  Cortex-M triple, `riscv32-unknown-elf`, `mipsel-none-elf`,
  `loongarch64-unknown-elf` and `avr` (`make rt-embedded`). For
  `mipsel-none-elf` and `loongarch64-unknown-elf` the C library is built
  too (`make libc-embedded`), on its bare-metal backend.
  It is not built for `riscv64-unknown-elf`, because `lib/rt/int128.c`
  and `lib/rt/fp128.c` use `__int128`, which the RV64 backend refuses.
  The other `lib/rt` files compile for RV64 one at a time.
- The runtime routines have libgcc's names (`__divdi3`, `__addsf3`,
  `__multi3`, ...). The ARM run-time ABI's `__aeabi_*` routines are not
  provided.
- On Linux, time functions enter the kernel on every call (the vDSO is
  not used), and the file-status functions use `statx`, so they need
  Linux 4.11 or later.

## C language features

EmbCC compiles one C dialect, with the GNU extensions and many C23
features; [C language](../manual/c-language.md) describes what is
supported. This section lists what is not.

### Types

| Construct | Diagnostic |
|---|---|
| `_Float16`, `__fp16` | ``_Float16/__fp16 is not supported: EmbCC has no 16-bit floating-point type, and widening it to `float` would give 24 bits of mantissa where the program asked for 11`` |
| `_Float128`, `__float128` on x86-64 | ``_Float128 is not supported on x86-64: `long double` here is x87's 80-bit extended format, not IEEE binary128, so it is not the same type`` |
| `_Float128` on Cortex-M, AVR and Apple arm64 | `_Float128 is not supported on this target: it has no 128-bit floating-point type` |
| `_BitInt(N)` | `expected a type before '_BitInt'` |
| `_Decimal32`, `_Decimal64`, `_Decimal128` | `expected a type before '_Decimal32'` |
| `_Imaginary` | `expected a type before '_Imaginary'` |
| Integer `_Complex` | `invalid _Complex type (only float, double and long double _Complex are supported)` |
| Integer imaginary constant (`2i`) | `an integer imaginary constant (GNU _Complex int) is not supported — write it as a floating one (2.0i)` |
| `_Atomic` on anything but an integer or a pointer | `reading an _Atomic struct s is not supported: EmbCC makes the operators atomic for integers and pointers only` (the first words name the operation) |
| C23 `constexpr` of a non-integer type | `constexpr 'd' of type double is not supported: EmbCC takes integer constants` |

`_Float128` is accepted where `long double` is binary128 (AArch64 ELF,
RISC-V).

### Declarations, statements and expressions

| Construct | Diagnostic |
|---|---|
| `asm goto` | `` `asm goto` is not supported: its template branches to a label, which needs a patchable placeholder in each backend's inline assembler and CFG edges the optimizer honours. Use a normal asm that sets a value and branch on that `` |
| Nested function definitions | A syntax error: `expected ';' before '{'` |
| K&R (old-style) function definitions | A syntax error: `expected a parameter type before 'a'` |
| Multi-character constants (`'ab'`) | `a character constant holds one character (multi-character constants are not supported)` |
| `$` in identifiers | `character '$' is not supported yet` |
| C23 `u8` character constants (`u8'a'`) | A syntax error |
| C23 storage-class specifiers in a compound literal (`(static int[]){1, 2}`) | `expected an expression, got 'static'` |
| A range designator followed by further designators (`[0 ... 1].a = 5`) | `a range designator [lo ... hi] followed by more designators is not supported` |
| An unsized array whose element count depends on brace elision that only the types can resolve | `cannot size 'g' from its initializer: the braces it leaves out read as 2 elements without the types and 1 with them; brace each element` |

Implicit `int` and implicit function declarations are errors, as C99
requires. Trigraphs are not replaced.

### Preprocessor

| Construct | Diagnostic |
|---|---|
| `#ident`, `#sccs`, `#assert`, `#unassert` | `unknown directive '#ident'` |
| `#embed` parameters (`limit`, `prefix`, `suffix`, `if_empty`) | `#embed parameters (limit, prefix, suffix, if_empty) are not supported; ignoring one would embed the wrong bytes` |
| `#pragma pack(push, NAME, ...)` | `#pragma pack(push, name) is not supported` |
| `#pragma pack(pop, ...)` with a name or count | `#pragma pack(pop, ...) with a name or count is not supported` |
| `#pragma pack` inside a struct body | `#pragma pack inside a struct body is not supported: put it before the struct` |

`#pragma pack` is the only pragma EmbCC acts on, in either the directive
or the `_Pragma` form. Every other pragma is ignored without a
diagnostic, including `#pragma once`; see
[Known defects](#known-defects). The C23 `unreachable()` macro is not
defined by `<stddef.h>`. `__STDC_VERSION__` is `201710L` whatever
`-std=` says.

### Attributes

These attributes would change the generated code in a way EmbCC does not
implement, so they are errors:

| Attribute | Diagnostic |
|---|---|
| `naked` (x86-64 and AArch64; supported on Cortex-M, RISC-V, MIPS32 and AVR) | `__attribute__((naked)) is not supported: on this target the body could only be assembled by the file-scope assembler's few instructions; it is supported on the ARM, RISC-V, MIPS and AVR targets` |
| `interrupt` (except on Cortex-M and AVR) | `__attribute__((interrupt)) is not supported: the handler would return with an ordinary return instead of the interrupt return the CPU needs, and without saving the registers (on ARMv7-M it needs neither, and is accepted; on AVR it is implemented)` |
| `signal` (except on AVR) | `__attribute__((signal)) is not supported: an interrupt handler needs the machine's own return instruction and every register saved, which only the AVR backend does` |
| `cleanup` | `__attribute__((cleanup)) is not supported: the cleanup function would never run` |
| `ms_abi` | `__attribute__((ms_abi)) is not supported: the arguments would be passed in System V's registers` |
| `sysv_abi` | `__attribute__((sysv_abi)) is not supported: the arguments would be passed in the other convention's registers` |
| `vector_size` | `__attribute__((vector_size)) is not supported: the type would stay a scalar: EmbCC's vector IR comes from the auto-vectorizer and only x86-64 lowers it, so a vector TYPE has no representation in the front end or on three of four targets` |
| `mode` | `__attribute__((mode)) is not supported: the declaration would keep its written type, so a typedef that asks for a specific width would silently get another` |
| `transparent_union` | `__attribute__((transparent_union)) is not supported: the union would be passed as a union rather than as its first member, which is a different calling convention` |
| `target` | `__attribute__((target)) is not supported: EmbCC selects its instruction set per compilation; a function asking for another would be compiled for the wrong one` |
| `weakref` | `__attribute__((weakref)) is not supported: the symbol would be emitted as an ordinary reference, so a missing target would fail to link instead of being null` |
| `ifunc` | `__attribute__((ifunc)) is not supported: the resolver would never run and calls would go to it rather than to the implementation it picks` |
| `constructor(N)`, `destructor(N)` | `__attribute__((constructor(101))) is not supported: EmbCC emits one .init_array in source order and cannot honour a priority` |
| `aligned` on a typedef | `__attribute__((aligned(16))) on a typedef is not supported: EmbCC carries alignment on objects and on struct definitions, not on a type name; put it on the declaration that uses 'i16'` |
| `packed` or `aligned` on an enum | `a packed or aligned enum is not supported (EmbCC's enums are always int-sized)` |
| `alias` on a variable | `alias attribute on variable 'b' is not supported (functions take it)` |
| `section` on a block-scope variable | `section attribute on block-scope 'x' is not supported — declare it at file scope` |

`error` and `warning` are accepted with a warning that the check they
request will not happen:

```text
warning: __attribute__((error)) is accepted but does nothing here: it makes a CALL to this function a compile error unless the optimizer removes the call, so the diagnostic has to wait until after optimisation and EmbCC issues its own before then; a build-time assertion written with it will not fire [-Wattributes]
```

An attribute EmbCC does not know is ignored with a warning:

```text
warning: attribute 'no_such_attribute' is not one EmbCC knows, and is ignored [-Wattributes]
```

A further set of attributes (`hot`, `cold`, `pure`, `const`, `nonnull`,
`flatten`, `optimize`, `returns_twice` and others) is accepted and has
no effect, because what it asks for is either already true or used only
by an analysis EmbCC does not have. The source table in
`src/parse/parse.c` gives the reason for each. For `returns_twice` the
reason is compatibility with newlib's `<setjmp.h>`; the source notes
that ignoring it can be wrong above `-O0`, where a value kept in a
register across the call may not survive the second return.

### Builtins

A builtin EmbCC does not provide is an undeclared function:

```text
error: '__builtin_abs' is not declared in 'f' — for a call, add a prototype or define it first [E0001]
```

`__has_builtin` answers 0 for these. Among the GCC and Clang builtins
that are missing:

- The `__builtin_` spellings of most C library functions:
  `__builtin_abs`, `__builtin_labs`, `__builtin_llabs`,
  `__builtin_printf`, `__builtin_puts`, `__builtin_malloc`,
  `__builtin_free`, and the math functions `__builtin_exp`,
  `__builtin_log`, `__builtin_pow`, `__builtin_powi`, `__builtin_floor`,
  `__builtin_ceil`, `__builtin_trunc`, `__builtin_round`,
  `__builtin_fmin`, `__builtin_fmax`, `__builtin_fmod`, `__builtin_fma`,
  `__builtin_ldexp` and `__builtin_frexp`. (`__builtin_memcpy`,
  `memset`, `memmove`, `memcmp`, `memchr`, `strlen`, `strcmp`, `strcpy`,
  `strchr`, `fabs`, `copysign` and `sqrt` are provided.)
- `__builtin_isgreater`, `__builtin_isgreaterequal`,
  `__builtin_isless`, `__builtin_islessequal`,
  `__builtin_islessgreater`, `__builtin_isunordered` and
  `__builtin_fpclassify`.
- `__builtin_setjmp`, `__builtin_longjmp`, `__builtin___clear_cache`,
  `__builtin_extract_return_addr`, `__builtin_debugtrap`.
- `__builtin_complex`, `__builtin_classify_type`,
  `__builtin_va_arg_pack`, `__builtin_assume`,
  `__builtin_speculation_safe_value`, `__builtin_add_overflow_p`.
- `__builtin_bitreverse*`, `__builtin_rotateleft*`,
  `__builtin_popcountg` and the C23 `__builtin_stdc_*` family.
- Vector builtins: `__builtin_shufflevector`, `__builtin_elementwise_*`,
  `__builtin_reduce_*`, `__builtin_nontemporal_load`.
- Target builtins: `__builtin_cpu_supports`, `__builtin_ia32_*`,
  `__builtin_avr_*`.
- Clang's `__c11_atomic_*` family. `_Atomic`, `<stdatomic.h>`, the
  `__atomic_*` builtins and the `__sync_*` builtins are provided.

### Code generation

| Construct | Diagnostic |
|---|---|

## C++

C++ is compiled by lowering it to C ([D-013](decisions.md#d-013)). Its
status feature by feature is in [C++ support](../manual/cxx.md). By
target:

| Target | C++ |
|---|---|
| x86-64 and AArch64 ELF | Supported, with exceptions, RTTI and `libcxx.a`. |
| Apple arm64 | Supported, with exceptions, against the system's C++ runtime. |
| macOS x86-64 | An object that uses exceptions does not link: Apple's linker reports `ld: fixup error (kind=x86_64_rip) ... target '___cxa_allocate_exception' does not have address`. Compile with `-fno-exceptions`. |
| Windows x86-64 | Only with `-fno-exceptions -fno-unwind-tables`. Otherwise every unit is refused: `C++ exceptions are not supported for a Windows target yet: the unwind tables go in .pdata and .xdata and neither is written` |
| Cortex-M, RV32, AVR | Not supported. The C++ front end lays out types for 8-byte `long` and pointers, so code generation is refused on a target where either is narrower: `C++ is not yet supported for thumbv7m-none-eabi: the C++ front end lays out types for 8-byte long and pointers, and this target's long is 4 bytes and its pointers 4`. `-fsyntax-only`, `-E` and `-c -M` still run. |
| RV64 | Not supported. Code is generated, but no `libcxx.a` is built, and a function that needs a landing pad is refused: `the RV64 backend cannot lower this operation yet (function F) [landing w=8 size=4]`. |

Language refusals:

| Construct | Diagnostic |
|---|---|
| Modules | `modules are not supported` |
| `asm goto` | `asm goto in C++ is not supported yet` |
| File-scope `asm` | `file-scope asm in C++ is not supported yet` |
| `&&label` and computed `goto` | `label addresses are not supported in C++` |
| `_Atomic` | `_Atomic is not supported in C++` |
| An initializer on an array `new` | `an initializer for new[] is not supported yet` |
| A structured binding at namespace scope | `a structured binding at namespace scope is not supported yet` |
| Explicit instantiation of a variable template | `explicit instantiation of a variable template is not supported yet` |
| `#pragma pack` | `#pragma pack is not supported in C++: it would change the layout and EmbCC would ignore it. Use __attribute__((packed)) on the struct` |
| `auto(x)` (C++23) | `expected an expression before 'auto'` |
| Multidimensional `operator[]` (C++23) | `'M' has no viable operator[]` |

Two-phase name lookup is not implemented: a template is parsed again at
each instantiation, so a non-dependent name is looked up at the point of
instantiation. Several ill-formed constructs are accepted without a
diagnostic. Both are described in
[C++ support](../manual/cxx.md#notes-on-partial-support).

## ABI limitations

### Windows

Every compile for `x86_64-windows-gnu` (or its alias
`x86_64-w64-mingw32`) prints this warning. It is on by default and
`-Wno-windows-abi` silences it:

```text
embcc: f.c: warning: x86_64-windows-gnu is not yet the Microsoft x64 ABI: `long` is 8 bytes and wchar_t 4 (Windows has 4 and 2), and rsi, rdi, xmm6 and xmm7 are not preserved across a call: objects EmbCC compiles agree with each other and with nothing else [-Wwindows-abi]
```

Argument registers, shadow space, by-reference passing of aggregates and
the Microsoft `va_list` are implemented; see
[Windows](../manual/targets.md#windows-coff). No program built for this
target has run on Windows, and EmbCC provides no C library for it.
These are refused:

| Construct | Diagnostic |
|---|---|
| `long double` in a function's signature | `long double in the signature of 'f' is not supported for a Windows target yet: there it travels by reference and returns through a hidden pointer, and EmbCC passes it on the stack by value` |
| `__int128` in a function's signature | `__int128 in the signature of 'f' is not supported for a Windows target yet: EmbCC lowers its arithmetic to libgcc helpers whose arguments it places in the System V registers` |
| `va_arg` of a structure | `va_arg of a struct is not supported for a Windows target yet` |
| `va_arg` of `long double` or `__int128` | `va_arg of long double is not supported for a Windows target yet: there it travels by reference` |
| `-g` | `-g is not supported for a Windows target yet: its debug information goes in CodeView records this does not write, and emitting DWARF under a COFF name would be worse than refusing` |
| `__thread` | `__thread is not supported for a Windows target yet: Windows reaches a thread-local through a _tls_index and a TLS directory this writer does not emit` |
| `constructor`, `destructor` | `__attribute__((constructor)) is not supported for a Windows target yet: it needs the .ctors/.dtors sections this COFF writer does not emit` |
| File-scope `asm` with labels or symbol references | `a file-scope asm block with labels or symbol references is not supported for a Windows target yet` |
| `alias` | `alias attribute on 'h' is not supported for COFF output` |
| `section` on a function | `a function's section attribute is not supported for COFF output` |
| `section` on a variable | `'v': a variable's section attribute is not supported for COFF output` |

### Darwin

Both Darwin triples produce Mach-O objects for the system linker.
`aarch64-apple-darwin` follows Apple's arm64 convention and data model,
and `x86_64-apple-darwin` follows System V; see
[Apple arm64](../manual/targets.md#apple-arm64). These are refused:

| Construct | Diagnostic |
|---|---|
| `-g` | `-g is not supported for a Darwin target yet: its DWARF goes in a __DWARF segment this does not write, and emitting the ELF layout under a Mach-O name would be worse than refusing` |
| `__thread` | `__thread is not supported for a Darwin target yet: Mach-O addresses a thread-local through a __thread_vars descriptor, which this writer does not emit` |
| `constructor`, `destructor` | `__attribute__((constructor)) is not supported for a Darwin target yet: it needs a __DATA,__mod_init_func section this Mach-O writer does not emit` |
| File-scope `asm` with labels or symbol references | `a file-scope asm block with labels or symbol references is not supported for a Darwin target yet: its bytes would be emitted but its symbols and relocations dropped` |
| `alias` | `alias attribute on 'h' is not supported for Mach-O output` |
| `section` on a function | `a function's section attribute is not supported for Mach-O output` |

EmbCC compiles against its own C headers on Darwin too. They declare
`stdin`, `stdout` and `stderr` as objects of those names and make
`errno` a call to `__errno_location`; the macOS C library defines none
of these, so a program that uses them does not link with the system
library. The macOS SDK headers do not compile with EmbCC: `sys/cdefs.h`
stops with `#error Unsupported architecture`, because EmbCC does not
predefine `__arm64__`. Calls to C library functions declared in EmbCC's
headers, such as `printf`, link and run.

### Embedded targets

- Cortex-M objects carry `.ARM.attributes`, and `embld` refuses to link
  objects whose float ABI (`Tag_ABI_VFP_args`) or enumeration size
  (`Tag_ABI_enum_size`) disagree. An enumeration is `int`-sized unless
  one of its values does not fit in `int`, as with Clang;
  `-fshort-enums` is refused.
- On AVR, structures are passed and returned by avr-gcc's documented
  rules. Clang's AVR target passes structure arguments differently.
- MIPS32 objects carry `.MIPS.abiflags` (soft float), and `embld` refuses
  to link objects whose floating-point ABI disagrees. EmbCC's objects and
  clang's (`--target=mipsel-unknown-elf -msoft-float`) call each other
  correctly in both directions (`tests/golden/mips-abi.sh`), including
  clang's `_Complex` results in `v0`..`a1`; clang's PIC and small-data
  objects (`-fPIC`, `-mno-abicalls -G8`) are refused by `embld` by
  relocation name.
- C++ code generation is refused on Cortex-M, RV32, MIPS32 and AVR,
  whose `long` or pointers are narrower than the 8 bytes the C++ front
  end lays types out for (see [C++](#c)).

## Options

The options below are refused, each with the message shown. The full
list, with the options that are accepted and have no effect, is in
[Invoking EmbCC](../manual/invoking.md#refused-options).

| Option | Diagnostic |
|---|---|
| `-fPIC`, `-fpic`, `-fPIE`, `-fpie`, `-flto`, `-fshort-enums`, `-fprofile-*`, `--coverage`, `-pg`, `-fstack-clash-protection`, `-fcf-protection` | `embcc: error: -fPIC is not supported; EmbCC would emit ordinary code and the flag's promise would not hold` (naming the option) |
| `-shared`, `-static-pie` | `embcc: error: -shared needs position-independent code, which EmbCC does not emit` |
| `-fsanitize=` with a check other than `undefined`, `signed-integer-overflow`, `integer-divide-by-zero`, `shift`, `shift-exponent` | `-fsanitize=address is not supported: EmbCC's sanitizer inserts checks that TRAP, and this one needs a runtime library to report through. The ones it has are undefined, signed-integer-overflow, integer-divide-by-zero and shift` |
| `-fstack-protector`, `-fstack-protector-strong`, ... | `embcc: '-fstack-protector' is not supported (EmbCC emits no stack protection); -fno-stack-protector is` |
| `-gdwarf-5`, `-gsplit-dwarf`, `-gz` | `embcc: error: -gdwarf-5 is not supported; EmbCC emits DWARF 4, uncompressed and in one piece` |
| `-O4` and up | `embcc: unknown optimization flag '-O4'` |
| `-mno-unaligned-access` (ARM) | `-mno-unaligned-access is not supported: EmbCC's ARMv7-M code uses word and halfword loads and stores at unaligned addresses (packed struct members; copies and by-value passing of structs aligned below 4), which the architecture allows and this flag forbids` |
| `-mabi=` other than `aapcs`, `aapcs-linux` (ARM) or `ilp32`/`lp64` (RISC-V), `-mbig-endian` | `-mabi=apcs-gnu is not supported: EmbCC emits the AAPCS ...`; `embcc: error: -mbig-endian is not supported: every target EmbCC emits for is little-endian` |
| `-fdump-*`, `-fcallgraph-info` | `embcc: error: -fdump-rtl-expand is not supported: it dumps GCC's internal representation, which EmbCC does not have; ...` |
| `-fcommon` (Mach-O, COFF), `-fsingle-precision-constant` (C++) | `embcc: error: -fcommon is not supported for x86_64-apple-darwin: EmbCC writes COMMON symbols into ELF objects only, ...` |

These are unknown arguments (`embcc: error: unknown argument '-march=native'`,
followed by the usage summary): `-march=`, `-mtune=`, `-m32`, `-m64`,
`-mabi=` on x86-64, AArch64 and AVR, `-mmcu=`, `-mavx2` and the other x86
feature flags; `-imacros`, `-iquote`, `-idirafter`, `-undef`; `-ansi`;
`-ffp-contract=`, `-funroll-loops`, `-ftrapv`, `-fvisibility=`,
`-fopenmp`; `-v`. The flags GCC builds for Cortex-M pass (`-ffast-math`,
`-fcommon`, `-fno-jump-tables`, `-save-temps`, `-specs=`, `-mabi=aapcs`
and the rest) are accepted, implemented or refused as
[Invoking EmbCC](../manual/invoking.md) lists.

`-Wl,OPTION` and `-Xlinker OPTION` reach the driver's link. EmbLD's own
options (`-T`, `-L`, `-u`, `-e`, `-Ttext`, `-Tdata`, `-Tstack`,
`--rom-limit`, `--lma-offset`, `--orphan-handling`, `--gc-sections`,
`--print-gc-sections`, `-Map`, `--print-memory-usage`) are applied, a
set that changes nothing about the image (`-s`, `-z now` and others) is
accepted, and any other is refused before anything is linked:

```text
embcc: error: linker option '-T' is not one EmbLD has (it takes -e, -Ttext, -Tdata, -Tstack, --rom-limit and --lma-offset); dropping it could build a different image from the one asked for
```

`-Wa,OPTION` accepts `--noexecstack`, `-g`, `--gdwarf*` and `-mrelax`
and refuses any other
(`embcc: error: assembler option '-al' is not one the integrated assembler has`).
The full rules are in
[Invoking EmbCC](../manual/invoking.md#-wlargs--xlinker-arg).

`-pedantic` and `-pedantic-errors` are accepted and turn nothing on:

```text
embcc: warning: -pedantic: EmbCC has no diagnostics for extensions to ISO C, so this turns nothing on
```

Any `-W` name that is not one of EmbCC's eighteen warnings
(`--help-warnings` lists them) is accepted with:

```text
embcc: warning: -Wconversion is not a warning EmbCC has, so it turns nothing on (--help-warnings lists them)
```

`-std=` selects no dialect. `-std=c89` and `-std=c99` are accepted with
a warning:

```text
embcc: warning: -std=c89 is accepted but not enforced; EmbCC has one C dialect, C11 with the GNU extensions, and will compile newer constructs anyway
```

## Known defects

These are cases where EmbCC accepts something and does not do what was
asked, without an error. Each is a defect, not intended behavior.

- **Pragmas.** `pack`, `once`, `push_macro`, `pop_macro` and `weak`
  have an effect. Every other pragma is dropped without a diagnostic,
  and there is no `-Wunknown-pragmas`. In particular,
  `#pragma GCC diagnostic` does not change any warning,
  `#pragma GCC poison` poisons nothing, and `#pragma redefine_extname`
  does not rename the symbol.
- **Debug information.** On AVR, `-g` produces a compile unit with no
  functions, variables or line-table rows. Enumerations, `typedef` names
  and lexical blocks are not described on any target. The full list is
  in [Debugging](../manual/debugging.md#known-problems).

## Optimizer gaps

The optimizer is described in [The optimizer](optimizer.md) and the
user-visible levels in [Optimization](../manual/optimization.md). These
are the transformations GCC and Clang make that EmbCC does not, as the
source states them:

- **No machine-level IR.** Instruction selection writes machine code
  directly from EmbIR (`embcc inspect mir` says so). There is no
  instruction scheduling and no machine-level optimization pass.
- **SSA only inside one pass.** EmbIR is linear three-address code.
  `mem2reg` builds SSA form and immediately leaves it again, turning each
  phi into copies; every other pass works on the non-SSA form and
  recomputes the control-flow and data-flow facts it needs.
- **Alias analysis by base object only.** Two references based on
  different objects do not alias, and an unknown pointer cannot reach a
  stack slot whose address is never taken. There is no type-based
  aliasing (so `-fstrict-aliasing` changes nothing), no use of
  `restrict`, and no field sensitivity.
- **Interprocedural optimization is limited to one translation unit**:
  inlining, inference of which functions read or write memory a caller
  can see, and replacing loads from a `static` global that nothing in
  the unit writes with its value. There is
  no link-time optimization, and no call is ever eliminated: two calls to
  a `const` or `pure` function stay two calls.
- **The inliner declines** a callee that is not defined in the unit, is
  variadic, recursive, `noinline` or too large for the budget, has
  exception regions, returns a structure or a value wider than a
  register, takes a parameter that is not a simple scalar, contains
  inline `asm`, `va_start`, a variable-length array or a computed goto,
  computes in `__int128`, or calls a function that returns a structure.
  `-fremarks` names the reason at each call
  (`remark: not-inlined 'g' [inline/callee-is-varargs]`).
- **Vectorization** is done on x86-64 only, and not at `-Os`. A
  floating-point sum reduction is not vectorized, because that would
  change the order of the additions; there is no option that permits it.
- **Division by a constant** becomes a multiply only on 64-bit targets.
  On Cortex-M the hardware divide is used; on AVR it is a library call.
- **No profile-guided optimization**: `-fprofile-*` is refused, and
  `hot` and `cold` do not affect code placement.
- **No fast-math mode.** Floating-point arithmetic is not reassociated
  or contracted into fused multiply-adds.
- **Functions with exception regions or computed `goto`.** The passes
  that need a complete control-flow graph are skipped in a function with
  live exception regions, and the passes that place code on an edge are
  skipped in a function with `goto *`. On x86-64 and AArch64 such a
  function keeps every value in memory.
- **`-O3` is `-O2`.** There is no `-Og`.

## Milestones

The project's original milestones, and their state:

| Milestone | Acceptance | State |
|---|---|---|
| <a id="m0"></a>M0 | `make && make test` runs on the host. | Closed. |
| <a id="m1"></a>M1 | An object compiled by EmbCC and linked against the EmbLinkOS C runtime runs on EmbLinkOS and exits with 42. | Closed. |
| <a id="m2"></a>M2 | A real EmbLinkOS program (`tally.c` and the `sval` SDK) compiles against real headers and passes the operating system's own check. | Closed. |
| <a id="m3"></a>M3 | EmbCC compiles itself, and the second-stage compiler is byte-identical to the first (`tests/golden/x86_64/self-host.sh`). | Closed. |
| <a id="m4"></a>M4 | EmbLinkOS builds EmbCC from `build.ebm` with EmbBuild, and that compiler builds the M1 program. | Open. The host half (`tests/golden/x86_64/embbuild.sh`) walks the manifest to a linked compiler; the build on EmbLinkOS itself is not part of this repository's tests. |

## Roadmap

These items are named as open in the source or in the project's
roadmap, and are open in the code. They are listed without priority or
dates.

- Several input files per invocation, object-file and archive inputs,
  and `-l`/`-L`.
- Position-independent code, shared libraries and dynamic linking.
- `embld` support for AArch64.
- Debug information, thread-local storage and constructors in Mach-O
  and COFF objects; the Microsoft x64 data model and callee-saved
  registers; `.pdata`/`.xdata` unwind tables for Windows.
- GNU-syntax assembly files for x86-64.
- binary128 `long double` arithmetic on RISC-V, and `__int128` at RV64.
- Computed `goto` on Cortex-M, RISC-V, MIPS32 and AVR; variable-length
  arrays and aligned locals on AVR;
  narrow and 8-byte atomic read-modify-write, unwind tables and
  `__attribute__((interrupt))` on MIPS32.
- On MIPS32, a delay slot filled from anywhere but the instruction just
  before the transfer (the branch target's first instruction, or one
  from before the `slt` a compare-and-branch needs).
- Scalar locals aligned beyond the stack alignment.
- Linker relaxation: on RISC-V a call between sections stays an
  eight-byte `auipc`/`jalr`.
- A machine-level IR below EmbIR.
- SSA as the form of EmbIR between passes.
- Alias information on memory references (types, `restrict`, fields).
- Vectorization on targets other than x86-64.
- Reading the vDSO on Linux.
- The C++ items marked "not supported yet" above and in
  [C++ support](../manual/cxx.md).
- [M4](#m4) on EmbLinkOS.

## Reporting a bug

### Is it a bug?

- An error that says something "is not supported" is a limitation
  listed on this page, not a bug.
- These are bugs: an `internal error` (EmbCC adds
  `note: this is a bug in EmbCC, not in the program being compiled`), a
  crash, a program that compiles and computes a wrong result, an object
  that another toolchain's assembler, linker or debugger rejects, a
  construct that is accepted and silently ignored or changed, and a
  disagreement with GCC or Clang about a calling convention or a data
  layout that the target's ABI defines.

### Narrowing it down

- Compile at `-O0`, `-O1` and `-O2`. A failure only at `-O2` points at
  the optimizer or the register allocator.
- Turn off one pass at a time with `-fno-NAME` (for example
  `-fno-gcse`); the names are in
  [Optimization](../manual/optimization.md#controlling-individual-passes).
- Set `EMBCC_VERIFY=1`. The optimizer then checks the IR before and
  after it runs and stops at the first inconsistency.
- `embcc inspect ir -O2 FILE` prints the IR the backend receives, and
  `-fremarks` reports what the optimizer did and declined to do.
- Reduce the program to the smallest file that still fails.

### What to include

- The first line of `embcc --version`, and the host system.
- The complete command line, including `--target=`, or the output of
  `-dumpmachine` with the same options.
- The source, preprocessed with the same options
  (`embcc -E OPTIONS FILE.c > FILE.i`), or the reduced file.
- The complete output of the compiler.
- What you expected and what happened. For a calling-convention or
  layout problem, the other compiler, its version and its command line.

Report it as an issue in the EmbCC repository,
`https://github.com/EmbLinkOs/EmbCC/issues`.

A fix comes with a test that fails without it; see
[Testing](testing.md#a-new-test-must-fail-first). A construct that
EmbCC should refuse is added to `tests/compile/reject-unimplemented.sh`
([A refusal test](testing.md#a-refusal-test)).
