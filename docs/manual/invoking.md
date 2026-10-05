# Invoking EmbCC

This page is the complete reference for the `embcc` command line: every
option the driver accepts, the argument each one takes, its default, and
what EmbCC does with options it recognizes but does not implement. It is
organized like the "Invoking GCC" chapter of the GCC manual and is meant
for people who compile code with EmbCC, write build systems around it, or
port a GCC or Clang command line to it.

EmbCC sorts every option it knows into one of three kinds, and this page
says which kind each one is:

- **Honoured.** The option does what GCC and Clang document.
- **Accepted.** The option is taken without error because EmbCC already
  behaves the way it asks, or because doing nothing is harmless. Each such
  entry below says which.
- **Refused.** The option names a promise about the generated code that
  EmbCC cannot keep (position independence, stack protection, profiling,
  link-time optimization, and so on). The driver stops with an error that
  names the flag, so a build never receives an object that silently lacks
  what it asked for.

An option that is in none of these lists stops the driver with
`embcc: error: unknown argument 'ARG'`, followed by the usage line.

## Synopsis

```text
embcc [OPTION...] [FILE] [OBJECT|ARCHIVE|-lLIB...]
                                           compile and link (x86-64 ELF; ARM,
                                           RISC-V and AVR firmware with -T)
embcc -c [OPTION...] FILE [-o OBJECT]      compile or assemble to an object
embcc -S [OPTION...] FILE [-o FILE.s]      write assembly
embcc -E [OPTION...] FILE                  preprocess to standard output
embcc inspect STAGE FILE [OPTION...]       print one stage of the pipeline
embcc why DECISION [SUBJECT] FILE [OPTION...]
                                           explain an optimizer decision
embcc --version | --help | --help-warnings | -dumpmachine | --dump-predef
embcc --print-search-dirs | --explain [ID]
embcc --emit-empty-object FILE
```

EmbCC compiles any number of source files in one command, and links them
with any number of objects, archives and `-l` libraries (see
[Several source files](#several-source-files)). A command line with no input file and none of the query options
ends with `embcc: error: no input file`. Running `embcc` with no arguments
prints the usage line and exits with status 1.

Options may appear in any order, before or after the input file, with the
exceptions noted under [`-x`](#-x-language) and
[`--emit-empty-object`](#--emit-empty-object-file). Options that take a
value accept it either attached or as the next argument where the entry
says so; GCC's `--opt=value` long forms are not accepted unless listed.

## Option summary

The table lists every spelling the driver recognizes. "Section" links to
the full entry.

| Section | Options |
|---|---|
| [Overall](#overall-options) | `-c` `-S` `-E` `-o FILE` `-j N` `-x LANG` `-fsyntax-only` `-save-temps[=cwd\|obj]` `-dumpbase NAME` `-pipe` `--emit-c` `--emit-interfaces` `--emit-empty-object FILE` `--help` `-h` `--help-warnings` `--version` `-dumpmachine` `--dump-predef` `--print-search-dirs` `--explain[=ID]` |
| [Language](#c-and-c-language-options) | `-std=STD` `-fsigned-char` `-funsigned-char` `-ffreestanding` `-fno-builtin` `-fno-builtin-NAME` `-fwrapv` `-fno-strict-overflow` `-fstrict-aliasing` `-fno-strict-aliasing` `-fcommon` `-fno-common` `-fno-short-enums` `-fsingle-precision-constant` `-fchar8_t` `-fexceptions` `-fno-exceptions` `-frtti` `-fno-rtti` `-faccess-control` `-fno-access-control` |
| [Diagnostics](#warning-and-diagnostic-options) | `-w` `-Werror` `-Wno-error` `-Werror=NAME` `-Wno-error=NAME` `-Wall` `-Wextra` `-W` `-WNAME` `-Wno-NAME` `-Wsystem-headers` `-pedantic` `-pedantic-errors` `-fdiagnostics-format=FMT` `-fdiagnostics-color[=WHEN]` `-fno-diagnostics-color` `-fmax-errors=N` `-fmessage-length=N` `-fdiagnostics-parseable-fixits` `--fix` `-fanalyzer` |
| [Debugging](#debugging-options) | `-g` `-g1` `-g2` `-g3` `-ggdb` `-gdwarf` `-gdwarf-2` `-gdwarf-3` `-gdwarf-4` |
| [Optimization](#optimization-options) | `-O` `-O0` `-O1` `-O2` `-O3` `-Os` `-Oz` `-Og` `-Ofast` `-fPASS` `-fno-PASS` `-fno-inline-functions` `-finline-functions` `-finline-small-functions` `-fno-inline-small-functions` `-finline-limit=N` `-ffast-math` `-fno-math-errno` `-fno-delete-null-pointer-checks` `-fno-tree-loop-distribute-patterns` `-fmerge-constants` `-fno-isolate-erroneous-paths-dereference` `-fno-move-loop-invariants` `-fno-ipa-sra` `-fno-lto` `-fremarks` `-fremarks=json` |
| [Instrumentation](#instrumentation-options) | `-fsanitize=LIST` `-fno-sanitize=LIST` `-fsanitize-trap[=LIST]` `-fsanitize-undefined-trap-on-error` `-fstack-usage` `-fno-stack-protector` |
| [Preprocessor](#preprocessor-options) | `-D NAME[=VALUE]` `-U NAME` `-include FILE` `-Wp,ARGS` `-M` `-MM` `-MD` `-MMD` `-MF FILE` `-MT TARGET` `-MQ TARGET` `-MP` |
| [Directory search](#directory-search-options) | `-I DIR` `-isystem DIR` `-nostdinc` |
| [Assembling and linking](#assembler-and-linker-options) | (input suffixes `.s` `.S` `.asm`) `-Wa,ARGS` `-Wl,ARGS` `-Xlinker ARG` `-specs=FILE` |
| [Code generation](#code-generation-options) | `-funwind-tables` `-fasynchronous-unwind-tables` `-fno-unwind-tables` `-fno-asynchronous-unwind-tables` `-fomit-frame-pointer` `-fno-omit-frame-pointer` `-fno-plt` `-fno-pic` `-fno-pie` `-fno-jump-tables` `-fjump-tables` `-fno-zero-initialized-in-bss` `-fstrict-volatile-bitfields` `-fno-strict-volatile-bitfields` `-fverbose-asm` `-ffunction-sections` `-fdata-sections` |
| [Machine options](#machine-dependent-options) | `-mno-sse` `-mno-sse2` `-mgeneral-regs-only` `-mno-mmx` `-mno-80387` `-mno-red-zone` `-mcmodel=MODEL` `-mthumb` `-marm` `-mcpu=CPU` `-mfpu=FPU` `-mfloat-abi=ABI` `-mabi=ABI` `-mthumb-interwork` `-mno-thumb-interwork` `-munaligned-access` `-mslow-flash-data` `-mlittle-endian` |
| [Target](#target-selection) | `--target=TRIPLE` |
| [Developer](#developer-and-inspection-options) | `inspect` `why` `-fremarks` `--emit-interfaces` `--explain` |
| [Refused](#refused-options) | `-fPIC` `-fpic` `-fPIE` `-fpie` `-shared` `-static-pie` `-flto` `-fshort-enums` `-fprofile*` `--coverage` `-fcoverage-mapping` `-pg` `-fstack-protector*` `-fstack-clash-protection` `-fcf-protection*` `-fsanitize*` (other than the forms above) `-gdwarf-N` (N not 2 to 4) `-gsplit-dwarf` `-gz` `-mno-unaligned-access` `-mbig-endian` `-mabi=` (other values) `-fdump-*` `-fcallgraph-info*` |

## Overall options

### Input files

The driver decides what to do with the input file from its suffix, unless
[`-x`](#-x-language) says otherwise.

| Suffix | Treated as |
|---|---|
| `.c` | C source |
| `.cc` `.cpp` `.cxx` `.C` `.c++` `.cp` `.CPP` `.ii` | C++ source (lowered to C internally; see [C++](cxx.md)) |
| `.s` | GNU-syntax assembly, assembled for the selected target without preprocessing |
| `.S` | GNU-syntax assembly, preprocessed first |
| `.asm` | NASM/Intel-syntax x86-64 assembly (the [`embas`](tools/embas.md) assembler) |
| `.ir` | EmbIR text; meaningful only to [`embcc inspect ir`](#embcc-inspect-stage-file-option) |
| `.o` `.obj` `.a` | an object or an archive, handed to the link as it is |

### Several source files

```sh
embcc -O2 main.c uart.c timer.c -lm -o prog      # compile all three, link
embcc -O2 -j4 -c *.c                             # main.o uart.o timer.o ...
```

Each source is compiled by a run of `embcc` of its own, with the same
options, as GCC's driver runs its compiler proper. Without `-c`, `-S` or
`-E` the objects are temporaries beside the output (`OUT.embcc-tmp-N.o`),
linked in command-line order among the other inputs and then removed;
with `-c` or `-S` each source gets its own `NAME.o` or `NAME.s` in the
working directory, and `-o` is refused, since it names one file; with
`-E` they are printed one after the other. If any source fails, nothing
is linked and the temporaries are removed. `-fsyntax-only` and `--fix`
check each one. `embcc inspect`, `--why`, `--emit-c` and
`--emit-interfaces` take one source.

`-j N` (or `-jN`) compiles N sources at once; `-j` alone, one per
processor. The default is one at a time, so the diagnostics come out in
command-line order. The image does not depend on `-j`.

This needs a host that can run a program. EmbCC built with
`PROCESS=none` -- as it is for EmbLinkOS, which has no fork/exec --
compiles one source per command, and refuses a second with
`embcc: error: more than one source file ('a.c' and 'b.c'), and this
host cannot run a compiler for each (EmbCC was built with
PROCESS=none): compile each with -c and link the objects`.

Anything else, including `.i` and `.h`, is not an input file to EmbCC.
Without `-x` such an argument is refused as
`embcc: error: unknown argument 'NAME'`. EmbCC does not read source from
standard input; `-` is not accepted as a file name.

What happens to the input depends on the mode options:

| Mode | Input | Result | Default output |
|---|---|---|---|
| none | C or C++ | compiled and linked (x86-64 ELF, and ARM, RISC-V and AVR firmware with a memory map) | `a.out` |
| none | `.s` `.S` | assembled and linked, as above | `a.out` |
| none | only objects, archives and `-l` | linked | `a.out` |
| `-c` | C or C++ | relocatable object | the input's file name with its suffix replaced by `.o` |
| `-S` | C or C++ | assembly text | the input's file name with its suffix replaced by `.s` |
| `-E` | C, C++ or `.S` | preprocessed text | standard output |
| `-fsyntax-only` | C or C++ | nothing; diagnostics only | none |
| `-c` | `.s` `.S` `.asm` | object | see [Assembler and linker options](#assembler-and-linker-options) |
| any | `.asm` | object (NASM syntax is never linked by the driver) | see [Assembler and linker options](#assembler-and-linker-options) |

As with GCC, a default output is written in the current directory, not
next to the input: `embcc -c src/foo.c` writes `foo.o`, and
`embcc -S src/foo.c` writes `foo.s`.

A C++ input is compiled only for a target whose `long` and pointers are
8 bytes. For the Cortex-M targets, RV32 and AVR, every mode but `-E`,
`-M`, `-MM` and `-fsyntax-only` stops with `embcc: error: C++ is not yet supported for
TRIPLE: the C++ front end lays out types for 8-byte long and pointers,
...` (see [Targets](targets.md)).

When several mode options are given, `-E` takes precedence over the
others, then `-fsyntax-only`, then `-S`, then `-c`.

### `-c`

Compile the input to a relocatable object and do not link. The object
format follows the target: ELF for the bare-metal, EmbLinkOS and Linux
targets, Mach-O for the `-apple-darwin` targets and COFF for
`x86_64-windows-gnu` (see [Targets](targets.md)).

### `-S`

Write the code the backend generated as GNU assembler text instead of an
object. Supported on every target. Without `-o` the output is
`NAME.s` in the current directory; `-o -` writes it to standard output.
`-S` implies `-c`.

Each instruction is written as its encoded bytes in a `.byte` directive,
with every relocation attached explicitly through `.reloc`, so that
assembling the file reproduces the object `-c` writes. On x86-64 each
line also carries the disassembly as a comment; the other targets show
the bytes only. The file is a record of what was compiled, not a starting
point for hand editing.

```text
add:
	.byte	0x03,0xfe	# add    %esi,%edi
	.byte	0x48,0x89,0xf8	# mov    %rdi,%rax
	.byte	0xc3	# ret
	.size	add, .-add
```

### `-E`

Preprocess the input and write the result to standard output, or to
`FILE` with `-o FILE`, as GCC does. `-E` applies to C, C++ and `.S`
files. For a `.asm` file it is refused with
`embcc: error: -E does not apply to assembly`.

Combined with `-M` or `-MM`, `-E` prints only the dependency rule (see
[`-M`](#-m)).

### `-save-temps`, `-save-temps=cwd`, `-save-temps=obj`

Honoured. Keep the preprocessed source (`.i`, or `.ii` for C++) and the
assembly (`.s`) of each C or C++ source, named as GCC 11 and later name
them:

| Command | Files kept |
|---|---|
| `embcc -save-temps -c x.c -o build/x.o` | `build/x.i`, `build/x.s` |
| `embcc -save-temps -c x.c` | `x.i`, `x.s` |
| `embcc -save-temps -S x.c -o build/y.s` | `build/y.i` (the `.s` is the output) |
| `embcc -save-temps x.c y.c -o fw.elf` | `fw-x.i`, `fw-x.s`, `fw-y.i`, `fw-y.s` |

`-save-temps=obj` is the same as `-save-temps`. `-save-temps=cwd` puts
the files in the current directory, named as above without the
directory. `-dumpbase NAME` names them `NAME.i` and `NAME.s`. EmbCC
passes nothing between its stages through files, so each is produced by
running the compiler again on the same source with `-E` and with `-S`;
the `.s` is the assembly of the object the command writes. A host that
cannot run a program (EmbCC built with `PROCESS=none`) refuses the
option. Nothing is kept for assembly inputs.

### `-pipe`

Accepted. EmbCC uses no temporary files between its stages.

### `-o FILE`

Write the output to `FILE`. The argument is the next word on the command
line; `-oFILE` is not accepted. If `-o` is the last argument the driver
stops with `embcc: -o needs a FILE`. When `-o` appears more than once, the
last one wins.

`-o -` means standard output for the text-producing modes (`-E`, `-S`,
`--emit-interfaces`, `--emit-c`). An object cannot be written to standard
output, and `-c -o -` is refused:

```text
embcc: error: `-o -` writes to stdout, which -E, -S and --emit-interfaces support but an object file does not; name a file
```

### `-j N`, `-jN`, `-j`

With several source files, compile `N` of them at once; `-j` alone, one
per processor. The default is one at a time. The objects and the image
are the same whatever `N` is; only the order the diagnostics come out in
can differ. See [Several source files](#several-source-files).

### `-x LANGUAGE`

Treat the input as `LANGUAGE` regardless of its suffix. The language may
be attached (`-xc++`) or the next argument (`-x c++`).

| `LANGUAGE` | Meaning |
|---|---|
| `c`, `cpp-output` | C |
| `c++`, `c++-cpp-output` | C++ |
| `none` | go back to deciding by suffix |

Any other value is refused with
`embcc: error: unknown language 'LANGUAGE' for -x (c or c++)`. Assembly
languages (`assembler`, `assembler-with-cpp`) are not accepted; use the
`.s` or `.S` suffix instead.

Unlike GCC, `-x` applies to the one input file wherever it appears. The
exception is an input whose suffix EmbCC does not recognize (`prog.txt`,
`prog.i`): such a file is only taken as the input when `-x c` or
`-x c++` comes before it on the command line.

### `-fsyntax-only`

Run the preprocessor, parser and semantic analysis, report diagnostics,
and write nothing. Implies `-c`. The exit status is 1 if any error was
reported.

### `--emit-c`

For a C++ input, write the C that EmbCC lowers the C++ unit to, instead
of compiling it. The output goes to `-o FILE` or to standard output. For a
C input the driver stops with `embcc: error: --emit-c lowers C++; 'FILE'
is C`.

### `--emit-interfaces`

Write the unit's interface record instead of an object: one line per
entity the unit provides or uses, each with a stable name (a USR) and a
hash of what dependents can observe of it. The output goes to `-o FILE`
or to standard output; `--emit-interfaces` implies `-c`. The record is
the input to the [`embidx`](tools/embidx.md) cross-unit index.

```text
; EmbCC interfaces v1
; t.c
file     21c568a73b4612ce t.c
provides c:@F@add                                 5336311b8eae8c73
```

A weak definition's `provides` line ends in ` weak` (see
[`embidx`](tools/embidx.md#units-usrs-and-hashes)).

### `--emit-empty-object FILE`

Write a valid relocatable ELF object with an empty `.text` section to
`FILE`. This option must be the first argument and `FILE` the only other
one; anything else is refused (`embcc: --emit-empty-object needs a FILE`,
or `unknown argument` when other options are present). The object is for
the configured [default target](#the-default-target), since
`--target=` cannot be combined with it. A default target whose object
format is not ELF or COFF is refused with
`embcc: error: no object writer for Mach-O yet, which is what 'TRIPLE' needs`.

### `--help`, `-h`

Print the usage line and a summary of the options, then exit with status
0. Recognized anywhere on the command line. The summary names the target
this compiler emits for when no `--target=` is given.

### `--help-warnings`

Print every warning option EmbCC has and the group (`-Wall` or `-Wextra`)
each belongs to, then exit with status 0. The list is reproduced under
[Warning options](#warning-and-diagnostic-options).

### `--version`

Print the version, the target in effect, the default target, the
targets and languages supported, which images `embld` links, and what is
not supported yet, then exit with status 0. Recognized anywhere on the
command line; a `--target=` on the same command line changes the target
it reports:

```text
EmbCC 1.0.0-m2.complete (a C and C++ compiler for EmbLinkOS and embedded boards)
Target: x86_64-elf
Default target: x86_64-elf
...
```

### `-dumpmachine`

Print the canonical name of the target in effect and exit. The name
reflects `--target=`, the configured default, and the ARM options that
change the sub-architecture or the float ABI:

```sh
embcc --target=arm-none-eabi -dumpmachine                  # thumbv7m-none-eabi
embcc --target=thumbv7m-none-eabi -mcpu=cortex-m4 \
      -mfpu=fpv4-sp-d16 -mfloat-abi=hard -dumpmachine     # thumbv7em-none-eabihf
```

All other options on the command line are still checked first, so an
invalid ARM option combination is reported instead of a name.

### `--dump-predef`

Print the target's table of predefined macros as `#define` lines and
exit. Like `-dumpmachine`, it is answered after every option has been
read, so `--target=`, `-mcpu=`, `-mfpu=` and `-mfloat-abi=` select the
table printed.

The table is the per-target set (architecture, data model, ABI). It does
not include the macros the preprocessor defines for every target
(`__STDC__`, `__STDC_VERSION__`, `__STDC_HOSTED__`, `__FILE__` and the
like), command-line `-D` definitions, the C++ macros, or
`__CHAR_UNSIGNED__` as changed by `-fsigned-char`/`-funsigned-char`. To
see exactly what a compile sees, preprocess a file that uses the macro.
The complete lists are in [Targets](targets.md).

### `--print-search-dirs`

Print where the driver found its own headers and libraries, then exit:

```text
layout: build tree
self: /home/me/EmbCC/embcc
lib: /home/me/EmbCC
include: /home/me/EmbCC/lib/libcxx/include
include: /home/me/EmbCC/lib/libc/include
include: /home/me/EmbCC/include
```

`layout` is `installed`, `build tree` or `none found`. With `none found`
the line `include: (none -- every header needs an explicit -I)` follows.
See [Directory search options](#directory-search-options).

### `--explain [ID]`, `--explain=ID`

Print the long explanation of diagnostic `ID` (`E0001` and up) and exit.
Every diagnostic that has an explanation prints its ID in brackets. With
no `ID`, list the IDs that have explanations. An unknown ID ends with
`embcc: no explanation for 'ID'; `embcc --explain` lists what there is`
and status 1. See [Diagnostics](diagnostics.md).

### Exit status

`embcc` exits with status 0 when the requested output was produced and no
error was reported, and 1 otherwise. A warning promoted by `-Werror` or
`-Werror=NAME` counts as an error, and the compile then leaves no output
file (see [`-Werror`](#-werror--wno-error)). With `--fix`, the status is 0
when at least one fix was applied.

## C and C++ language options

### `-std=STANDARD`

Name the language standard. EmbCC has one C dialect, C11 with the GNU
extensions, and parses C++ as C++20. `-std=` never changes what the parser
accepts.

For C, the accepted names are `c89`, `c90`, `iso9899:1990`, `gnu89`,
`gnu90`, `c99`, `c9x`, `iso9899:1999`, `gnu99`, `gnu9x`, `c11`, `c1x`,
`iso9899:2011`, `gnu11`, `gnu1x`, `c17`, `c18`, `iso9899:2017`, `gnu17`,
`gnu18`, `c23`, `c2x`, `gnu23` and `gnu2x`. They are accepted and change
nothing: `__STDC_VERSION__` is `201710L` and `__STRICT_ANSI__` is not
defined, whichever is given. A standard older than C11 also prints

```text
embcc: warning: -std=c99 is accepted but not enforced; EmbCC has one C dialect, C11 with the GNU extensions, and will compile newer constructs anyway
```

For C++, the accepted names are `c++NN` and `gnu++NN`, where `NN` is one
of `98`, `03`, `11`, `0x`, `14`, `1y`, `17`, `1z`, `20`, `2a`, `23`, `2b`,
`26` and `2c`. These set the standard macros that libraries choose code
by: `__cplusplus` (`199711L`, `201103L`, `201402L`, `201703L`, `202002L`,
`202302L`, or `202400L` for C++26), the feature-test macros, and
`__STRICT_ANSI__` for the `c++NN` spellings. The parser still reads C++20.
A year before 2020 prints

```text
embcc: warning: -std=c++17 sets the standard macros but is not enforced; EmbCC parses C++20 and will accept newer constructs
```

The default for C++ is equivalent to `-std=gnu++20`.

One C standard changes code generation: `c89`, `c90`, `iso9899:1990`,
`gnu89` and `gnu90` select GNU89 `inline` semantics, as
[`-fgnu89-inline`](#-fgnu89-inline) does.

### `-fgnu89-inline`, `-fno-gnu89-inline`

Select the meaning of `inline` on a function with external linkage.
The default, `-fno-gnu89-inline`, is C99's: a definition whose
declarations all say `inline` and none `extern` is an inline definition,
which is never emitted. `-fgnu89-inline` selects GNU89's: `inline` alone
is an external definition, and `extern inline` is never emitted. See
[Inline functions](c-language.md#inline-functions).

With `-std=c89`, `c90`, `iso9899:1990`, `gnu89` or `gnu90`, GNU89
semantics apply and `-fno-gnu89-inline` does not change them, as with
Clang.

Any other name is refused: `embcc: error: unknown standard '-std=NAME'`,
or `embcc: error: unknown C++ standard '-std=NAME'` for a malformed
`c++`/`gnu++` name.

`-ansi` is not accepted (unknown argument). `-pedantic` and
`-pedantic-errors` are accepted with a warning and change nothing (see
[`-pedantic`](#-pedantic--pedantic-errors)). `-Wpedantic` is accepted as a
warning name EmbCC does not have (see [`-WNAME`](#-wname)).

### `-fsigned-char`, `-funsigned-char`

Make plain `char` signed or unsigned, overriding the target's default
(signed on x86-64, macOS on AArch64, and AVR; unsigned on the other
AArch64 targets, ARM and RISC-V; see [Targets](targets.md)). Honoured: the
predefined macro `__CHAR_UNSIGNED__` is defined or removed to match. The
last of the two on the command line wins.

### `-ffreestanding`

Accepted. EmbCC makes no hosted assumptions to turn off: it treats no
library function name as a builtin. `__STDC_HOSTED__` stays defined as 1
on every target, with or without this option.

### `-fno-builtin`, `-fno-builtin-NAME`

Accepted. EmbCC recognizes no library function name (`memcpy`, `printf`)
as a builtin; only the `__builtin_` spellings are builtins. A call to
`memcpy` is a call, so the per-name form asks for nothing more.

### `-fwrapv`

Accepted. Signed integer overflow wraps in two's complement on every
target, and no optimization assumes it cannot happen. `-fno-strict-overflow`
says the same and is accepted too. `-fno-wrapv`, `-ftrapv` and
`-fstrict-overflow` are not accepted.

### `-fstrict-aliasing`, `-fno-strict-aliasing`

Accepted. EmbCC's alias analysis is not type-based, so code is compiled
as `-fno-strict-aliasing` would compile it in either case.

### `-fcommon`, `-fno-common`

`-fno-common` is the default: a tentative definition (`int x;` at file
scope, with no initializer) is a definition in `.bss`, and two units
that both have one do not link (`embld: multiple definition of 'x'`).

`-fcommon` is honoured for C on the ELF targets. A tentative definition
is emitted as a COMMON symbol (`SHN_COMMON`, its value the alignment),
as GCC emits it, and `embld` merges every unit's COMMON symbols of one
name into one object with the largest size and the strictest alignment,
or drops them for a real definition elsewhere. A `static`, thread-local
or weak tentative definition, or one with a `section` attribute, stays a
definition, as in GCC. A linker script must place `*(COMMON)` (in
`.bss`, where the startup code zeroes it); `embld` says so if it does
not. `-S` writes `.comm NAME,SIZE,ALIGN`. C++ has no tentative
definitions, so the option changes nothing there. For a Mach-O or COFF
target it is refused: `embcc: error: -fcommon is not supported for
x86_64-apple-darwin: EmbCC writes COMMON symbols into ELF objects only,
and this target's are Mach-O`.

### `-fno-short-enums`

Accepted. An enumeration is `int`-sized on every target unless its values
need a wider type; this is the layout the option asks for.
`-fshort-enums` is [refused](#refused-options).

### `-fsingle-precision-constant`

Honoured, for C. A floating constant without a suffix has type `float`
and the value it has as a `float`, as with GCC's option:
`sizeof(1.0)` is 4, `x * 0.1` with `float x` is a `float` multiply, and
`double d = 0.1;` holds `0.1f` widened. A constant beyond the range of
`float` becomes an infinity, and one too small becomes zero, with a
warning (`floating constant exceeds the range of 'float'`). `L`-suffixed
constants are unchanged. For a C++ input it is refused (`supported for
C, not C++`), because the C++ front end types its own constants.
`-fno-single-precision-constant` is the default.

### `-fchar8_t`

C++ only. Define `__cpp_char8_t` when the standard selected by `-std=` is
earlier than C++20. In C++20 mode `char8_t` is a keyword already.

### `-fexceptions`, `-fno-exceptions`

C++ exceptions are on by default. `-fno-exceptions` turns them off as
g++'s flag does. `-fexceptions` turns them on and also requests unwind
tables, which makes it useful for C code that C++ exceptions must unwind
through (see [unwind tables](#-funwind-tables--fasynchronous-unwind-tables)).

### `-frtti`, `-fno-rtti`

Run-time type information is on by default for C++. `-fno-rtti` turns it
off.

### `-faccess-control`, `-fno-access-control`

`-fno-access-control` stops enforcing `private` and `protected` in C++,
as GCC's option does. `-faccess-control` restores the default.

### Language options that are not accepted

`-fshort-enums` is [refused](#refused-options): on every target an
enumeration is `int`-sized unless its values need a wider type (see
[Targets](targets.md#data-models)), and a structure containing one would
be laid out differently. `-fshort-wchar`, `-fms-extensions`, `-fno-asm`
and `-fvisibility=...` are not accepted (unknown argument).

## Warning and diagnostic options

The diagnostic format, the warnings themselves, `--explain` and fix-its
are described in [Diagnostics](diagnostics.md). This section lists the
options.

### `-w`

Suppress all warnings. Takes precedence over every `-W` option, including
`-Werror`.

### `-Werror`, `-Wno-error`

`-Werror` reports every warning as an error, and the compile fails.
`-Wno-error` turns that off again; the last one wins.

A compile that fails because of a warning made an error writes no output:
the object, assembly file or executable it was producing is removed, so
that `make` does not take it as up to date. A file of that name left by
an earlier compile is removed as well. The dependency file of `-MD` or
`-MMD` and the `.su` file of `-fstack-usage` are still written.

### `-Werror=NAME`, `-Wno-error=NAME`

`-Werror=NAME` reports the warning `NAME` as an error, with or without
`-Werror`, and turns the warning on. `-Wno-error=NAME` keeps `NAME` a
warning under `-Werror`; it does not turn the warning on. Either form
decides for its one warning whatever `-Werror` and `-Wno-error` say, and
wherever it appears on the command line. `-w` still suppresses the
warning. A `NAME` that is not a warning EmbCC has is reported and
ignored:

```text
embcc: warning: -Werror=cast-align names no warning EmbCC has (--help-warnings lists them)
```

The warnings that no option controls (see
[Diagnostics](diagnostics.md#warnings-no-option-controls)) follow
`-Werror` alone.

### `-Wall`, `-Wextra`, `-W`

Turn on the warnings in the `-Wall` or `-Wextra` group. `-W` is the old
spelling of `-Wextra`. As in GCC, `-Wextra` does not include `-Wall`.

### `-WNAME`

Turn on the warning `NAME`. A name EmbCC does not have is not an error:

```text
embcc: warning: -Wcast-align is not a warning EmbCC has, so it turns nothing on (--help-warnings lists them)
```

Any argument beginning with `-W` that matches nothing else is handled
this way, including `-Wformat=2`. `-Wa,...`, `-Wl,...` and `-Wp,...` are
options of their own (see
[Assembler and linker options](#assembler-and-linker-options) and
[`-Wp,ARGS`](#-wpargs)).

`-Wimplicit-function-declaration`, `-Werror=implicit-function-declaration`
and GCC's older `-Werror-implicit-function-declaration` are accepted
without a word: in EmbCC a call to an undeclared function is always an
error (`'g' is not declared in 'f'`, E0001), which is what they ask for.
`-Wno-error=implicit-function-declaration` cannot make it a warning, and
says so.

### `-Wno-NAME`

Turn off the warning `NAME`. An unknown `NAME` is accepted silently.
`-Wno-error=NAME` is a different option; see above.

### `-Wsystem-headers`

Report warnings in system headers. By default a warning whose location is
in a header found through `-isystem` or through EmbCC's own include
directories is not reported.

### `-pedantic`, `-pedantic-errors`

Accepted, with a warning that they turn nothing on: EmbCC has no
diagnostics for extensions to ISO C.

```text
embcc: warning: -pedantic: EmbCC has no diagnostics for extensions to ISO C, so this turns nothing on
```

The warning is printed when the command line is read; `-w` does not
suppress it and `-Werror` does not make it an error.

### The warnings

| Option | Group | On by default |
|---|---|---|
| `-Waddress` | `-Wall` | no |
| `-Wattributes` | | yes |
| `-Wdeprecated-declarations` | | yes |
| `-Wdiv-by-zero` | | yes |
| `-Wformat` | `-Wall` | no |
| `-Wlogical-op` | `-Wextra` | no |
| `-Wmaybe-uninitialized` | `-Wall` | no |
| `-Wparentheses` | `-Wall` | no |
| `-Wshadow` | | no |
| `-Wshift-count-overflow` | `-Wall` | no |
| `-Wsign-compare` | `-Wextra` | no |
| `-Wtype-limits` | `-Wextra` | no |
| `-Wuninitialized` | `-Wall` | no |
| `-Wunused-function` | `-Wall` | no |
| `-Wunused-parameter` | `-Wextra` | no |
| `-Wunused-result` | | yes |
| `-Wunused-variable` | `-Wall` | no |
| `-Wwindows-abi` | | yes |

For a C++ input, `-Wunused-variable`, `-Wunused-parameter`,
`-Wunused-function`, `-Wshadow` and `-Wsign-compare` are always off, even
when named on the command line, because they would be reported against
code the C++ lowering generated.

### `-fdiagnostics-format=FORMAT`

`text` (the default) prints caret diagnostics. `json` prints the
diagnostics as one JSON array in GCC's format, on standard error. Any
other value is refused with `embcc: unknown diagnostic format 'FORMAT'
(text, json)`.

### `-fdiagnostics-color[=WHEN]`, `-fno-diagnostics-color`

Control color in text diagnostics. `-fdiagnostics-color` and
`-fdiagnostics-color=always` force it on; `-fdiagnostics-color=never` and
`-fno-diagnostics-color` turn it off. Any other value, including `auto`,
selects the default: color when standard error is a terminal. Clang's
`-fcolor-diagnostics` spelling is not accepted.

### `-fmessage-length=N`

Accepted. EmbCC does not wrap diagnostics, which is what
`-fmessage-length=0` asks for; other values change nothing.

### `-fanalyzer`

Accepted with a warning: `embcc: warning: -fanalyzer: EmbCC has no
static analyzer, so this checks nothing`.

### `-fmax-errors=N`

Stop after `N` errors with `embcc: compilation terminated due to
-fmax-errors=N`. `0`, the default, means no limit. Clang's
`-ferror-limit=` is not accepted.

### `-fdiagnostics-parseable-fixits`

After each fix-it hint, also print it in the machine-readable form Clang
uses:

```text
fix-it:"bad.c":{2:36-2:36}:";"
```

### `--fix`

Apply the fix-its the diagnostics propose, editing the source file in
place, and write no object. Implies `-fsyntax-only`. When it changed
something the driver reports `embcc: 1 fix applied; compile again` (or
`N fixes`) and exits with status 0; otherwise it reports
`embcc: nothing to fix automatically` and exits with the compile's
status.

## Debugging options

### `-g`

Emit DWARF version 4 debug information (`.debug_info`, `.debug_abbrev`,
`.debug_line`) on every ELF target. See [Debugging](debugging.md).

`-g1`, `-g2`, `-g3`, `-ggdb`, `-gdwarf`, `-gdwarf-2`, `-gdwarf-3` and
`-gdwarf-4` are all accepted and mean `-g`: EmbCC emits one kind of debug
information, so a level or version that asks for no more than DWARF 4
gets exactly that.

`-g` can change the code generated. On some targets it keeps a frame and
a stack home for each variable, and it disables tail calls on ARMv7-M, so
that the debugger can find every variable. Build with the same `-g`
setting you ship with if you compare code size.

`-g` is refused in three situations, each with an error naming the cause:

- for the `-apple-darwin` targets (`-g is not supported for a Darwin
  target yet: its DWARF goes in a __DWARF segment this does not write ...`);
- for `x86_64-windows-gnu` (`-g is not supported for a Windows target
  yet: its debug information goes in CodeView records this does not
  write ...`);
- for a unit that places a function in a named section with
  `__attribute__((section))` (`-g with a function in a section of its
  own ('NAME') is not supported yet ...`).

### Refused debugging options

A DWARF version EmbCC does not emit, split DWARF and compressed debug
sections are refused rather than downgraded:

```text
embcc: error: -gdwarf-5 is not supported; EmbCC emits DWARF 4, uncompressed and in one piece
```

The same message is given for `-gsplit-dwarf` and `-gz`. `-g0` and
`-gline-tables-only` are not accepted (unknown argument); to compile
without debug information, omit `-g`.

## Optimization options

What each level and pass does is described in
[Optimization](optimization.md). This section lists the options.

### `-O0`

No optimization. This is the default. The optimizer does not run, so the
[per-pass options](#-fpass--fno-pass) have no effect at `-O0`. Every
source variable stays in its stack slot; on ARM Cortex-M and RISC-V the
temporaries of expressions get registers (see
[Optimization](optimization.md#-o0)).

### `-O`, `-O1`

Optimize without making the code larger, as GCC's and Clang's `-O1` do.
Every backend allocates registers, with tail calls where its conditions
allow them, and the optimizer runs the passes that only remove work:
the local passes, `cfg-clean`, `mem2reg`, `sroa`, `sccp`, `dse`,
`load-cse`, `licm`, `if-convert` and `div-magic`, and inlines a `static`
function with a single caller and an `always_inline` function. Static
functions that are unreachable after optimization are not emitted.

### `-O2`

Everything in `-O1`, plus every other pass in the
[pass table](#-fpass--fno-pass) (the vectorizer on x86-64 only): full
inlining, `gcse`, `pre`, `unroll`, `switch-thread`, `tail-recursion` and
`idiom`.

### `-O3`

The same as `-O2`. The output is identical.

### `-Os`, `-Oz`

Optimize for size: `-O2` with the `vectorize`, `unroll` and
`switch-thread` passes off, a smaller inlining budget, and size-directed
choices in IR generation and in the backends. `-Oz` is the same as
`-Os`.

### `-Og`

The same as `-O1`: the level that removes work without reorganizing the
program.

### `-Ofast`

The same as `-O3` (and so `-O2`). GCC's `-Ofast` adds `-ffast-math`,
which EmbCC does not do (see [`-ffast-math`](#-ffast-math--fno-math-errno)),
so no result differs from `-O3`'s.

Any other `-O` spelling (`-O4`, `-Ox`) is refused with
`embcc: unknown optimization flag '-O4'`.

### `-fPASS`, `-fno-PASS`

Turn one optimizer pass on or off, overriding what the `-O` level chose.
A pass turned on this way also runs at `-O1`, which lets one pass be tried
on its own. The pass names are:

| Pass | Default at | What it does |
|---|---|---|
| `mem2reg` | `-O1` | promotes local variables to SSA values |
| `gcse` | `-O2` | dominator-scoped global common-subexpression elimination |
| `load-cse` | `-O1` | global redundant-load elimination |
| `sccp` | `-O1` | resolves constant branches and drops dead blocks |
| `licm` | `-O1` | loop-invariant code motion, loop rotation and strength reduction |
| `vectorize` | `-O2`, x86-64 only, not `-Os` | lane-wise loops |
| `inline` | `-O1` (a single caller or `always_inline` only), `-O2` | inlines small callees into their callers |
| `dse` | `-O1` | removes a store a later store overwrites |
| `div-magic` | `-O1` | divides by a constant without a divide instruction |
| `if-convert` | `-O1` | replaces a two-way branch with a select |
| `cfg-clean` | `-O1` | threads jumps and removes unreachable code |
| `tail-recursion` | `-O2` | turns a self tail call into a loop |
| `idiom` | `-O2` | turns a copy or clear loop into `memcpy`/memory clear |
| `sroa` | `-O1` | splits a private aggregate into scalars |
| `unroll` | `-O2`, not `-Os` | unrolls counted loops |
| `pre` | `-O2` | partial-redundancy elimination |
| `switch-thread` | `-O2`, not `-Os` | jumps from a known state straight to its `switch` case |

An `-f` or `-fno-` option that is not a pass name and not listed elsewhere
on this page is an unknown argument. GCC's pass options are accepted only
where this page lists them (below); `-funroll-loops` and
`-ftree-vectorize`, for example, are not.

### `-fno-inline-functions`, `-finline-functions`

`-fno-inline-functions` is honoured: the inliner takes only functions
declared `inline` in some declaration, or marked `always_inline`, at
every level. A function its author did not mark stays a call (and a
symbol in the image) at `-O2` and `-Os` as at `-O1`, including a static
function with a single caller, which GCC's option would still inline
(`-finline-functions-called-once`). `embcc why not-inlined` reports
`not-declared-inline` for such a call. C++ member functions defined in
their class are not marked `inline` in the C the C++ front end lowers to,
so under this option they are not inlined. `-finline-functions`, GCC's
`-O2` default, restores the default.

### `-finline-small-functions`, `-fno-inline-small-functions`, `-finline-limit=N`

Accepted and without effect. They are hints to GCC's inliner, whose
budgets EmbCC's does not share (see [Optimization](optimization.md)).

### `-ffast-math`, `-fno-math-errno`

Accepted and without effect. Both give the compiler permission to relax
IEEE arithmetic or to assume the math library sets no `errno`; EmbCC
uses neither permission. `__FAST_MATH__` is not defined, so a header
that tests it takes its careful path, as it should for code that is not
compiled fast-math.

### `-fno-delete-null-pointer-checks`

Accepted. EmbCC already keeps the promise: no pass infers that a pointer
is non-null -- not from a dereference before the check, not from the
`nonnull` and `returns_nonnull` attributes (which are ignored), not from
its being an object's address -- so no comparison with a null pointer is
removed, and a load from address 0 stays a load.

### `-fno-tree-loop-distribute-patterns`

Accepted. No loop ever becomes a call. The `idiom` pass turns a copy or
clear loop with a constant trip count into a block copy or clear, and
every backend expands those inline, as it does structure copies, so a
hand-written `memset` or `memcpy` cannot become a call to itself. No
EmbCC object refers to `memset`, `memcpy` or `memmove` unless the source
calls them.

### `-fmerge-constants`

Accepted and without effect: it permits merging identical constants
across units, and EmbCC does not.

### `-fno-isolate-erroneous-paths-dereference`, `-fno-move-loop-invariants`, `-fno-ipa-sra`, `-fno-lto`

Accepted and without effect. EmbCC turns no null dereference into a
trap, and has no passes under the other three names; `-fno-lto` is the
default (`-flto` is [refused](#refused-options)). EmbCC's loop-invariant
motion is the `licm` pass, turned off with `-fno-licm`.

### `-fremarks`, `-fremarks=json`

Report what the optimization passes decided and why. The report is
written to standard error after the compile, one line per decision:

```text
w.c:3: remark: inlined 'sq': 4 instructions into f, budget 24 [inline/small-enough]
w.c:1: remark: promoted-to-register 's' [mem2reg/scalar-and-never-addressed]
w.c:1: remark: unrolled 'big': 8 copies of a 5-instruction body, the original kept for the remainder [unroll/counted-loop]
```

`-fremarks=json` writes the same records as a JSON array of objects with
the keys `pass`, `decision`, `subject`, `reason`, `detail` and
`location`. Most decisions are made only at `-O2`. Clang's `-Rpass=`,
`-fsave-optimization-record` and GCC's `-fopt-info` are not accepted. See
also [`embcc why`](#embcc-why-decision-subject-file-option).

## Instrumentation options

### `-fsanitize=LIST`, `-fno-sanitize=LIST`

Insert run-time checks for undefined behavior. `LIST` is comma-separated;
the checks EmbCC has are:

| Name | Checks |
|---|---|
| `signed-integer-overflow` | signed `+`, `-`, `*` that overflow |
| `integer-divide-by-zero` | integer division and remainder by zero |
| `shift`, `shift-exponent` | a shift count out of range |
| `undefined` | all three of the above |

A failed check executes the target's trap instruction: under a debugger
that stops at the faulting operation, and without one the program stops.
There is no diagnosing runtime, so trap mode is the only mode. At `-O2`
the optimizer removes checks whose operands it can prove safe.

`-fno-sanitize=LIST` removes checks from the set. Options accumulate in
command-line order.

A check EmbCC does not have is refused by name:

```text
embcc: error: -fsanitize=address is not supported: EmbCC's sanitizer inserts checks that TRAP, and this one needs a runtime library to report through. The ones it has are undefined, signed-integer-overflow, integer-divide-by-zero and shift
```

### `-fsanitize-trap`, `-fsanitize-trap=LIST`, `-fsanitize-undefined-trap-on-error`

Accepted, since trapping is the only mode. Note one difference from GCC
and Clang: `-fsanitize-trap=LIST` with a list also enables the checks it
names, as `-fsanitize=LIST` would. An unknown name in the list is reported
with the `-fsanitize=NAME` message above.

Every other option beginning with `-fsanitize` (for example
`-fsanitize-recover=...`) is [refused](#refused-options).

### `-fstack-usage`

Write a stack-usage file beside the output: the output name with its
suffix replaced by `.su` (`foo.o` gives `foo.su`). It has one line per
function that was emitted, in GCC's format:

```text
w.c:3:f	8	static
```

The fields are `FILE:LINE:FUNCTION`, the frame size in bytes, and the
qualifier, which is always `static`. Functions that inlining absorbed or
that were dropped as unreachable are not listed.

The `.su` name is derived from the output file, so `-fstack-usage` with
`-S` needs `-o FILE`.
<!-- Lead: `embcc -fstack-usage -S foo.c` (no -o) crashes with a
segmentation fault in this build (out is NULL in the .su naming code in
compile_unit). Remove the sentence above once fixed. -->

### `-fno-stack-protector`

Accepted: EmbCC emits no stack protection. Every `-fstack-protector`
form is refused:

```text
embcc: '-fstack-protector-strong' is not supported (EmbCC emits no stack protection); -fno-stack-protector is
```

## Preprocessor options

The preprocessor itself is described in [C language](c-language.md) and
[Extensions](extensions.md).

### `-Wp,ARGS`

Pass `ARGS`, split at the commas, to the preprocessor. Each must be `-D`,
`-U` or `-I` with its value joined to it, and is applied where the
`-Wp` option stands among the others, so `-DY -Wp,-DX=1,-UY` defines `X`
and leaves `Y` undefined. Any other preprocessor option is refused:

```text
embcc: error: preprocessor option '-MD' in -Wp,-MD,dep.d is not supported; pass -D, -U or -I with its value, or give the option directly
```

### `-D NAME`, `-D NAME=VALUE`

Define a macro, as `#define NAME 1` or `#define NAME VALUE`. The argument
may be attached (`-DNAME`) or the next word (`-D NAME`). With no argument
the driver stops with `embcc: -D needs a macro name`.

### `-U NAME`

Remove a predefined or earlier `-D` definition of `NAME`. `-D` and `-U`
are applied in command-line order: `-DFOO -UFOO` leaves `FOO` undefined,
and `-UFOO -DFOO` leaves it defined.

### `-M`

Instead of compiling, write a `make` rule naming the input and every
header it includes, to standard output (or to the `-MF` file). Use it with
`-c` or `-E`: without one of them the driver goes on to link and fails.

```sh
$ embcc -M -c m.c
m.o: m.c \
  /opt/embcc/lib/embcc/1.0.0-m2.complete/include/stdio.h \
  /opt/embcc/lib/embcc/1.0.0-m2.complete/freestanding/stddef.h \
  /opt/embcc/lib/embcc/1.0.0-m2.complete/freestanding/stdarg.h \
  loc.h
$ embcc -MM -c m.c
m.o: m.c loc.h
```

The rule's target is the `-MT` value if one is given. Otherwise, with
`-c`, it is the object's name (the `-o` name or the default object name),
and with `-E` it is the input's file name with its suffix replaced by
`.o`. Long rules are wrapped at about 72 columns with a backslash.

### `-MM`

Like `-M`, but leave out headers found in system directories (those added
with `-isystem` and EmbCC's own).

### `-MD`, `-MMD`

Compile normally and also write the rule (all headers, or non-system
headers only) to a file named after the object with its suffix replaced
by `.d`, or to the `-MF` file. The rule is written once the unit has
passed semantic analysis.

### `-MF FILE`

Write the rule to `FILE`. `FILE` is the next argument; with none the
driver stops with `embcc: -MF needs a file name`.

### `-MT TARGET`, `-MQ TARGET`

Use `TARGET` as the rule's target. The two are identical in EmbCC:
`-MQ` does not quote characters special to `make`. When given more than
once, the last one is used (GCC would list them all).

### `-MP`

Also write an empty rule for each header, so that `make` does not fail
when a header is deleted.

### `-include FILE`

Read `FILE` as if `#include "FILE"` were the first line of the source
file. `FILE` is looked for in the working directory first, as GCC does,
then on the include path. Several are read in the order given. A file
that cannot be found is refused:
`cannot find -include file "FILE"`.

### Preprocessor options that are not accepted

`-imacros`, `-iquote`, `-idirafter`,
`-iprefix`, `-dM`, `-dD`, `-C`, `-CC`, `-P`, `-H`, `-MG`,
`-trigraphs` and `-undef` are not accepted either. Use `--dump-predef`
to list predefined macros.

## Directory search options

### `-I DIR`

Add `DIR` to the list of directories searched for headers. The argument
may be attached (`-Iinclude`) or the next word. With no argument the
driver stops with `embcc: -I needs a directory`.

### `-isystem DIR`

Add `DIR` as a system header directory: searched in command-line order
with the `-I` directories, but its headers do not produce warnings (unless
`-Wsystem-headers`) and are left out of `-MM` and `-MMD` rules.

### `-nostdinc`

Do not search EmbCC's C library, C++ library and freestanding header
directories. See the note below on the one directory this does not
remove.

### Search order

For `#include "file"` the directory of the including file is searched
first. Then, for both forms of `#include`, the directories are searched
in this order:

1. every `-I` and `-isystem` directory, in command-line order;
2. the directory `include` beside the path the compiler was invoked as:
   for `/opt/embcc/bin/embcc` that is `/opt/embcc/bin/include`; for a bare
   `embcc` found through `PATH` it is `./include` in the current working
   directory;
3. unless `-nostdinc` is given, EmbCC's own directories, all treated as
   system directories:
   the C++ library headers, the C library headers, and the freestanding
   headers (`<stddef.h>`, `<stdarg.h>`, `<stdint.h>`, `<limits.h>`,
   `<float.h>` and the rest).

Directory 2 is searched as a system directory and is added even with
`-nostdinc`. In a build tree it is EmbCC's freestanding header directory,
so `<stddef.h>` and `<stdint.h>` remain reachable under `-nostdinc` there.
When `embcc` is run through `PATH` from a directory that has an `include`
subdirectory, headers there are found as system headers.

Project headers therefore take precedence over EmbCC's: a project that
ships its own `<stdio.h>` in an `-I` directory gets its own.

The search list holds at most 16 directories. EmbCC's four are appended
only while there is room, so with more than 12 `-I`/`-isystem` options
some of them are left out without a message, and a 17th `-I` is refused
with `embcc: too many -I directories` (`embcc: too many include
directories` for `-isystem`).

### How the compiler finds its own files

EmbCC locates its headers and per-target libraries relative to its own
executable, so a build tree and an installation both work without `-I`:

1. If the environment variable `EMBCC_PREFIX` is set and
   `$EMBCC_PREFIX/lib/embcc/VERSION/include/stdio.h` exists, that
   installation is used. If the variable is set and the file does not
   exist, the driver prints
   `embcc: EMBCC_PREFIX=DIR has no lib/embcc/VERSION/include` and goes on
   to the next step.
2. If the directory containing the executable has
   `lib/libc/include/stdio.h`, it is a build tree, and its `lib/` and
   `build/` directories are used.
3. Otherwise, if `BINDIR/../lib/embcc/VERSION/include/stdio.h` exists,
   that installation is used.

`VERSION` is the string `--version` prints (`1.0.0-m2.complete`). The
executable's own path is read from the operating system on macOS and
Linux; on other hosts none of the steps after the first can apply, and
every header needs an explicit `-I`. `--print-search-dirs` shows the
result. The installed layout is described in
[Getting started](getting-started.md#installing).

## Assembler and linker options

### Assembly input

A `.s` or `.S` file is assembled by EmbCC's built-in GNU-syntax assembler
for the selected target: AArch64, ARM (Thumb), RISC-V or AVR. A `.S` file
is preprocessed first; `-D` and `-U` apply, but the `-I` and `-isystem`
directories are not searched (a `#include "file"` beside the source is
found). The inline-assembly vocabulary of each target is listed in
[Inline assembly](inline-asm.md).

The assembler follows GNU as for each target:

- **Comments.** `/* ... */` anywhere; `#` starts a comment, except on
  ARM and AArch64, where it is an immediate's prefix (`mov r0, #1`) and
  starts a comment only as a line's first character. `@` is ARM's comment
  character and `;` AVR's (elsewhere `;` separates statements); `//`
  works everywhere.
- **Macros and expressions.** `.macro`, `.rept`, `.irp`, the `.if`
  family, `.include`, `.equ`/`.set`/`.thumb_set`, named sections with
  flags, and GNU as's expressions; the full list is in
  [embas](tools/embas.md#gnu-syntax-assembly).
- **Alignment.** `.align N` and `.p2align N` align to 2^N bytes, and
  `.balign N` to N bytes. Padding in code is the target's no-op
  instruction, and zero bytes in data.
- **Operand words.** A special register, interrupt mask, IT condition,
  barrier option or floating-point register on ARM, and a CSR name or a
  fence set on RISC-V, is an operand and never a symbol.
- **Symbols.** On ARM, a symbol is reached by `bl sym`, `b sym`,
  `ldr rd, =sym` (a load from the literal pool, as GNU as makes it),
  `movw rd, #:lower16:sym`, `movt rd, #:upper16:sym` and `.word sym`. `ldr rd, label` loads a word
  from a label defined in the same file. `.thumb_func`, or
  `.type sym, %function`, makes a label a Thumb function: its symbol
  carries the interworking bit, which a vector table entry needs. A symbol
  the file uses but does not define is an external reference.
- **Sizes.** `.size sym, .-sym` records a function's size, which a
  debugger uses to attribute addresses to it.

For x86-64 targets, GNU-syntax assembly files are refused:

```text
embcc: error: no assembly-file support for x86_64-elf yet; its instruction encoder exists (inline __asm__ works) but this driver has not been wired to it
```

A `.asm` file is assembled as NASM/Intel-syntax x86-64 into an x86-64
ELF64 object. This is the same assembler as the standalone
[`embas`](tools/embas.md). For any target other than x86-64 with ELF
objects, a `.asm` input is refused:

```text
embcc: error: 'boot.asm' is NASM-syntax x86-64 assembly, which EmbCC assembles to x86-64 ELF only, and the target is thumbv7m-none-eabi
```

An assembly input always produces an object: `-c` and `-S` make no
difference, and no link follows. The one other mode is `-E`, which
preprocesses a `.S` file to standard output. Without `-o` the object
is the input's file name with its suffix replaced by `.o`, in the
current directory (`boot/start.S` gives `start.o`).

### `-Wa,ARGS`

Options for the assembler, separated by commas. The integrated assembler
takes no options, so only those that change nothing it writes are
accepted: `--noexecstack`, `-g`, any option beginning with `--gdwarf`, and
`-mrelax`. Any other is refused, and nothing is compiled:

```text
embcc: error: assembler option '-adhln' is not one the integrated assembler has
```

The options are checked on every invocation, whatever the input file.

### Linking

Without `-c`, `-S`, `-E` or `-fsyntax-only`, `embcc FILE -o OUT` compiles
the file and links it in the same process with EmbCC's linker,
[`embld`](tools/embld.md). This is available only for x86-64 ELF targets
(`x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu` and their aliases).
For the firmware targets (ARMv7-M, ARMv8-M, RV32, RV64 and AVR) the
driver links too, and the build supplies the memory map: a linker script
with `-T FILE.ld` (ARM and RISC-V), or `-Wl,-Ttext=FLASH` and
`-Wl,-Tdata=RAM`. There is no default map, because an image linked to a
guessed one runs, wrongly:

```text
embcc: error: linking a thumbv7em-none-eabi image needs its memory map: a linker script (-T FILE.ld), or -Wl,-Ttext=FLASH and -Wl,-Tdata=RAM
```

```sh
embcc --target=thumbv7em-none-eabi -Os -c main.c -o main.o
embcc --target=thumbv7em-none-eabi -T STM32F407VGTx_FLASH.ld \
      startup.o main.o -o fw.elf
```

`embld` does not read AArch64, Mach-O or COFF objects, and for those
targets the driver stops with `embcc: error: cannot link for TRIPLE`; link
them with the platform's linker.

The link line, in order:

1. `crt1.o` from the target's library directory, for a hosted target
   (not with `-nostdlib` or `-nostartfiles`; a firmware target has none,
   because its startup is the program's own);
2. the object just compiled (written to `OUT.embcc-tmp.o` beside the
   output and removed afterwards; when `embld` itself refuses the link,
   for an undefined symbol or an image over `--rom-limit`, the temporary
   object is left in place);
3. the objects, archives and `-l` libraries of the command line, in
   their order;
4. `libcxx.a`, for a C++ input;
5. `libc.a`, if the target has one (not with `-nostdlib` or
   `-nodefaultlibs`);
6. `librt.a`, the compiler runtime, if the target has one (likewise).

`-lNAME` is `libNAME.a` in the first `-L` directory that has it. `-lc` and
`-lm` name EmbCC's own `libc.a` (its math is in it) and `-lgcc` its
`librt.a`, so a link line written for `arm-none-eabi-gcc` that says
`-nostdlib ... -lc -lgcc` gets them. A library found nowhere is refused:
`embcc: error: cannot find libnosys.a for -lnosys in any -L directory`.

For `x86_64-linux-gnu` the result is a static executable with no
dependence on another C library. A missing `crt1.o` or `libc.a` for that
target is reported by name (`embcc: error: no crt1.o for
x86_64-linux-gnu -- the target's library is not built or not installed`),
as is a missing `libcxx.a` for a C++ program. For the freestanding x86-64
targets nothing supplies the entry point: the program must define
`_start`, or be linked with `embld` and a start file.

The image is written without execute permission; run `chmod +x` on it
before running it directly.

### `-Wl,ARGS`, `-Xlinker ARG`

Pass options to the link. `-Wl,` splits `ARGS` at its commas, so
`-Wl,-Ttext,0x200000` is two words; `-Xlinker` passes the next argument as
one word, and with none the driver stops with `embcc: -Xlinker needs an
option`. The words are kept in command-line order and read when the
driver links. A compile that does not link (`-c`, `-S`, `-E`,
`-fsyntax-only`) ignores them, as GCC does.

These are applied, with the meaning of the [`embld`](tools/embld.md)
option of the same name:

| Linker option | Effect |
|---|---|
| `-Ttext ADDR`, `-Ttext=ADDR`, `-TtextADDR`, `-Ttext-segment ADDR`, `-Ttext-segment=ADDR` | the address the image starts at (default `0x400000`) |
| `-Tdata ADDR`, `-Tdata=ADDR`, `-TdataADDR` | the address of the writable data |
| `--rom-limit N`, `--rom-limit=N` | refuse an image whose stored bytes exceed `N` |
| `--lma-offset N`, `--lma-offset=N` | load each segment at its address minus `N` |
| `-e SYM`, `--entry SYM`, `--entry=SYM` | the entry symbol (default `_start`) |
| `-T FILE`, `-TFILE`, `--script=FILE` | the GNU ld linker script that lays the image out (ARM and RISC-V) |
| `-L DIR` | a directory for `-l` and for the script's `INPUT`/`GROUP`/`INCLUDE` |
| `-u SYM`, `--undefined=SYM` | treat `SYM` as referenced, so that an archive supplies it |
| `--orphan-handling=place\|warn\|error` | what to do with a section the script places nowhere |
| `--gc-sections`, `--no-gc-sections` | drop the sections nothing kept refers to (see [`-ffunction-sections`](#-ffunction-sections--fdata-sections)) |
| `--print-gc-sections` | name each section dropped, on standard error |
| `-Map FILE`, `-Map=FILE`, `--Map=FILE` | write a map of where every input went, in GNU ld's format |
| `--print-memory-usage` | print how full each `MEMORY` region of the script is, as ld does |
| `-Tstack ADDR`, `-Tstack=ADDR` | passed on, and refused by `embld` for x86-64: `embld: -Tstack is a RISC-V option: ...` |

These are accepted and change nothing, because nothing in an image
`embld` makes depends on them: `--as-needed`, `--no-as-needed`, `-O0`, `-O1`, `-O2`, `--build-id`,
`--build-id=STYLE`, `--no-undefined`, `-s`, `--strip-all`, `-S`,
`--strip-debug`, and `-z` followed by `noexecstack`, `relro`, `norelro`,
`now` or `lazy` as a separate word (`-Wl,-z,now`). The image keeps its
symbol table under `-s` and `--strip-all`.

Any other linker option is refused, and nothing is linked:

```text
embcc: error: linker option '--section-start=.text=0' is not one EmbLD has (it takes -T, -L, -u, -e, -Ttext, -Tdata, -Tstack, --rom-limit, --lma-offset, --orphan-handling, --gc-sections, --print-gc-sections, -Map and --print-memory-usage); dropping it could build a different image from the one asked for
```

This covers `--section-start`, `-zKEYWORD` written as one word, and `-z`
with any other keyword.

### Linker options

Besides [`-Wl,` and `-Xlinker`](#-wlargs--xlinker-arg), the driver takes
gcc's own link options:

| Option | What happens |
|---|---|
| `-lNAME`, `-l NAME` | link `libNAME.a` from the `-L` directories, at its place on the line |
| `-LDIR`, `-L DIR` | search `DIR` for `-l` libraries |
| `-T FILE` | link with the GNU ld linker script `FILE` (ARM and RISC-V) |
| `-e SYM`, `-u SYM` | the entry symbol; a symbol to treat as referenced |
| `-nostdlib` | no `crt1.o`, `libc.a` or `librt.a` |
| `-nodefaultlibs` | no `libc.a` or `librt.a` |
| `-nostartfiles` | no `crt1.o` |
| `-static` | accepted: every image EmbLD writes is static |
| `-specs=FILE`, `--specs=FILE` | accepted: `nano.specs` and `nosys.specs` choose GCC's newlib and its system-call stubs, and EmbCC links its own C library and runtime; a link notes once `embcc: note: -specs=nano.specs is ignored: EmbCC links its own C library and runtime, not newlib`, and a compile says nothing |
| `-pthread`, `-rdynamic`, `-no-pie` | unknown argument |
| `-shared`, `-static-pie` | refused: `embcc: error: -shared needs position-independent code, which EmbCC does not emit` |

## Code generation options

### `-funwind-tables`, `-fasynchronous-unwind-tables`

Emit unwind tables (`.eh_frame`) so that a C++ exception can unwind
through the unit's functions. `-fexceptions` has the same effect for C.

The default depends on the language and target. Unwind tables are always
emitted for C++ units (unless both `-fno-exceptions` and
`-fno-unwind-tables` are given). For C they are emitted by default on the
hosted ELF targets (`-linux-gnu`), where an exception may unwind through
C library frames, and not on the freestanding targets.

### `-fno-unwind-tables`, `-fno-asynchronous-unwind-tables`

Do not emit unwind tables for a C unit. For a C++ unit with exceptions
enabled, tables are emitted regardless.

### `-fomit-frame-pointer`, `-fno-omit-frame-pointer`

Accepted and without effect. The x86-64 and AArch64 backends always keep
a frame pointer; the ARM, RISC-V and AVR backends never use one.

### `-fno-plt`

Accepted. EmbCC does not generate code that calls through a procedure
linkage table.

### `-fno-pic`, `-fno-pie`

Accepted, with `-fno-PIC` and `-fno-PIE`. EmbCC's code is not position
independent (`-fPIC` is [refused](#refused-options)): an x86-64 jump
table holds absolute addresses, a Cortex-M address is a `movw`/`movt`
pair, and `embld` links no position-independent executable. The Darwin
targets' code is PC-relative because Mach-O requires it, and links the
same way either way.

### `-fno-jump-tables`, `-fjump-tables`

`-fno-jump-tables` is honoured on every target: no `switch` is lowered
through a table of addresses, which code that runs before it is
relocated, or from an address other than the one it was linked at,
cannot use. A switch dense enough for a table (`n` cases, at least
four, whose values span at most `4n + 4`; at `-Os` at least six,
spanning at most `2n`) becomes the balanced compare tree that sparse
switches always get. The tables this removes are x86-64's indirect `jmp`,
AArch64's `br`, ARM's `tbb`/`tbh` and RISC-V's `jr` through a table;
the AVR target never uses one. `-fjump-tables`, the default, turns them
back on.

### `-fno-zero-initialized-in-bss`

Accepted. EmbCC already places every object with an initializer in
`.data`, zeros included: `int x = 0;` and `static char buf[64] = {0};`
are in `.data`, so a bootloader that does not clear `.bss` still finds
them zero. Only an object with no initializer at all (`int y;`) is in
`.bss`, as with GCC's option. `-fzero-initialized-in-bss`, which permits
the other placement, is accepted and changes nothing.

### `-fstrict-volatile-bitfields`, `-fno-strict-volatile-bitfields`

Accepted. EmbCC already reads and writes a volatile bit-field with one
access of its declared type's width, as the AAPCS requires and
`-fstrict-volatile-bitfields` asks: a `volatile uint32_t f : 8` is an
`ldr` (and an `ldr`/`str` pair to write it), never an `ldrb`, even when
the field lies in one byte. The access is to the field's container at
the type's alignment, so it can include neighbouring members. The
negative form permits narrower accesses, and EmbCC does not use the
permission.

### `-fverbose-asm`

Accepted and without effect on what `-S` writes.

### `-fgnuc-version=MAJOR[.MINOR[.PATCH]]`

Define `__GNUC__`, `__GNUC_MINOR__`, `__GNUC_PATCHLEVEL__` and
`__GNUC_STDC_INLINE__` in a C unit, as that version of GCC, the way clang
does by default. EmbCC does not by default, because a header that sees
`__GNUC__` may take paths that need GCC itself. Vendor headers that pick
their compiler support by it need it: CMSIS's `cmsis_compiler.h` stops at
`#error Unknown compiler` without it, and compiles with
`-fgnuc-version=4.2.1` (see [Embedded programming](embedded.md#cmsis-and-vendor-files)).
`0` leaves the macros undefined. A C++ unit always presents itself as
g++ (see [C++](cxx.md#compiler-identity)).

### `-ffunction-sections`, `-fdata-sections`

Put each function in a section of its own, `.text.NAME`, and each object
in `.data.NAME`, `.rodata.NAME` or `.bss.NAME`, as GCC names them, so that
the linker's `--gc-sections` can drop whatever the program never reaches:

```sh
embcc --target=thumbv7em-none-eabi -O2 -ffunction-sections -fdata-sections -c main.c
embcc --target=thumbv7em-none-eabi -T board.ld -Wl,--gc-sections startup.o main.o -o fw.elf
```

A function or object that names its own section keeps it, and so does a
thread-local object (`.tdata`/`.tbss`). String literals stay in the
unit's `.rodata`. A call from one function to another in the same unit
becomes a relocation, as it is between units; on RISC-V that is the
eight-byte `call` instead of a four-byte `jal`, because `embld` does not
relax. `-g` works with either option: the unit's code is described by
`DW_AT_ranges`. ELF output only; for Mach-O and COFF the options change
nothing. `-fno-function-sections` and `-fno-data-sections` turn them off.
What the linker keeps is in [`embld`](tools/embld.md#garbage-collection).

## Machine-dependent options

### x86-64 options

#### `-mno-sse`, `-mno-sse2`, `-mgeneral-regs-only`

Generate no SSE instructions, for code that runs before the kernel has
enabled SSE. Floating-point arithmetic needs SSE on x86-64, so a unit
that uses it is refused: `floating point needs SSE, which -mno-sse
forbids`. On AArch64, `-mgeneral-regs-only` (and `-mno-sse`) similarly
keeps code out of the floating-point registers, and floating point is
refused with `floating point used under -mgeneral-regs-only (in 'f')`.
On the other targets these options are accepted and have no effect.

#### `-mno-mmx`, `-mno-80387`, `-mno-red-zone`

Accepted on every target and without effect: EmbCC never uses MMX or
the x87 register stack for ordinary code, and never uses the red zone.

#### `-mcmodel=MODEL`

Accepted on every target, with any `MODEL`, and without effect. On
x86-64 the code model EmbCC uses already suits a kernel linked in the
upper 2 GiB of the address space.

### ARM options

These options apply to the Thumb targets. On any other target each one is
refused: `-mcpu=cortex-m4 is an ARM option, and the target is x86_64-elf`
(`-mabi=` is the RISC-V option of that name on RISC-V, below).

#### `-mthumb`

Accepted. Thumb is the only instruction set a Cortex-M has.

#### `-marm`

Refused: `-marm is not supported: a Cortex-M has no ARM instruction set,
only Thumb`.

#### `-mcpu=CPU`

Select the processor. `cortex-m4`, `cortex-m7` and `cortex-m33` select
ARMv7E-M code (with the DSP extension); `cortex-m3` selects ARMv7-M code.
`-mcpu=` does not move between ARMv7-M and ARMv8-M; that level comes from
the triple (`thumbv8m.main-none-eabi`).

EmbCC has no ARMv6-M or ARMv8-M Baseline code generator, so `cortex-m0`,
`cortex-m0plus`, `cortex-m1` and `cortex-m23` are refused: `-mcpu=cortex-m0
is ARMv6-M, and EmbCC emits ARMv7-M Thumb-2: that core does not implement
its ldr.w or IT blocks`.

Any other `CPU` is refused: `-mcpu=cortex-m55 is not a part EmbCC knows:
it emits ARMv7-M and ARMv7E-M (cortex-m3, m4, m7, m33)`.

#### `-mfpu=FPU`

Name the floating-point unit. EmbCC generates code for three units: the
single-precision `fpv4-sp-d16` (Cortex-M4F, ARMv7E-M) and `fpv5-sp-d16`
(Cortex-M33, ARMv8-M Mainline), and the double-precision `fpv5-d16`
(Cortex-M7, ARMv7E-M). `none`, `soft` and `auto` mean no FPU. The unit
must match the architecture, on ARMv7 the architecture must be ARMv7E-M,
and `fpv5-d16` is refused with an `-mcpu=` other than `cortex-m7`:

```text
-mfpu=fpv4-sp-d16 is an ARMv7E-M unit, and the part is ARMv7-M (a Cortex-M3 has no FPU); add -mcpu=cortex-m4
-mfpu=fpv5-d16 is the Cortex-M7's double-precision unit, and -mcpu=cortex-m4 does not have it; the Cortex-M4F's is -mfpu=fpv4-sp-d16
-mfpu=fpv5-d16 is not supported on thumbv8m.main-none-eabi: EmbCC emits VFP for the Cortex-M33's unit (-mfpu=fpv5-sp-d16) and nothing else: ...
```

With a single-precision unit, `float` arithmetic is VFP instructions and
`double` arithmetic is runtime calls. With `fpv5-d16` both are VFP
instructions: `double` arithmetic, comparisons, `fabs`, negation,
`__builtin_sqrt` and the conversions to and from `float` and 32-bit
integers are `.f64` instructions, and only the conversions between a
floating type and a 64-bit integer are still calls. `__ARM_FP` is
`0x4` for the single-precision units and `0xc` for `fpv5-d16`.

`-mfpu=` alone generates no FPU instructions; `-mfloat-abi=` decides
that, as in GCC.

#### `-mfloat-abi=ABI`

| `ABI` | Effect |
|---|---|
| `soft` | No FPU instructions; floating point is done by runtime calls. The default, except for the `-eabihf` triples. |
| `softfp` | FPU instructions; floating-point arguments and results passed in core registers. Links with `soft` objects. |
| `hard` | FPU instructions; floating-point arguments and results passed in `s0`-`s15`/`d0`-`d7` (AAPCS-VFP). Does not link with `soft` objects. |

`softfp` and `hard` need an FPU (`-mfloat-abi=hard needs an FPU to use:
add -mfpu=fpv4-sp-d16 (Cortex-M4F), -mfpu=fpv5-d16 (Cortex-M7) or
-mfpu=fpv5-sp-d16 (Cortex-M33)`). Any other value is refused:
`-mfloat-abi=ABI is not an ARM float ABI: it is one of soft, softfp and
hard`.

`-mfpu=` and `-mfloat-abi=` are resolved together after the whole command
line is read, so their order does not matter. An `-eabihf` triple implies
the part's FPU and `-mfloat-abi=hard` -- `fpv4-sp-d16` for
`thumbv7em-none-eabihf`, or `fpv5-d16` with `-mcpu=cortex-m7` -- and an
explicit option overrides it.
The choice is recorded in the object's `.ARM.attributes`, in the
predefined macros (`__ARM_FP`, `__ARM_PCS_VFP`, `__SOFTFP__`), and in the
triple `-dumpmachine` prints.

#### `-mabi=ABI`

`aapcs` and `aapcs-linux` are accepted: EmbCC emits the AAPCS (the
AAPCS-VFP variant under `-mfloat-abi=hard`), and `aapcs-linux` differs
from it only in making every enumeration `int`-sized, which EmbCC's are.
The other values (`apcs-gnu`, `atpcs`, `iwmmxt`) are refused:
`-mabi=apcs-gnu is not supported: EmbCC emits the AAPCS ...`.

#### `-mthumb-interwork`, `-mno-thumb-interwork`

Accepted. Interworking is between ARM and Thumb code, and a Cortex-M
runs only Thumb: every call and return is already one a `bx` or `blx`
would make.

#### `-munaligned-access`, `-mno-unaligned-access`

`-munaligned-access` is accepted: it is the ARMv7-M and ARMv8-M default,
and what EmbCC's code relies on. `-mno-unaligned-access` is refused,
because EmbCC cannot keep it: a packed structure's `int` member is one
`ldr` at its odd address, and a structure aligned below 4 (packed, or
`struct { char c[5]; }`) is copied and passed by value a word at a time
from wherever it lies.

```text
-mno-unaligned-access is not supported: EmbCC's ARMv7-M code uses word and halfword loads and stores at unaligned addresses (packed struct members; copies and by-value passing of structs aligned below 4), which the architecture allows and this flag forbids
```

#### `-mslow-flash-data`

Accepted. It asks the compiler to keep constants out of literal pools
in flash, and EmbCC's compiled Thumb code has none: it builds constants
and addresses with `movw`/`movt`. (An assembly file's `ldr rd, =VALUE`
still gets a pool entry where it needs one.)

### RISC-V options

#### `-mabi=ABI`

`ilp32` for RV32 and `lp64` for RV64 are accepted: EmbCC passes floating
point in the integer registers (`__riscv_float_abi_soft`). Any other
value -- the F and D conventions `ilp32f`, `ilp32d`, `lp64f`, `lp64d`,
the embedded `ilp32e`, or the other width's -- is refused:
`embcc: error: -mabi=ilp32d is not supported for riscv32-unknown-elf:
EmbCC emits the soft-float -mabi=ilp32 convention`.

### Options for every target

#### `-mlittle-endian`

Accepted: every target EmbCC emits for is little-endian.
`-mbig-endian` is [refused](#refused-options).

### Machine options that are not accepted

`-march=`, `-mtune=`, `-mabi=` (on x86-64, AArch64 and AVR), `-mmcu=`,
`-msoft-float`, `-mhard-float`, `-mcmse`, `-masm=`, `-m32` and `-m64`
are unknown arguments. The architecture, ABI and part are selected by the
[target triple](#target-selection) (and on ARM by `-mcpu=`, `-mfpu=` and
`-mfloat-abi=`). The RISC-V targets generate the C (compressed) extension
and the integer multiply/divide instructions; the AVR target generates
code for the avr5 architecture of the ATmega328P. See
[Targets](targets.md).

## Target selection

### `--target=TRIPLE`

Generate code for `TRIPLE`. One `embcc` binary contains every backend, so
any accepted triple can be named on any compiler. The triple selects the
backend, the data model and ABI, the predefined macros, the object
format, and the library directory used for linking.

A triple EmbCC does not know is refused, with the full list:

```text
embcc: error: unknown target 'bogus'
embcc: the targets it emits for are:
embcc:   x86_64-elf
...
```

The accepted triples, with the canonical spelling first. The canonical
name is what `-dumpmachine`, `--version` and diagnostics print.

| Canonical | Also accepted | Architecture | Environment | Object |
|---|---|---|---|---|
| `x86_64-elf` | `x86_64`, `x86_64-none-elf` | x86-64 | bare metal | ELF64 |
| `aarch64-elf` | `aarch64`, `arm64`, `aarch64-none-elf` | AArch64 | bare metal | ELF64 |
| `thumbv7m-none-eabi` | `thumbv7m`, `armv7m-none-eabi`, `arm-none-eabi` | ARMv7-M (Cortex-M3) | bare metal | ELF32 |
| `thumbv7em-none-eabi` | `thumbv7em`, `armv7em-none-eabi` | ARMv7E-M (Cortex-M4/M7), soft float | bare metal | ELF32 |
| `thumbv7em-none-eabihf` | | ARMv7E-M with FPv4-SP-D16 (FPv5-D16 with `-mcpu=cortex-m7`), hard float | bare metal | ELF32 |
| `thumbv8m.main-none-eabi` | `thumbv8m.main`, `thumbv8m-none-eabi`, `armv8m.main-none-eabi` | ARMv8-M Mainline (Cortex-M33), soft float | bare metal | ELF32 |
| `thumbv8m.main-none-eabihf` | | ARMv8-M Mainline with FPv5-SP-D16, hard float | bare metal | ELF32 |
| `riscv32-unknown-elf` | `riscv32`, `riscv32-elf`, `rv32` | RV32 | bare metal | ELF32 |
| `riscv64-unknown-elf` | `riscv64`, `riscv64-elf`, `rv64` | RV64 | bare metal | ELF64 |
| `avr` | `avr-none-elf`, `avr-elf`, `avr-unknown-none` | AVR (ATmega328P) | bare metal | ELF32 |
| `x86_64-emblink` | | x86-64 | EmbLinkOS | ELF64 |
| `aarch64-emblink` | | AArch64 | EmbLinkOS | ELF64 |
| `x86_64-linux-gnu` | `x86_64-linux` | x86-64 | Linux, static | ELF64 |
| `aarch64-linux-gnu` | `aarch64-linux` | AArch64 | Linux, static | ELF64 |
| `x86_64-apple-darwin` | `x86_64-darwin` | x86-64 | macOS | Mach-O |
| `aarch64-apple-darwin` | `arm64-apple-darwin`, `aarch64-darwin` | AArch64 | macOS | Mach-O |
| `x86_64-windows-gnu` | `x86_64-w64-mingw32` | x86-64 | Windows | COFF |

The ARM sub-architecture also follows from `-mcpu=` and the float ABI, so
`--target=thumbv7m-none-eabi -mcpu=cortex-m4` reports itself as
`thumbv7em-none-eabi`. Every compile for `x86_64-windows-gnu` prints the
`-Wwindows-abi` warning, because the Windows calling convention is not yet
complete. What each target supports is in [Targets](targets.md).

### The default target

With no `--target=`, the target is chosen in this order:

1. the environment variable `EMBCC_DEFAULT_TARGET`, if set and not empty;
2. the `DEFAULT_TARGET` the compiler was built with
   (`make DEFAULT_TARGET=riscv32-unknown-elf`; see
   [Getting started](getting-started.md#choosing-the-default-target));
3. `x86_64-elf`.

`--target=` on the command line overrides both. A default that is not an
accepted triple stops every invocation, naming where it came from:

```text
embcc: error: the default target 'bogus' is not one EmbCC knows
embcc: it came from EMBCC_DEFAULT_TARGET in the environment
embcc: the targets it emits for are:
...
```

`embcc --help` and `embcc --version` print the default this compiler
uses, and `embcc -dumpmachine` prints the target in effect.

## Developer and inspection options

### `embcc inspect STAGE FILE [OPTION...]`

Run the pipeline up to `STAGE` and print what it built, instead of
producing an object. `inspect` must be the first argument and `STAGE` the
second; the rest is an ordinary command line, so `-I`, `-D`, `--target=`
and `-O` apply as they would to a compile. The output goes to standard
output.

| `STAGE` | Prints |
|---|---|
| `tokens` | the tokens of the preprocessed source, with positions |
| `pp` | the preprocessed source (the same as `-E`) |
| `ast` | the syntax tree, with resolved types |
| `symbols` | every declaration in the unit, with its type and location |
| `types` | structure layout: member offsets, bit-fields and padding |
| `ir` | EmbIR as it stands at the selected `-O` level |
| `cfg` | the control-flow graph of each function: blocks, edges, dominators, loops |
| `callgraph` | which function calls which, from the IR |

`embcc inspect ir FILE.ir` reads EmbIR text instead of C and prints it
back, which is the IR's round-trip test. With `-O1`, `-O2` or `-Os`, the
optimizer runs on the parsed IR before it is printed, so a pass can be
tried on IR written by hand. `embcc inspect mir` explains that
EmbCC has no machine-IR level. An unknown stage is refused with
`embcc: inspect: unknown stage 'STAGE'`. With fewer than two arguments
after `inspect`, the stage list is printed and the status is 1.

Comparing `inspect ir` at `-O0` and at `-O2` shows what the optimizer did.

### `embcc why DECISION [SUBJECT] FILE [OPTION...]`

Compile `FILE` with remarks enabled and print the recorded decisions of
kind `DECISION`, optionally only those about `SUBJECT` (a function or
variable name). `why` must be the first argument. `SUBJECT` is recognized
when it does not start with `-` and contains no `.`; otherwise the word
is taken as the file.

```sh
$ embcc why inlined w.c -O2
sq (w.c:3): inlined
  because small-enough — 4 instructions into f, budget 24
  decided by the inline pass
```

`DECISION` is any decision name that appears in [`-fremarks`](#-fremarks--fremarksjson)
output, such as `inlined`, `not-inlined`, `promoted-to-register`,
`hoisted`, `unrolled` or `spilled-to-stack`. When nothing matches, `why`
says so and reminds you that optimization decisions need `-O2`; the
status is still 0.

### Machine-readable output

The driver's machine-readable forms are
[`-fdiagnostics-format=json`](#-fdiagnostics-formatformat),
[`-fdiagnostics-parseable-fixits`](#-fdiagnostics-parseable-fixits),
[`-fremarks=json`](#-fremarks--fremarksjson),
[`--emit-interfaces`](#--emit-interfaces) and
[`-fstack-usage`](#-fstack-usage). The language server
[`embls`](tools/embls.md) obtains its diagnostics by running `embcc`.

## Refused options

These options are refused because each is a promise about the generated
code that EmbCC would not keep. The driver stops with status 1 and a
message that names the option.

| Option | Message |
|---|---|
| `-fPIC`, `-fpic`, `-fPIE`, `-fpie` | `embcc: error: -fPIC is not supported; EmbCC would emit ordinary code and the flag's promise would not hold` |
| `-flto` | (same form) |
| `-fshort-enums` | (same form) |
| `-fprofile-*`, `--coverage`, `-fcoverage-mapping`, `-pg` | (same form) |
| `-fstack-clash-protection`, `-fcf-protection`, `-fcf-protection=...` | (same form) |
| `-fsanitize*` other than the forms in [Instrumentation](#instrumentation-options) | (same form) |
| `-fsanitize=NAME` for a check EmbCC does not have | `-fsanitize=NAME is not supported: EmbCC's sanitizer inserts checks that TRAP ...` |
| `-fstack-protector`, `-fstack-protector-all`, `-fstack-protector-strong`, `-fstack-protector-explicit` | `embcc: '-fstack-protector...' is not supported (EmbCC emits no stack protection); -fno-stack-protector is` |
| `-shared`, `-static-pie` | `embcc: error: -shared needs position-independent code, which EmbCC does not emit` |
| `-gdwarf-5` (any version but 2 to 4), `-gsplit-dwarf`, `-gz` | `embcc: error: -gdwarf-5 is not supported; EmbCC emits DWARF 4, uncompressed and in one piece` |
| `-marm` | `-marm is not supported: a Cortex-M has no ARM instruction set, only Thumb` |
| `-mno-unaligned-access` (ARM) | `-mno-unaligned-access is not supported: EmbCC's ARMv7-M code uses word and halfword loads and stores at unaligned addresses ...` |
| `-mabi=apcs-gnu`, `-mabi=atpcs`, `-mabi=iwmmxt` (ARM) | `-mabi=apcs-gnu is not supported: EmbCC emits the AAPCS ...` |
| `-mabi=ilp32f`, `ilp32d`, `ilp32e`, `lp64f`, `lp64d` (RISC-V) | `embcc: error: -mabi=ilp32d is not supported for riscv32-unknown-elf: EmbCC emits the soft-float -mabi=ilp32 convention` |
| `-mbig-endian` | `embcc: error: -mbig-endian is not supported: every target EmbCC emits for is little-endian` |
| `-fdump-rtl-*`, `-fdump-tree-*` and every other `-fdump-`, `-fcallgraph-info[=...]` | `embcc: error: -fdump-rtl-expand is not supported: it dumps GCC's internal representation, which EmbCC does not have; ...` |
| `-fcommon` for a Mach-O or COFF target | `embcc: error: -fcommon is not supported for x86_64-apple-darwin: ...` |
| `-fsingle-precision-constant` for C++ | `embcc: error: -fsingle-precision-constant is supported for C, not C++: ...` |
| `-O4` and other `-O` forms not listed | `embcc: unknown optimization flag '-O4'` |
| `-Wl,OPTION` or `-Xlinker OPTION`, for a linker option not listed under [`-Wl,`](#-wlargs--xlinker-arg) | `embcc: error: linker option 'OPTION' is not one EmbLD has (it takes -e, -Ttext, -Tdata, -Tstack, --rom-limit and --lma-offset); dropping it could build a different image from the one asked for` |
| `-Wa,OPTION`, for an option not listed under [`-Wa,`](#-waargs) | `embcc: error: assembler option 'OPTION' is not one the integrated assembler has` |

## Environment variables

### Variables the driver reads

`EMBCC_DEFAULT_TARGET`
: The target used when the command line has no `--target=`. Overrides
  the compiled-in default. See [The default target](#the-default-target).

`EMBCC_PREFIX`
: An installation prefix to take headers and libraries from, ahead of
  the build tree or installation found beside the executable. See
  [How the compiler finds its own files](#how-the-compiler-finds-its-own-files).

The driver reads no other variables: `CPATH`, `C_INCLUDE_PATH`,
`LIBRARY_PATH`, `GCC_EXEC_PREFIX`, `TMPDIR` and `NO_COLOR` have no effect.

### Variables for developing EmbCC

The compiler also reads the following variables. They exist for testing
and bisecting EmbCC itself; they change generated code or print internal
traces, and they are not a stable interface. See
[Testing](../internals/testing.md) and
[Register allocation](../internals/register-allocation.md).

| Variable | Effect |
|---|---|
| `EMBCC_VERIFY` | Run the IR verifier after IR generation and after optimization, and make an optimizer that fails to converge a fatal error. The test suite sets it. |
| `EMBCC_RA_MAXPOOL=N` | Limit the integer register pool to its first `N` registers, to exercise spill paths. |
| `EMBCC_RA_WHY` | Print a line to standard error for each function in which values were spilled. |
| `EMBCC_RA_TRACE` | Print the register allocator's pool and each value's assignment to standard error. |
| `EMBCC_RA_DEGREE_SPILL` | Choose spill candidates by interference degree instead of by cost. |
| `EMBCC_NO_TAILCALL` | Disable tail calls in the AArch64, ARM, RISC-V and AVR backends. |
| `EMBCC_NO_JUMP_TABLES` | Compile as with `-fno-jump-tables`, whatever the command line says, so a test suite's own command lines can be run without tables. |
| `EMBCC_NO_DIVMOD_CONST`, `EMBCC_NO_MLAKEEP` | On Thumb, stop sharing one divide between a quotient and a remainder by the same constant, or stop keeping a multiply's constant in a register for `mla`/`mls`. |
| `EMBCC_NO_SPLITLOOPS` | Disable the optimizer's loop-splitting step. |
| `EMBCC_NO_LKCONST` | Stop deciding compares and branches from a constant the same block has just written. |
| `EMBCC_VECDEBUG` | Trace the vectorizer's decisions. |
| `EMBCC_NO_FULLUNROLL` | Unroll a loop with a constant trip count like any other, instead of copying it whole. |
| `EMBCC_NO_RMW` | Disable x86-64 read-modify-write instruction fusion. |
| `EMBCC_NO_SINKUPD` | Leave a loop counter's update where it was instead of moving it next to the copy that ends it. |
| `EMBCC_NO_MEMOFF` | Disable folding constant offsets into loads and stores (ARM, RISC-V, AVR). |
| `EMBCC_NO_IDXOFF` | On RISC-V, stop moving a constant out of an array index into the access's displacement. |
| `EMBCC_RV_RA_MAX`, `EMBCC_RV_PAIRS`, `EMBCC_RV_PAIRS_ONLY`, `EMBCC_RV_JAL_RANGE`, `EMBCC_RV_LONG_CALLS` | RISC-V allocator, register-pair and call-range knobs. |
| `EMBCC_T_RA_MAX`, `EMBCC_T_PAIRS`, `EMBCC_T_PAIRS_ONLY`, `EMBCC_T_NOLO`, `EMBCC_T_FPU` | ARM allocator, register-pair and FPU knobs. |
| `EMBCC_T_NOWIDEIMM`, `EMBCC_NO_SIGNTEST` | Build a 64-bit constant whole on Thumb instead of using it half by half; keep `x >> 63` as a shift before a branch. |
| `EMBCC_AVR_RA`, `EMBCC_AVR_RA_MODE`, `EMBCC_AVR_RA_ONLY`, `EMBCC_AVR_RA_LIMIT`, `EMBCC_AVR_RA_CAP`, `EMBCC_AVR_REMAT_MAX`, `EMBCC_AVR_NO_VOL`, `EMBCC_AVR_NO_OCT`, `EMBCC_AVR_NO_XHOME` | AVR register-allocation mode and bisection knobs. |

The build and test scripts read further variables (`EMBCC_TARGET`,
`EMBCC_QEMU_TIMEOUT`, `EMBLD`, `EMBCC_AR` and others); those are
described in [Getting started](getting-started.md) and
[Testing](../internals/testing.md).
