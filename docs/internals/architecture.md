# Architecture

This page is the map of EmbCC for people about to change it: the
constraints the whole source tree obeys, the path of one source file
from the command line to an object file and an executable, the
directory, entry point and output of each stage, how the target is
chosen, the build, and the tools around the compiler. Each stage has a
page of its own, linked from here.

## Constraints on the whole tree

### One process

`embcc` runs every stage in one process: the preprocessor, the front
end, the optimizer, the code generator, the object writer and, when it
produces an executable, the linker. It never spawns a program. There is
no `cc1`, `as` or `ld` behind the driver; the assembler and the linker
are libraries (`src/as`, `src/arch/x86_64/as.c`, `src/link`) that the
driver calls. The reason is the system EmbCC is built to run on:
EmbLinkOS has no `fork` or `exec`, so a driver that started other
programs could not be hosted there.

### The platform layer is the only view of the host

Every interaction with the host goes through `src/platform/platform.h`:
reading and writing a file (`plat_read_file`, `plat_write_file`),
asking whether a file exists, reading an environment variable
(`plat_getenv`), asking whether standard error is a terminal, finding
the running binary (`plat_self_path`), and reading source text through a
replaceable provider (`src_read`, `src_set_provider`) so that a language
server can supply unsaved editor buffers. Running a program is optional
(`plat_can_run`, `plat_run_start`, `plat_run_wait`): `process_spawn.c` on
macOS and Linux, `process_none.c` on EmbLinkOS, which has no fork/exec,
and nothing may depend on it -- only the driver's several sources in one
command use it.
What every host shares is `src/platform/platform_common.c`, in ISO C.
The console and the binary's own path are `platform_posix.c` (macOS,
Linux, EmbLinkOS) or `platform_iso.c` (any host with a C library, chosen
with `make PLATFORM=iso`); a new host is another file beside them. See
[Porting EmbCC to a new host](porting.md).

Nothing reads the host's architecture: an `embcc` running on macOS and
one running on EmbLinkOS must produce the same object from the same
input and target.

### Machine knowledge lives in `src/arch/`

Target-dependent behaviour is a question asked by name of
`src/arch/target.h` (`target_ptr_size()`, `target_char_unsigned()`,
`target_va_list_is_pointer()`, ...); the data model comes from
`g_model[]` in `target.c`, one row per architecture
([D-012](decisions.md#d-012)). New target-dependent behaviour is a new
function there, not a test of `target_get()` where it is needed. Such
tests do remain outside `src/arch/`, in `src/parse/parse.c`,
`src/sema/sema.c`, `src/ir/irgen.c`, `src/opt/opt.c`,
`src/debug/dwarf.c`, `src/as/gas.c`, the C++ front end, and the
driver's choice of backend and object writer.

### EmbCC compiles itself

The compiler's own source must be accepted by EmbCC. This is a gate,
not a goal: `tests/golden/x86_64/self-host.sh` reads `SRCS` from the
Makefile, compiles every file with `./embcc`, compiles each a second
time to check that the objects are byte-identical, and links the result
with `embld` into an EmbLinkOS executable with no unresolved symbols (it
is skipped when the EmbLinkOS runtime and newlib are not installed).
`tests/golden/host-agnostic.sh` builds EmbCC with two different host
compilers and requires the objects the two produce from a test corpus to
be byte-identical. Together they
support the self-hosting fixed point: a stage-1 EmbCC built by EmbCC
must build a byte-identical stage 2.

In practice this means:

- Plain C99. The Makefile builds with `-std=c99 -Wall -Wextra -Werror`.
- No construct or builtin EmbCC does not implement. EmbCC does not
  implement the math builtins (`__builtin_floor`, `__builtin_ldexp` and
  the rest of that family); a call to one in `src/` breaks the self-host
  test although the host compiler accepts it.
- No system headers for object formats: `src/elf/elf.h`,
  `src/macho/macho.h` and `src/coff/coff.h` define what the writers
  need. Operating-system headers (`<unistd.h>`, `<mach-o/dyld.h>`)
  appear only in `src/platform/platform_posix.c`; the rest of `src/`
  includes nothing beyond the ISO C library.
- Deterministic output: no order-of-evaluation dependence between two
  IR-emitting calls passed as arguments to one call, no timestamps, no
  current directory in the output.

The full rules are in [Contributing](contributing.md#code-style).

## The compilation pipeline

For `embcc -c FILE.c -o FILE.o`, `main` in `src/driver/main.c` parses
the options, selects the target, and calls `compile`, which installs the
error boundary and runs `compile_unit`. `compile_unit` runs the stages
in order:

```text
 directory        entry point            what it hands on
 ---------------  ---------------------  ----------------------------------
 src/platform     src_read               the file's bytes
     |
 src/cpp          cpp_process            preprocessed text, with line
     |                                   markers (# N "FILE")
 src/cxx          cxx_translate          C text            (C++ input only)
     |
 src/lex          lex_next               tokens, on demand to the parser
 src/parse        parse_unit             struct unit: the AST
     |
 src/sema         sema_check             the same tree, every expression
     |                                   typed, every conversion explicit
 src/ir           irgen                  struct ir_unit: EmbIR
     |
 src/opt          opt_run                the same ir_unit, optimized
     |                                   (-O1 and above)
 src/arch/<arch>  codegen_unit*          struct code (the .text bytes) and
   regalloc.c       ra_allocate            relocation sites (enum reloc_kind)
     |
 src/debug        dwarf_emit, eh_emit    debug and unwind sections, with
     |                                   their relocations
 src/elf          elfw_*                 a relocatable object:
 src/macho        machow_*                 ELF, Mach-O or COFF
 src/coff         coffw_*
     |
 src/link         embld_link             an executable (ET_EXEC ELF), when
                                         -c is absent; EMBX from embld
```

| Stage | Entry point | Directory | Described in |
|---|---|---|---|
| Driver | `main`, `compile`, `compile_unit` | `src/driver` | [Invoking EmbCC](../manual/invoking.md) |
| Source input | `src_read` | `src/platform` | [The front end](front-end.md#source-input) |
| Preprocessor | `cpp_process` | `src/cpp` | [The front end](front-end.md#the-preprocessor) |
| C++ front end | `cxx_translate` | `src/cxx` | [The front end](front-end.md#the-c-front-end) |
| Lexer | `lex_init`, `lex_next` | `src/lex` | [The front end](front-end.md#the-lexer) |
| Parser | `parse_unit` | `src/parse` | [The front end](front-end.md#the-parser) |
| Semantic analysis | `sema_check` | `src/sema` | [The front end](front-end.md#semantic-analysis) |
| IR generation | `irgen` | `src/ir`, `src/arch/<arch>/irgen.c` | [EmbIR](ir.md) |
| Optimizer | `opt_run` | `src/opt` | [The optimizer](optimizer.md) |
| Register allocation | `ra_allocate`, `ra_allocate_fp` | `src/arch/regalloc.c` | [Register allocation](register-allocation.md) |
| Code generation | `codegen_unit`, `codegen_unit_arm64`, `codegen_unit_thumb`, `codegen_unit_riscv`, `codegen_unit_mips`, `codegen_unit_avr` | `src/arch/<arch>` | [Backends](backends.md) |
| Debug and unwind tables | `dwarf_emit`, `eh_emit` | `src/debug` | [Object files](object-formats.md#debug-information) |
| Object writers | `elfw_write`, `machow_write`, `coffw_write` | `src/elf`, `src/macho`, `src/coff` | [Object files](object-formats.md) |
| Linker | `embld_link` | `src/link`, `src/embx` | [EmbLD](linker.md) |

Notes on the order:

- The C++ front end is a translator. It parses and analyses C++, writes
  C, and the C front end compiles that C exactly as it compiles a C file
  ([D-013](decisions.md#d-013)). Everything after `cxx_translate` is
  shared between the two languages.
- The lexer is not a separate pass. The parser holds a `struct lexer` and
  calls `lex_next` as it goes; the C++ front end lexes its input whole
  with the same lexer in C++ mode (`lex_init_mode`).
- Each front-end stage that recovers from errors is followed by a check
  of its error count (`cx_nerrors`, `parse_error_count()`,
  `sema_error_count()`); if it is nonzero the driver prints
  `compilation terminated: N errors` and no later stage runs.
- Register allocation is not a separate stage in the driver. Each
  backend calls the shared allocator for each function, and only when
  the driver passes it `regalloc` (at `-O2`, `-O3`, `-Os` and `-Oz`).
- Between the optimizer and code generation the driver removes, at
  `-O1` and above, the `static` functions that no root reaches. It then
  moves functions with a `section` attribute to the end, grouped by
  section.
- The backend is chosen by `target_get()` in `compile_unit`: one call
  per architecture, with RV32 and RV64 sharing `codegen_unit_riscv`
  ([D-016](decisions.md#d-016)). All six have the same signature and
  return the same structures, so nothing after this point knows which
  machine produced the bytes.
- The driver, not the backends, lays out `.data`, `.bss`, `.rodata` and
  the thread-local sections, turns relocation sites into the object
  format's relocations, and writes the file. For `-S` it hands the same
  bytes and sites to `asm_emit_unit` (`src/driver/asmout.c`) instead of
  an object writer. [Object files](object-formats.md#from-code-to-an-object)
  describes this step in full.

### What each stage hands to the next

| Data | Structure | Declared in | Built by | Read by |
|---|---|---|---|---|
| Source text | `char *` | `src/platform/platform.h` | `src_read` | `cpp_process` |
| Preprocessed text | `char *`, with `# N "FILE"` line markers | `src/cpp/cpp.h` | `cpp_process`, `cxx_translate` | the parser |
| Tokens | `struct token` inside `struct lexer` | `src/lex/lex.h` | `lex_next` | the parser, the C++ front end |
| AST | `struct unit`: lists of `struct func`, `struct global`, `struct econst` and `struct topasm` (file-scope `asm`), with `struct stmt` and `struct expr` trees | `src/parse/ast.h` | `parse_unit` | `sema_check`, `irgen`, the driver |
| C types | `struct type`, `struct member` | `src/sema/type.h` | the parser and `sema_check` (`ty_*` in `src/sema/type.c`) | everything up to and including `irgen` |
| EmbIR | `struct ir_unit` holding `struct ir_func` arrays of `struct ir_ins`, the symbol table (`struct ir_sym`) and the string pool (`struct ir_str`) | `src/ir/ir.h` | `irgen`, or `ir_parse` from text | `opt_run`, the backends, `dwarf_emit`, `eh_emit`, `asm_emit_unit` |
| Machine code | `struct code`: a growable byte buffer and the data ranges inside it | `src/arch/code.h` | each backend's encoder (`src/arch/<arch>/emit.c`) | the driver, the object writers |
| Relocation sites | `struct extcall`, `struct strsite`, `struct gsite`, `struct fsite`, each with a `patch_off` and an `enum reloc_kind` | `src/arch/backend.h`, `src/arch/target.h` | the backends | the driver |
| Debug and unwind sections | `struct dwarf_out`, `struct eh_out`, each with its relocation list | `src/debug/dwarf.h`, `src/debug/eh.h` | `dwarf_emit`, `eh_emit` | the driver |
| Object under construction | `struct elfw`, `struct machow`, `struct coffw` | `src/elf/write.h`, `src/macho/write.h`, `src/coff/write.h` | the driver | written to disk by `elfw_write`, `machow_write`, `coffw_write` |

Properties a stage can rely on:

- After `sema_check`, every expression node carries a type and every
  implicit conversion C performs is an explicit `EXPR_CAST` node. IR
  generation never infers a width or a signedness.
- EmbIR is linear three-address code over virtual registers, typed by
  width and flags rather than by C types. The ABI decisions that need C
  types (how an argument is passed, how a structure is returned) are
  made during IR generation and recorded on the instruction
  ([EmbIR](ir.md)).
- EmbIR is not yet independent of the AST. `ir_func.src` points at the
  `struct func` the function came from, where the backend records its
  `code_off` and `code_len`, and a call carries its callee's
  `struct func *` alongside the index into `ir_unit.syms`. The AST
  therefore lives until the object is written.
- Relocations are machine-neutral until the driver writes them. A
  backend records a `reloc_kind` (for example `RK_ADR_HI21` and
  `RK_ADD_LO12` for an AArch64 address, `RK_PCREL32` for an x86-64
  RIP-relative one), and `target_reloc_type`, `target_macho_reloc` and
  `target_coff_reloc` in `src/arch/target.c` map the kind to the object
  format's numbering ([Object files](object-formats.md#relocation-kinds)).
- There is no machine IR between EmbIR and bytes. Each backend selects
  instructions and encodes them directly from EmbIR;
  `embcc inspect mir` explains this instead of printing a dump.

### Where the pipeline stops early

Several options run the pipeline part of the way and print what the
last stage built. They are the intended way to see the hand-off between
two stages.

| Command | Stops after | Writes |
|---|---|---|
| `embcc -E FILE`, `embcc inspect pp FILE` | the preprocessor | preprocessed text |
| `embcc -c -M FILE`, `-c -MM` | the preprocessor | a make rule |
| `embcc --emit-c FILE.cc` | the C++ front end | the C it lowered to |
| `embcc inspect tokens FILE` | the lexer | the token stream |
| `embcc inspect ast`, `symbols`, `types FILE` | semantic analysis | the tree, the declarations, the structure layouts |
| `embcc --emit-interfaces FILE` | semantic analysis | interface hashes (`src/driver/iface.c`), read by `embidx` |
| `embcc -fsyntax-only FILE` | semantic analysis | diagnostics only |
| `embcc inspect ir`, `cfg`, `callgraph FILE` | the optimizer, at the `-O` level given | EmbIR text, the control-flow graph, the call graph |
| `embcc why DECISION [SUBJECT] FILE` | the optimizer | the remarks a pass recorded (`src/driver/remark.c`) |
| `embcc -S FILE` | code generation | the emitted bytes as assembly text |

`embcc inspect ir FILE.ir` reads EmbIR's textual form with `ir_parse`
and prints it back, so a pass can be studied with no C source and no
backend; printing, parsing and printing again gives the same bytes.
Given an `-O` level, it runs `opt_run` on the parsed unit first. The
option reference is [Invoking EmbCC](../manual/invoking.md).

### Other inputs

The driver dispatches on the input's suffix before the C pipeline runs:

| Input | Handled by | Notes |
|---|---|---|
| `FILE.c`, or any file with `-x c` | the pipeline above | |
| `FILE.cc`, `.cpp`, `.cxx`, `.C`, `.c++`, `.cp`, `.CPP`, `.ii`, or `-x c++` | the pipeline, through `cxx_translate` | |
| `FILE.s`, `FILE.S` | `gas_assemble` in `src/as/gas.c` | GNU syntax for AArch64, ARMv7-M, RISC-V and AVR, encoded by each target's own inline-asm assembler; `.S` is preprocessed first |
| `FILE.asm` | `as_assemble` in `src/arch/x86_64/as.c` | NASM syntax, x86-64 only; the same code as `embas` |
| `FILE.ir` | `ir_parse` in `src/ir/irparse.c` | with `embcc inspect ir` only |

The compiler's state is per process, so one process compiles one
source. Given several (`embcc a.c b.c -o OUT`), the driver runs itself
once per source with `-c` and a temporary object (`multi_source`), up to
`-j N` at a time, then links the objects in command-line order among the
other inputs and removes them. That needs a host that can run a program
(`plat_can_run`); on one that cannot (EmbLinkOS), a second source is
refused with the way round it: compile each with `-c` and link the
objects. Objects, archives and `-l` libraries go to the link as they
are.

### Linking

Without `-c`, `compile_and_link` compiles the input to a temporary
object beside the output (`OUT.embcc-tmp.o`) and calls `embld_link` in
the same process with, in order: `crt1.o` (hosted targets), the object,
the command line's objects, archives and `-l` libraries, `libcxx.a` for a
C++ input, `libc.a`, and `librt.a` (`-nostdlib`, `-nodefaultlibs` and
`-nostartfiles` leave them out). The files are found through
`paths_target_file` (`src/driver/paths.c`), relative to the `embcc`
binary. A hosted target refuses to link without its `crt1.o` and
`libc.a`; a C++ input refuses to link without `libcxx.a`. The link
options are those `-Wl,` and `-Xlinker` gave (`apply_wl`): EmbLD's own
`-e`, `-Ttext`, `-Tdata`, `-Tstack`, `--rom-limit` and `--lma-offset`
are applied, options that change nothing about the image are accepted,
and any other is refused ([EmbLD](linker.md#the-drivers-link)).

The driver links x86-64 ELF programs and, for the firmware targets
(ARMv7-M, ARMv8-M, RV32, RV64, MIPS32, AVR), images whose memory map the build
gives: a linker script (`-T`, ARM and RISC-V) or `-Wl,-Ttext`/`-Tdata`.
A firmware link without one stops with `embcc: error: linking a TRIPLE
image needs its memory map`. Every other target (AArch64 ELF, Mach-O,
COFF) stops with `embcc: error: cannot link for TRIPLE`, because embld
does not read those objects.

The standalone `embld` links more than the driver uses: it accepts
x86-64, ARMv7-M and ARMv8-M, RV32, RV64 and AVR objects, and refuses
AArch64 ones. EMBX, EmbLinkOS's native executable format, is written by
`embld --embx` from the same linked image ([D-003](decisions.md#d-003));
`src/embx` holds the container definition that `embld` and `embread`
share. The compiler never writes EMBX directly. See [EmbLD](linker.md).

## Errors and the fatal boundary

A stage shares its process with its caller, which may be a server
that must keep running, so a stage that meets an error does not end the
process. `compile` installs a `setjmp` boundary with
`fatal_set_boundary` (`src/driver/util.h`) around `compile_unit`, and a
stage that cannot continue leaves through it:

- `diag_fatal` and `diag_at` record a diagnostic and unwind.
- `fatal_unwind` unwinds after a diagnostic is already recorded.
- `internal_error` reports an impossible state as a compiler bug and
  unwinds.

With no boundary installed, the same calls flush the diagnostics and
call `exit(1)`, so a tool that does not install one still behaves
correctly. Diagnostics are records rendered once, at the boundary or at
exit, as caret text or JSON (`src/driver/diag.c`).

`compile` also fails a unit that ran to the end but recorded an error,
which is how a warning promoted by `-Werror` or `-Werror=NAME` ends:
when `diag_error_count()` is nonzero it returns 1 and removes the output
file it was writing, so no object, assembly file or executable is left
for `make` to take as up to date. `compile_and_link` then stops before
linking.

The following still end the process directly:

- `xmalloc`, `xcalloc` and `xrealloc` on out-of-memory, where building a
  diagnostic would itself allocate.
- The refusal functions of the embedded backends (`t_refuse` in
  `src/arch/thumb/codegen.c`, `rv_refuse` in `src/arch/riscv/codegen.c`,
  `mips_refuse` in `src/arch/mips/codegen.c`, `a_refuse` in
  `src/arch/avr/codegen.c`), which print the
  `cannot lower ... yet` message and call `exit(1)`.
- Consistency checks in the Thumb encoder (`src/arch/thumb/emit.c`),
  which call `abort()` because the file is also linked into the encoding
  checkers, which have no driver.

How each front-end stage recovers is described in
[The front end](front-end.md#errors-and-recovery).

## Target selection

One process compiles for one target, fixed before the front end runs.
A target is a triple of architecture (`enum target_arch`), operating
system (`enum target_os`) and object format (`enum target_fmt`), all
declared in `src/arch/target.h` ([D-014](decisions.md#d-014)). The
driver sets it in this order:

1. The built-in default, `x86_64-elf`: the initial values of the
   globals in `src/arch/target.c`.
2. `target_apply_default`, before any option is read. It uses
   `EMBCC_DEFAULT_TARGET` from the environment if set, otherwise the
   triple compiled in by `make DEFAULT_TARGET=TRIPLE`
   ([D-017](decisions.md#d-017)). An unknown name stops the compiler and
   says which of the two it came from.
3. Every `--target=TRIPLE` on the command line, in a scan made before the
   other options are parsed, so that `--version`, `-dumpmachine` and
   `--dump-predef` describe the requested target.
   `target_from_triple` looks the name up in `g_triples[]`, the explicit
   table of accepted spellings; an unknown name is refused with
   `embcc: error: unknown target 'NAME'` and the list of known triples.
   Once the scan is done, the backend's "this operation calls a runtime
   helper" predicate is installed for the optimizer
   (`target_set_calls_helper`), from the target finally chosen, so a
   compiler with a configured default makes the same code as one given
   that target by `--target=`. Thumb, RISC-V and AArch64 have one;
   x86-64 and AVR have none.
4. After all options, `arm_float_resolve` settles the Thumb FPU and
   float ABI from `-mfpu=`, `-mfloat-abi=` and an `-eabihf` triple.

Everything downstream reads the result through `target.h`: the data
model (`g_model[]`, one row per architecture), the predefined-macro
table (`src/arch/predef.c` chooses among the generated tables in
`src/arch/<arch>/predef.c` and `predef_cxx.c`), the backend
`compile_unit` calls, the object writer (`target_fmt_get()`), and the
relocation numbering. Every backend is linked into every `embcc`; a
configured default changes only which one is used when no `--target=`
is given. The triples and what each implies are listed in
[Backends](backends.md#the-target-model) and
[Targets](../manual/targets.md).

## Source tree

| Directory | Contents |
|---|---|
| `src/driver/` | `main.c`: options, target selection, the pipeline, data layout and object emission. `diag.c` and `util.c`: diagnostics, allocation, the fatal boundary. `explain.c`: `--explain`. `inspect.c`: the `inspect` dumps. `remark.c`: optimization remarks for `why`. `asmout.c`: `-S`. `iface.c`: `--emit-interfaces`. `paths.c`: the installed headers and libraries, found relative to the binary. `version.h`: the version string. |
| `src/platform/` | The host interface (`platform.h`) and its POSIX implementation. |
| `src/cpp/` | The preprocessor. |
| `src/lex/` | The lexer, used by the C parser, the C++ front end and `inspect tokens`. |
| `src/parse/` | The C parser and the AST (`ast.h`). |
| `src/sema/` | Semantic analysis (`sema.c`); the type system and layout (`type.c`); exact compile-time `long double` arithmetic (`ldfloat.c`); 128-bit integer arithmetic (`w128.c`); `-Wuninitialized` (`uninit.c`); `-Wformat` (`format.c`). |
| `src/cxx/` | The C++ front end. `translate.h` is its one public header; `cxx.h` is internal. |
| `src/ir/` | EmbIR (`ir.h`), its generation (`irgen.c`, `irgen_int.h`), and its textual form (`irprint.c`, `irparse.c`). |
| `src/opt/` | The optimizer and the IR verifier, in `opt.c`. |
| `src/arch/` | Target selection and the data model (`target.c`), the backend contract (`backend.h`), the code buffer (`code.c`), the choice of macro table (`predef.c`), the shared register allocator (`regalloc.c`), and one directory per architecture. See [Backends](backends.md#layout-of-srcarch). |
| `src/debug/` | DWARF 4 debug information (`dwarf.c`) and `.eh_frame` and `.gcc_except_table` (`eh.c`). |
| `src/elf/`, `src/macho/`, `src/coff/` | The object-file definitions and writers for each format. |
| `src/as/` | The GNU-syntax assembler front end for `.s` and `.S` files. |
| `src/link/` | EmbLD, the linker, as a library (`link.h`). |
| `src/embx/` | The EMBX container definition and checksum, shared by `embld` and `embread`. |
| `include/` | The freestanding headers installed with the compiler (`stddef.h`, `stdint.h`, `stdarg.h`, ...). |
| `lib/` | The libraries EmbCC compiles for its targets: `libc` (one implementation with a backend per operating system under `lib/libc/os/`), `libcxx` (the C++ runtime) and `rt` (the compiler runtime). See [Libraries](../manual/libraries.md). |
| `tests/` | The test suite and the QEMU harnesses. See [Testing](testing.md). |
| `tools/` | The toolchain programs other than `embcc`, the encoding checkers, and scripts. See [Tools](#tools). |
| `build.ebm` | The EmbBuild manifest that builds EmbCC on EmbLinkOS. |

## The build

The Makefile builds EmbCC with any C99 compiler. The defaults are
`CC = cc`, `CFLAGS = -std=c99 -Wall -Wextra -Werror -g`, and objects in
`build/` (`BUILD=`). Every object depends on every header, so a header
edit rebuilds everything.

| Target | Builds |
|---|---|
| `embcc` | the compiler: every file in `SRCS`, plus `tools/embdbg/embdbg.c` compiled with `-DEMBDBG_NO_MAIN` (`build/embdbg_core.o`), whose `.embdbg` writer the linker library calls |
| `all` | `embcc`, `embread`, `embld`, `embas`, `embls`, `embidx` |
| `embdbg` | the debugger (not part of `all`) |
| `$(BUILD)/embcc` | the compiler linked inside the object directory, leaving `./embcc` alone |
| `check`, `test`, `test-arm64`, `test-libstdcxx` | the test suites; see [Testing](testing.md#make-targets) |
| `libc-*`, `libcxx-*`, `rt-embedded` | the libraries, compiled by `./embcc` |
| `install`, `install-files`, `uninstall` | installation under `PREFIX`, staged under `DESTDIR` |
| `clean` | removes `build/`, `embcc`, `embread`, `embld`, `embdbg`, `embas` and `embls` |

Name a target. Plain `make` builds only the first rule in the Makefile,
`build/embdbg_core.o`, and leaves `./embcc` as it was.

Each tool has its own link rule and its own list of sources (`EMBLS_SRCS`
for `embls`, explicit lists for `embas` and `embld`). These lists, `SRCS`
and `build.ebm` are kept by hand; adding a file to `src/` means adding it
to each list that needs it
([Contributing](contributing.md#lists-that-are-kept-by-hand)).

`make DEFAULT_TARGET=TRIPLE embcc` compiles in a default target
([D-017](decisions.md#d-017)). Building and installing are described for
users in [Getting started](../manual/getting-started.md) and for
contributors in [Contributing](contributing.md#building).

## Tools

The programs in `tools/` are host programs built around the libraries in
`src/`. Unlike `src/`, they may use the host directly: `embls`, for
example, runs `embcc` for diagnostics and parses in a forked child.

### Toolchain programs

| Program | Source | What it links from `src/` | Reference |
|---|---|---|---|
| `embcc` | `src/driver/main.c` | all of `SRCS` | [Invoking EmbCC](../manual/invoking.md) |
| `embld` | `tools/embld/embld.c`, `doctor.c` | `src/link`, `src/embx`, the RISC-V, MIPS and AVR encoders (for the RISC-V and MIPS entry stubs and AVR relocations), the x86-64 decoder, the `embdbg` core | [EmbLD](linker.md), [embld](../manual/tools/embld.md) |
| `embas` | `tools/embas/embas.c` | the NASM-syntax assembler (`src/arch/x86_64/as.c`) and the ELF writer | [embas](../manual/tools/embas.md) |
| `embread` | `tools/embread/embread.c` | `src/embx` | [embread](../manual/tools/embread.md) |
| `embdbg` | `tools/embdbg/embdbg.c`, `remote.c` | the x86-64 decoder (`src/arch/x86_64/disasm.c`) | [embdbg](../manual/tools/embdbg.md) |
| `embls` | `tools/embls/embls.c` | the preprocessor, parser, semantic analysis, IR generation, optimizer and backends (`EMBLS_SRCS`) | [embls](../manual/tools/embls.md) |
| `embidx` | `tools/embidx/embidx.c` | nothing; it runs `embcc --emit-interfaces` and indexes the text | [embidx](../manual/tools/embidx.md) |

### Encoding checkers

Each backend encodes its own instructions, so each encoder and each
inline-asm assembler has a checker that compares its output with an
external assembler or disassembler. The checkers are compiled by the
golden tests that use them, not by the Makefile.

| Checker | Checks | Test |
|---|---|---|
| `tools/a64check/a64check.c`, `a64asmcheck.c` | the AArch64 encoder and inline-asm assembler | `tests/golden/aarch64/arm64-encoding.sh`, `arm64-asm.sh` |
| `tools/thumbcheck`, `tools/tasmcheck`, `tools/vfpcheck` | the Thumb encoder, the Thumb inline-asm assembler, the VFP instructions | `thumb-encoding.sh`, `thumb-asm.sh`, `thumb-vfp.sh` |
| `tools/t1check` | the ARMv6-M (Thumb-1) encoders, `t1_*` in `src/arch/thumb/emit.c` | `thumb-v6m-encoding.sh` |
| `tools/riscvcheck`, `tools/rvasmcheck` | the RISC-V encoder and inline-asm assembler | `riscv-encoding.sh`, `riscv-compressed.sh`, `riscv-asm.sh` |
| `tools/mipscheck`, `tools/mipsasmcheck` | the MIPS32 encoder and inline-asm assembler | `mips-encoding.sh`, `mips-asm.sh` |
| `tools/avrcheck`, `tools/avrasmcheck` | the AVR encoder and inline-asm assembler | `avr-encoding.sh`, `avr-asm.sh` |
| `tools/pmovecheck` | `ra_parallel_move`, by executing every small case against a model register file | `parallel-move.sh` |

Tests without a directory are in `tests/golden/`. See
[Backends](backends.md#encoders-and-their-referees) and
[Testing](testing.md#referees).

### Scripts

| Script | Purpose |
|---|---|
| `gen-predef.sh` | Regenerates `src/arch/<arch>/predef.c` and `predef_cxx.c` from the target's reference compiler. The only way those files change. |
| `gen-embbuild-manifest.sh`, `gen-kernel-manifest.sh` | Generate `build.ebm`, and the EmbBuild manifest for the EmbLinkOS kernel. |
| `gen-selfhost-ref.sh` | Builds the reference objects for the self-hosting fixed point on EmbLinkOS and relinks stage 1. |
| `build-rt.sh` | Builds `librt.a` for one embedded target (used by `make rt-embedded`). |
| `build-ref-gxx.sh`, `build-libstdcxx.sh` | Build the reference g++ and its libstdc++, and libstdc++ compiled by EmbCC. |
| `bench-opt.sh` | Compares optimized code with gcc and clang on `tests/bench/kernels.c`. |
| `x86-identity.sh` | Requires x86-64 objects to be byte-identical to those of a baseline revision. |
| `gap-probe.sh` | Re-runs the feature audit against GCC and Clang. |
| `hostpaths.sh` | The one place the host locations of cross tools, newlib and an EmbLinkOS tree are set. |
| `embbuild-run.sh`, `os-stage.sh`, `os-build-embld.sh` | Run an EmbBuild manifest on the host, and stage programs and `embld` into an EmbLinkOS image. |

## Related pages

[The front end](front-end.md), [EmbIR](ir.md),
[The optimizer](optimizer.md),
[Register allocation](register-allocation.md), [Backends](backends.md),
[Object files](object-formats.md), [EmbLD](linker.md),
[Testing](testing.md), [Contributing](contributing.md),
[Design decisions](decisions.md), [Status](status.md).
