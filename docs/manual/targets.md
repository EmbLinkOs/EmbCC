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
little-endian.

| Family | Canonical triples | Object format | Calling convention | Linked by |
|---|---|---|---|---|
| [x86-64](#x86-64) | `x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu` | ELF64 | System V AMD64 | `embcc` (integrated `embld`) |
| [x86-64 macOS](#macos-mach-o) | `x86_64-apple-darwin` | Mach-O | System V AMD64 | the system linker |
| [x86-64 Windows](#windows-coff) | `x86_64-windows-gnu` | COFF | Microsoft x64, incomplete | an external linker |
| [AArch64](#aarch64) | `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` | ELF64 | AAPCS64 | an external linker |
| [Apple arm64](#apple-arm64) | `aarch64-apple-darwin` | Mach-O | Apple arm64 | the system linker |
| [ARM Cortex-M](#arm-cortex-m) | `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` | ELF32 | AAPCS32, AAPCS-VFP | `embld` |
| [RISC-V](#risc-v) | `riscv32-unknown-elf`, `riscv64-unknown-elf` | ELF32, ELF64 | RISC-V psABI, `ilp32` / `lp64` | `embld` |
| [AVR](#avr) | `avr` | ELF32 | avr-gcc | `embld` |
| [MIPS32](#mips32) | `mipsel-none-elf` | ELF32 | o32, soft float | `embld` |

| Target | Status | Floating point | `-g` | Lock-free atomic read-modify-write | `__thread` | C++ |
|---|---|---|---|---|---|---|
| x86-64 ELF, EmbLinkOS, Linux | Primary target | SSE2; x87 for `long double` | DWARF | 1, 2, 4, 8, 16 bytes | Local-exec TLS | Yes, with exceptions |
| x86-64 macOS | Objects for the system linker | SSE2; x87 for `long double` | Refused | 1, 2, 4, 8, 16 bytes | Refused | Yes, with exceptions |
| x86-64 Windows | Objects only; warns on every compile | SSE2 | Refused | 1, 2, 4, 8, 16 bytes | Refused | Refused |
| AArch64 ELF, EmbLinkOS, Linux | Supported | FP/SIMD; `long double` in software | DWARF | 1, 2, 4, 8, 16 bytes | Local-exec TLS | Yes, with exceptions |
| Apple arm64 | Objects for the system linker | FP/SIMD | Refused | 1, 2, 4, 8, 16 bytes | Refused | Yes, with exceptions |
| Cortex-M, soft float | Bare metal | Software | DWARF | 1, 2, 4 bytes | One shared instance | Refused |
| Cortex-M, FPU | Bare metal | Single-precision VFP; `double` in software | DWARF | 1, 2, 4 bytes | One shared instance | Refused |
| RV32 | Bare metal | Software | DWARF | 4 bytes | One shared instance | Refused |
| RV64 | Bare metal | Software | DWARF | 4, 8 bytes | One shared instance | Without exceptions |
| AVR (ATmega328P) | Bare metal | Software, 4-byte `double` | DWARF | None (1-byte load and store only) | One shared instance | Refused |
| MIPS32r2 (PIC32-class) | Bare metal | Software | DWARF | 4 bytes | One shared instance | Refused |

"One shared instance" means the object is placed in `.tbss` but
addressed as an ordinary static object: there is one copy, not one per
thread. "Refused" under C++ means a C++ unit is not compiled for that
target (`-fsyntax-only` still checks it); the diagnostic is in the
target's Limitations. "Without exceptions" means code that needs a
landing pad (a `try` block, or a destructor that must run during
unwinding) does not compile; build C++ for RV64 with `-fno-exceptions`.

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

| Type | x86-64 | macOS x86-64 | Windows | AArch64 | Apple arm64 | Cortex-M | RV32 | RV64 | AVR | MIPS32 |
|---|---|---|---|---|---|---|---|---|---|---|
| plain `char` | signed | signed | signed | unsigned | signed | unsigned | unsigned | unsigned | signed | signed |
| `short` | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/1 | 2/2 |
| `int` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 2/1 | 4/4 |
| `long` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/4 | 4/4 | 8/8 | 4/1 | 4/4 |
| `long long` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/1 | 8/8 |
| pointer, `size_t` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/4 | 4/4 | 8/8 | 2/1 | 4/4 |
| `float` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/1 | 4/4 |
| `double` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/1 | 8/8 |
| `long double` | 16/16 x87 | 16/16 x87 | 16/16 x87 | 16/16 binary128 | 8/8 binary64 | 8/8 binary64 | 16/16 binary128 | 16/16 binary128 | 4/1 binary32 | 8/8 binary64 |
| `wchar_t` | 4/4 `int` | 4/4 `int` | 4/4 `int` | 4/4 `unsigned int` | 4/4 `int` | 4/4 `unsigned int` | 4/4 `int` | 4/4 `int` | 2/1 `int` | 4/4 `int` |
| `__int128` | 16/16 | 16/16 | 16/16 | 16/16 | 16/16 | — | — | 16/16 | — | — |
| `enum` (all values fit `int`) | 4 | 4 | 4 | 4 | 4 | 4 | 4 | 4 | 2 | 4 |
| `__BIGGEST_ALIGNMENT__` | 16 | 16 | 16 | 16 | 16 | 8 | 16 | 16 | 1 | 8 |
| Stack alignment at a call | 16 | 16 | 16 | 16 | 16 | 8 | 16 | 16 | 1 | 8 |

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
`embcc: error: unknown argument '-mfoo'`. In particular `-m32`, `-m64`
and `-mmcu=` are not accepted, and `-march=` and `-mabi=` only on MIPS
(see [MIPS32](#mips32)).

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
| `-g` | `-g is not supported for a Darwin target yet: its DWARF goes in a __DWARF segment this does not write, and emitting the ELF layout under a Mach-O name would be worse than refusing` |
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
- A scalar local aligned beyond 16 bytes is refused: `'x' needs 32-byte
  alignment and the stack only guarantees 16: supported for an array or a
  struct, not yet for a scalar`. An array or structure local so aligned
  is supported: its storage is carved from the stack at function entry
  and rounded up.

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

The ARMv6-M profile in Thumb-1, and the ARMv7-M, ARMv7E-M and ARMv8-M
Mainline profiles in Thumb-2. Every Cortex-M target is freestanding.

### Triples

| Triple | Accepted aliases | Architecture | Float ABI | Cores |
|---|---|---|---|---|
| `thumbv6m-none-eabi` | `thumbv6m`, `armv6m-none-eabi` | ARMv6-M | soft | Cortex-M0, M0+, M1 |
| `thumbv7m-none-eabi` | `thumbv7m`, `armv7m-none-eabi`, `arm-none-eabi` | ARMv7-M | soft | Cortex-M3 |
| `thumbv7em-none-eabi` | `thumbv7em`, `armv7em-none-eabi` | ARMv7E-M | soft | Cortex-M4, M7 |
| `thumbv7em-none-eabihf` | | ARMv7E-M | hard, FPv4-SP-D16; FPv5-D16 with `-mcpu=cortex-m7` | Cortex-M4F; Cortex-M7 |
| `thumbv8m.main-none-eabi` | `thumbv8m.main`, `thumbv8m-none-eabi`, `armv8m.main-none-eabi` | ARMv8-M Mainline | soft | Cortex-M33 |
| `thumbv8m.main-none-eabihf` | | ARMv8-M Mainline | hard, FPv5-SP-D16 | Cortex-M33 with FPU |

`arm-none-eabi` means ARMv7-M; there is no target for the A- or R-profile
or for the ARM instruction set.

The architecture changes the object's build attributes, the predefined
macros and `-dumpmachine`. The instructions generated for ARMv7-M and
ARMv7E-M are the same: EmbCC does not use the DSP extension.

### Options

#### `-mthumb`

Accepted; no effect. Thumb is the only instruction set.

#### `-marm`

Refused: `-marm is not supported: a Cortex-M has no ARM instruction set,
only Thumb`.

#### `-mcpu=CPU`

Select the architecture variant by core. `cortex-m0`, `cortex-m0plus`
and `cortex-m1` select ARMv6-M on any ARM triple, as
`thumbv6m-none-eabi` does. `cortex-m3` selects ARMv7-M; `cortex-m4`,
`cortex-m7` and `cortex-m33` select ARMv7E-M. Otherwise the option does
not change the architecture level: `--target=thumbv7m-none-eabi
-mcpu=cortex-m33` is `thumbv7em-none-eabi`, and ARMv8-M is selected only
by a `thumbv8m.main` triple. On a `thumbv6m` triple, an ARMv7-M or
ARMv8-M part raises the level. Any other value is an error:

```text
embcc: error: -mcpu=cortex-m55 is not a part EmbCC knows: it emits ARMv6-M (cortex-m0, m0plus, m1), ARMv7-M and ARMv7E-M (cortex-m3, m4, m7, m33)
```

The Cortex-M23 (ARMv8-M Baseline) is refused by name: it is a different
subset from ARMv6-M, and EmbCC emits neither for it:

```text
embcc: error: -mcpu=cortex-m23 is ARMv8-M Baseline, and EmbCC emits ARMv6-M (cortex-m0, m0plus, m1) or ARMv7-M Thumb-2: the second faults on that core and the first is not what it is
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

ARMv8-M's security extension (TrustZone-M) is not supported. `-mcmse`
is an unknown argument, and `cmse_nonsecure_entry` and the other CMSE
attributes are ignored with a `-Wattributes` warning.

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
| `__ARM_ARCH_7M__` / `__ARM_ARCH_8M_MAIN__` | `__ARM_ARCH_7M__` | `__ARM_ARCH_8M_MAIN__` |
| `__ARM_ARCH_PROFILE` | `'M'` | `'M'` |
| `__ARM_FEATURE_IDIV`, `__ARM_FEATURE_CLZ`, `__ARM_FEATURE_LDREX` (0x7) | yes | yes |
| `__CHAR_UNSIGNED__`, `__WCHAR_UNSIGNED__` | 1 | 1 |
| `__BIGGEST_ALIGNMENT__` | 8 | 8 |

The ARMv7E-M triples define the same macros as ARMv7-M:
`__ARM_ARCH_7EM__` and `__ARM_FEATURE_DSP` are not defined.

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

The atomic routines mask interrupts with PRIMASK around the access. That
is atomic on a single core running privileged code; CPSID is ignored in
unprivileged Thread mode, so an RTOS whose tasks run unprivileged, or a
part with another bus master, defines its own.

### Limitations

| Construct | Diagnostic |
|---|---|
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| 8-byte atomic read-modify-write | `the ARMv7-M backend cannot lower this operation at 64 bits yet (function f) [xadd w=8 size=8]` (ARMv6-M: `the ARMv6-M backend cannot lower an atomic wider than four bytes`) |
| on ARMv6-M, an inline asm template that uses a Thumb-2 instruction | `the ARMv6-M backend cannot lower an instruction ARMv6-M does not have (a 32-bit Thumb-2 encoding, from inline asm or the backend) yet (function f)` |
| 8-byte atomic load or store | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| a scalar local aligned beyond 8 | `'x' needs 32-byte alignment and the stack only guarantees 8: supported for an array or a struct, not yet for a scalar` |
| any C++ translation unit, except with `-fsyntax-only`, `-E`, `-M` or `-MM` | `C++ is not yet supported for thumbv7m-none-eabi: the C++ front end lays out types for 8-byte long and pointers, and this target's long is 4 bytes and its pointers 4` |

An array or structure local aligned beyond 8 bytes is supported: its
storage is carved from the stack at function entry and rounded up.

## RISC-V

### Triples

| Triple | Accepted aliases | ISA | ABI |
|---|---|---|---|
| `riscv32-unknown-elf` | `riscv32`, `riscv32-elf`, `rv32` | RV32IMAC | `ilp32` |
| `riscv64-unknown-elf` | `riscv64`, `riscv64-elf`, `rv64` | RV64IMAC | `lp64` |

Both are freestanding. One code generator serves both widths.

### Extensions

The instruction set is fixed; there is no `-march=` or `-mabi=`.

| Extension | Use |
|---|---|
| I | The base integer instruction set. |
| M | Multiply and divide. At RV32, 64-bit division is a call (`__divdi3` and family). |
| A | Atomic read-modify-write: `amoadd`, `amoor` and the other AMOs, and `lr`/`sc` loops for compare-and-swap, on 4-byte (and at RV64, 8-byte) objects. Memory barriers are `fence rw, rw`. |
| C | Compressed instructions, emitted wherever an encoding allows. The object's `e_flags` has `EF_RISCV_RVC` set. |

There is no F or D extension: every floating-point operation is a call to
a soft-float helper (`__addsf3`, `__adddf3`, ...). The objects carry a
`.riscv.attributes` section (`rv32i2p1_m2p0_a2p1_c2p0` or the RV64
equivalent, stack alignment 16).

### Calling convention: RISC-V psABI, soft float

- Arguments go in `a0`–`a7`, then on the stack. A scalar of up to XLEN
  bits takes one register; one of 2×XLEN bits (a `long long` or a
  `double` at RV32) takes two, in any two consecutive registers for a
  named argument and in an even-aligned pair for a variadic one. An
  argument may be split between `a7` and the stack.
- An aggregate of up to 2×XLEN bits is passed in up to two registers; a
  larger one is passed by reference to a copy.
- `float` and `double` are passed and returned in integer registers, as
  the `ilp32` and `lp64` soft-float ABIs require.
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
defined at RV64 only.

### Runtime

`make rt-embedded` builds `librt.a` for `riscv32-unknown-elf`, holding
the soft-float, 64-bit division and complex helpers. There is no
archive for `riscv64-unknown-elf`, because `lib/rt/int128.c` and
`lib/rt/fp128.c` do not compile for it; compile the other `lib/rt` files
individually.

### Limitations

| Construct | Diagnostic (RV32 shown; RV64 names itself) |
|---|---|
| any operation on `long double` (and at RV64 on `__int128`) | `the RV32 backend cannot lower a 128-bit value yet (function f) [ldvar w=16 size=16]` |
| `__int128` at RV32 | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| an atomic read-modify-write on a 1- or 2-byte object | `the RV32 backend cannot lower an atomic narrower than four bytes (the A extension has no such form, and a read-modify-write of the containing word is not atomic against its neighbours) yet (function f) [xadd w=4 size=2]` |
| an 8-byte atomic read-modify-write at RV32 | `the RV32 backend cannot lower this operation at 64 bits yet (function f) [xadd w=8 size=8]` |
| an 8-byte atomic load or store at RV32 | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| a scalar local aligned beyond 16 | `'x' needs 32-byte alignment and the stack only guarantees 16: supported for an array or a struct, not yet for a scalar` |
| any C++ translation unit at RV32, except with `-fsyntax-only`, `-E`, `-M` or `-MM` | `C++ is not yet supported for riscv32-unknown-elf: the C++ front end lays out types for 8-byte long and pointers, and this target's long is 4 bytes and its pointers 4` |
| C++ code that needs a landing pad (`try`, or a destructor run during unwinding) at RV64 | `the RV64 backend cannot lower this operation yet (function f) [landing w=8 size=4]` |

An array or structure local aligned beyond 16 bytes is supported: its
storage is carved from the stack at function entry and rounded up.

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

### Limitations

| Construct | Diagnostic |
|---|---|
| an atomic read-modify-write on a 1- or 2-byte object | `the MIPS32 backend cannot lower an atomic narrower than four bytes (ll/sc are word-sized, and a read-modify-write of the containing word is not atomic against its neighbours) yet (function f) [xadd w=4 size=1]` |
| an 8-byte atomic read-modify-write | `the MIPS32 backend cannot lower an atomic wider than a register yet (function f) [xadd w=8 size=8]` |
| an 8-byte atomic load or store | `an atomic access of 8 bytes is not one access on this target (it moves 4 at once): the halves could be split by an interrupt or another core` |
| a computed `goto` | `the MIPS32 backend cannot lower a computed goto yet (function f) [labeladdr w=4 size=4]` |
| `__builtin_frame_address`, `__builtin_return_address` | `the MIPS32 backend cannot lower __builtin_frame_address or __builtin_return_address (o32 code keeps no frame-pointer chain) yet (function f) [frameaddr w=8 size=4]` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| `__attribute__((interrupt))` | `__attribute__((interrupt)) is not supported: ...` (write the exception entry in a `.S` file or a naked function; see [Bare metal](embedded.md#mips32)) |
| `-funwind-tables`, `-fasynchronous-unwind-tables`, `-fexceptions` | `unwind tables are not supported for mipsel-none-elf yet (-funwind-tables, -fasynchronous-unwind-tables, -fexceptions): EmbCC writes no MIPS .eh_frame` |
| a scalar local aligned beyond 8 | `'x' needs 16-byte alignment and the stack only guarantees 8: supported for an array or a struct, not yet for a scalar` |
| any C++ translation unit, except with `-fsyntax-only`, `-E`, `-M` or `-MM` | `C++ is not yet supported for mipsel-none-elf: ...` |

## AVR

### Triples

| Triple | Accepted aliases |
|---|---|
| `avr` | `avr-none-elf`, `avr-elf`, `avr-unknown-none` |

The part is the ATmega328P (the AVR5 architecture, as on the Arduino
Uno). There is no option to choose another; `-mmcu=` is not accepted.

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
`__SIZEOF_DOUBLE__` (4).

The table also defines `__flash` (as
`__attribute__((__address_space__(1)))`) and `__BUILTIN_AVR_CLI`,
`__BUILTIN_AVR_SEI`, `__BUILTIN_AVR_NOP`, `__BUILTIN_AVR_SLEEP`,
`__BUILTIN_AVR_SWAP` and `__BUILTIN_AVR_WDR`, but EmbCC implements
neither. `__flash` at the start of a declaration
(`__flash const char t[]`) is ignored with a `-Wattributes` warning, and
the data goes to RAM like any other; after another specifier
(`const __flash char t[]`) it fails with `expected a type before
'__attribute__'`. The `__builtin_avr_*` functions are undeclared. Use
inline assembly (`cli`, `sei`, `sleep`, `wdr`, `swap`) instead.

### Interrupt handlers

`__attribute__((signal))` and `__attribute__((interrupt))` make a
function an interrupt handler: it saves `SREG`, `r0`, `r1` and every
register the backend uses, clears `r1`, and returns with `reti`. See
[Embedded programming](embedded.md).

### Runtime

Integer multiplication and division are helper calls with libgcc's names
(`__mulsi3`, `__divsi3`, `__umodsi3`, `__muldi3`, `__divdi3`, ...), from
`lib/rt/avr.c` and `avr64.c`; floating-point arithmetic calls the
binary32 helpers (`__addsf3`, ...) in `lib/rt/avrfp*.c`. `make
rt-embedded` builds `librt.a` for `avr`.

### Limitations

| Construct | Diagnostic |
|---|---|
| any atomic read-modify-write | `the AVR backend cannot lower xadd yet (function f) [xadd w=4 size=2]` (the operation is named) |
| an atomic load or store wider than 1 byte | `an atomic access of 2 bytes is not one access on this target (it moves 1 at once): the halves could be split by an interrupt or another core` |
| a variable-length array | `the AVR backend cannot lower a variable-length array yet (function f)` |
| any local with `__attribute__((aligned))` | `the AVR backend cannot lower a local with __attribute__((aligned)): AVR's stack pointer has no known alignment, so a frame slot cannot be given one yet (function f)` |
| an interrupt handler with parameters | `the AVR backend cannot lower an interrupt handler with parameters: the hardware calls it, so there is no caller to pass them and they would be read out of whatever the interrupted code left in those registers yet (function __vector_3)` |
| an interrupt handler that returns a value | ``the AVR backend cannot lower an interrupt handler that returns a value: `reti` goes back to the interrupted instruction, and nothing is there to receive it yet (function __vector_3)`` |
| `__int128` | `__int128 does not exist on this target (it needs 64-bit registers; use long long)` |
| any C++ translation unit, except with `-fsyntax-only`, `-E`, `-M` or `-MM` | `C++ is not yet supported for avr: the C++ front end lays out types for 8-byte long and pointers, and this target's long is 4 bytes and its pointers 2` |

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
| `avr` | `qemu-system-avr -M uno`, the image passed with `-bios` | The program prints a sentinel; `qrun.sh --until` stops QEMU when it appears |

`-bios none` matters on RISC-V: without it QEMU runs OpenSBI first and
enters the image in supervisor mode. On AVR, QEMU refuses an image whose
ELF entry point is not 0.
