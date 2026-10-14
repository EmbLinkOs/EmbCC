# Contributing

This page is for anyone who changes EmbCC: how to build it, the rules the
code and its tests follow, how commits are written, how to add an option,
an attribute, a builtin, a warning, an IR operation or a target, and what a
change has to show before it is merged. The testing side is described in
full in [Testing](testing.md); the reasoning behind the project's standing
choices is in [Design decisions](decisions.md).

## Ground rules

### The refusal rule

> A capability may only be claimed if it is genuinely present. A refusal
> is only honest if the thing is truly absent.

For a compiler this means: when EmbCC cannot do something correctly, it
stops with an error that names the thing it cannot do. It never emits code
that does something else. A missing feature is expected; a wrong one is a
bug that surfaces months later as a program that crashes for no visible
reason.

The rule applies everywhere a promise is made:

- **Language constructs.** An unsupported construct is a located error
  naming it: `__attribute__((cleanup)) is not supported: the cleanup
  function would never run`.
- **Targets.** A backend that cannot lower an operation says which one and
  where: `the RV32 backend cannot lower a 128-bit value yet (function f)`.
  It never falls through to another target's code generator.
- **Options.** A flag that promises something about the code (`-fPIC`,
  `-flto`, `-fsanitize=address`) is refused by name if
  EmbCC would emit ordinary code instead. A flag is accepted silently only
  when EmbCC already behaves that way (`-fwrapv`, `-fno-strict-aliasing`),
  and the reason is written next to the code that accepts it.
- **Attributes.** An attribute whose loss changes what the program
  computes is refused; one with genuinely nothing to do is accepted; an
  unknown one is warned about under `-Wattributes`.
- **Documentation and `--help`.** Neither may name an option or a feature
  the compiler does not have.

Each refusal states the reason, so a reader who meets it knows whether the
construct is absent, partly present, or deliberately not planned.

### A change is done when a test exercises the invariant

For EmbCC the invariant is almost always "the machine runs what we emitted,
and the result is correct". A change is not done because it compiles, and a
test that only checks that the compiler exits 0, or that an object "looks
valid", does not establish it. Prefer a test that runs the program and
asserts its result against a reference; see [Testing](testing.md).

Four ways a green result lies, each of which has happened:

1. **The binary under test was stale.** Something rebuilt and something did
   not, and the test measured yesterday's artifact. Every shell test prints
   a `TEST-MARKER` line the runner looks for; check that what you think ran
   is what ran.
2. **The code never ran.** A test that silently skipped, a redirect that
   left the previous log in place, a pipeline that died early. If the
   marker is missing, doubt the harness before the code.
3. **A green build is not a green test.** After touching a subsystem, run
   that subsystem's tests, not the ones in muscle memory.
4. **Emulated time is not wall time.** Under QEMU a guest runs at a
   fraction of native speed; no output for a minute is usually slow, not
   hung. Take a register or state dump before concluding anything.

Check exit statuses, never only grepped output, and capture the status
before any command substitution in a script (`rc=$?` first).

### Prove on the host, confirm on the target

Iteration on the host takes seconds; booting an operating system takes
minutes. Compare EmbCC's output against a reference compiler and with
`readelf`, `objdump` and `nm`; link EmbCC's objects with another linker to
check symbol closure independently of EmbLD; then run the result on the
target, which is the final judge but not the first one
([D-005](decisions.md#d-005)).

### Scope

EmbCC is a separate project from EmbLinkOS, which is the parent and wins any
contest for attention ([D-001](decisions.md#d-001)). A change to EmbCC
that would require destabilizing the operating system to test is a sign
that the change is wrong or premature.

## Building

EmbCC builds with a C99 compiler, `make` and a POSIX shell:

```sh
make embcc            # the compiler
make all              # embcc, embread, embld, embas, embls, embidx
make embdbg           # the debugger (not part of `all`)
```

Name the target. Plain `make` with no target builds only
`build/embdbg_core.o`, the first rule in the Makefile.

The default flags are `CFLAGS = -std=c99 -Wall -Wextra -Werror -g`, so a
warning is a build failure. Objects go to `build/` (`BUILD=`); every object
depends on every header, so no object can be stale after a header edit.
`make CC=clang BUILD=/tmp/x /tmp/x/embcc` builds a second compiler in a
private directory without touching `./embcc`.

| Target | Builds |
|---|---|
| `libc-x86_64`, `libc-aarch64` | EmbCC's C library for the freestanding targets, compiled by EmbCC |
| `libc-linux-x86_64`, `libc-linux-aarch64` | the C library for the static Linux targets |
| `libc-emblinkos` | the C library over the EmbLinkOS backend |
| `libcxx-x86_64`, `libcxx-aarch64`, `libcxx-linux-x86_64`, `libcxx-linux-aarch64` | EmbCC's C++ runtime |
| `rt-embedded` | `librt.a` for each embedded target (`tools/build-rt.sh`) |
| `install` | everything, installed under `PREFIX` (default `/usr/local`), staged under `DESTDIR` |
| `clean` | removes the build products |

`make DEFAULT_TARGET=riscv32-unknown-elf embcc` builds a compiler whose
default target is that triple; every backend is still present
([D-017](decisions.md#d-017)).

### Lists that are kept by hand

Some lists cannot be derived, and a new source file has to join them:

- `SRCS` in the Makefile, for every compiler source. The self-host test
  reads this list from the Makefile.
- `EMBLS_SRCS`, when the new file defines anything that `sema.c`,
  `irgen.c` or `predef.c` reaches. `make check` does not build `embls`, so a
  missing entry breaks only `make test`, at the build step, before any test
  runs.
- the `embld` rule, when the linker needs the file (it links the encoders
  it uses to write start-up code).
- `build.ebm`, the EmbBuild manifest that builds EmbCC on EmbLinkOS. It
  records each source's header closure, so it goes stale when a source file
  is added **or when an `#include` line changes**. Regenerate it with
  `tools/gen-embbuild-manifest.sh > build.ebm`;
  `tests/golden/x86_64/embbuild.sh` fails on a stale one.

Generated files are changed only by their generators. The predefined-macro
tables (`src/arch/<arch>/predef.c`, `predef_cxx.c`) come from
`tools/gen-predef.sh`, which reads them off the reference compiler for each
target; they are never edited by hand.

## Code style

- **C99.** The compiler is built with `-std=c99 -Wall -Wextra -Werror`.
  Keep it buildable by a strict C99 compiler.
- **The self-hosting subset.** `src/` must also compile with EmbCC itself:
  `tests/golden/x86_64/self-host.sh` compiles every file in `SRCS` with
  `./embcc`. Use only what EmbCC implements. In particular, EmbCC does not
  implement the math builtins (`__builtin_floor`, `__builtin_ldexp` and the
  rest of that family), so a call to one in `src/` breaks the self-host
  test even though the host compiler accepts it. Before using any
  `__builtin_*` in `src/`, check that `sema.c` handles it.
- **Determinism.** EmbCC's output must be a function of its input only.
  It must not depend on the host compiler that built EmbCC
  (`tests/golden/host-agnostic.sh` builds EmbCC with two host compilers and
  compares): do not pass two calls that emit IR as arguments of one call,
  because their order of evaluation is unspecified. Two runs on the same
  input produce the same bytes, and `-O0` output is byte-identical to the
  output with no `-O` flag; the self-hosting fixed point depends on both
  (`tests/golden/x86_64/self-host.sh`, `optimizer.sh`).
- **No host knowledge above the platform layer.** Every interaction with
  the host (files, the environment, the console) goes through
  `src/platform/platform.h`. Nothing reads the host's architecture, nothing
  spawns a process (EmbLinkOS has no `fork`/`exec`), and there is no
  `#ifdef __APPLE__` or similar outside the platform layer.
- **Target knowledge in `src/arch/`.** The lexer, parser, sema, IR,
  optimizer and object writers never name a machine. They ask a question
  by name (`target_ptr_size()`, `target_char_unsigned()`,
  `target_va_list_is_pointer()`), and a new question becomes a new
  function, not a comparison against one architecture.
- **Layout.** Four-space indentation, no tabs. A function's opening brace
  on its own line; other braces on the line of their statement. Comments
  in `/* */` only. Keep lines within 80 columns where practical.
- **Names.** `snake_case`, with a prefix per module or target (`ra_`,
  `ir_`, `diag_`, `a64_`, `t_`, `rv_`, `avr_`, `x86_`). File-local
  functions and data are `static`.
- **Errors.** `diag_fatal(file, line, fmt, ...)` for a refusal or any error
  that ends the compile, and `diag_at` for the same with a column for the
  caret; `diag_error_at` followed by `diag_note_at` for an error that
  carries notes; `diag_warn_opt(file, line, col, "name", fmt, ...)` for a
  warning that has a `-W` name (all in `src/driver/util.h`). Allocate with
  `xmalloc`, `xcalloc` and `xrealloc`, which never return NULL.
- **Comments say why.** A comment explains what the code decides and the
  reason, including what would go wrong if it were done the obvious other
  way. Where a recorded decision governs the code, cite it (`D-014`).

## Commit messages

```text
area: what is now true, in one line

What was wrong, shown by the smallest case that exposes it. Why it was
wrong. What changed, and what the change deliberately does not handle.

The evidence: the test that failed before this change and passes after
it (and what it printed before), which suites were run, and any
measurement, with how it was taken.
```

- The subject is `area: summary`. The area is the part of the tree or the
  target: `opt`, `regalloc`, `sema`, `parse`, `cpp`, `lex`, `ir`,
  `irgen`, `driver`, `link`, `as`, `rt`, `tests`, `build`, `docs`, or a
  target (`x86`, `aarch64`, `thumb`, `riscv`, `avr`, `windows`,
  `darwin`). A change that spans targets lists them: `thumb, riscv:`.
- The summary states the behaviour after the change ("a constant divisor
  is not paired with its remainder"), not the activity ("fix divmod").
- The body is evidence, not narrative. Name the test that proves the
  change, say that it failed against the previous code, and list what else
  was run (`make check`, the target's goldens, the full matrix).
- Name what the change does not handle rather than implying completeness.
- Do not add a `Co-Authored-By` line.

## Adding to the compiler

Each item below ends in the same two obligations: a test that fails without
the change, and an entry in the user documentation (the
[Using EmbCC](../manual/overview.md) manual) when users can see the change.

### An option

Options are parsed in the argument loop of `main` in
`src/driver/main.c`. Decide which of these the option is:

| Kind | What to do |
|---|---|
| EmbCC can honour it | implement it, and list it in `print_options` (the `--help` text) |
| EmbCC already behaves this way | accept it in the block that accepts such flags, and add the reason to that block's comment |
| accepting it unimplemented costs nothing (the objects are still correct) | accept it, and say in `--help` that it does nothing yet (`-fstrict-aliasing` is the model) |
| it promises something EmbCC does not do | refuse it by name with the reason, in the refusal block |

An unknown argument is already an error (`unknown argument '...'`); never
add a flag to a list that swallows it. Optimizer passes need no driver
code: `-f<pass>` and `-fno-<pass>` come from the pass table `g_pass` in
`src/opt/opt.c`, which `--help` also reads.

### An attribute

Attributes are listed in `attr_table` in `src/parse/parse.c`, each with a
disposition:

| Disposition | Meaning |
|---|---|
| `ATTR_HONOURED` | acted on |
| `ATTR_REFUSED` | ignoring it would change what the program computes; refused by name, printing the entry's reason |
| `ATTR_NOOP` | there is nothing to do here (what it asks for is already true, or it only affects a diagnostic EmbCC does not issue); accepted in silence |
| `ATTR_WARNED` | ignoring it is not a miscompile but loses something the program asked for; accepted with a warning that says what is lost |

The entry's text is printed to the user, so it must be true. An attribute
that is not in the table is warned about under `-Wattributes` and ignored.
An attribute whose effect is visible only in the object (`used`,
`visibility`, `section`) is tested by reading the object with another tool
(`tests/golden/attrs.sh`).

### A builtin

Builtins are lowered by name in `src/sema/sema.c`. Add the name to
`g_named_builtins` beside the dispatch, so that `__has_builtin` answers for
it; `tests/golden/has-feature.sh` extracts the names from the dispatch and
fails if one is handled but not listed. The lowering must work on every
target, or the target that cannot lower it must refuse it by name (for
example, `__builtin_sqrt` is refused on the soft-float targets, where it
would be a library call). Test it by running it on every target against
the host compiler, at every optimization level, with the inputs where the
obvious implementation is wrong (`tests/golden/float-bits.sh` is the
pattern).

### A warning

Warnings are listed in `g_warns` in `src/driver/diag.c` with their default
state and whether `-Wall` or `-Wextra` turns them on, and are emitted with
`diag_warn_opt` under that name. `--help-warnings` lists them. A warning
must not fire where gcc does not: `tests/golden/warnings.sh` and
`format-check.sh` compile the same code with gcc and require that every
line EmbCC warns about is one gcc warns about. Most of a warning's test is
code that must not warn.

### An IR operation

An operation is added to `enum ir_op` in `src/ir/ir.h`, and then every
consumer has to learn it. Most of them are hand-maintained `switch`
statements, so a missed one is a silent miscompile rather than a build
error. Find them by searching for an existing operation of the same kind;
for a terminator, search for `IR_UD2` and `IR_IGOTO`:

- the textual form: `ir_opname` in `src/ir/irprint.c` and the parser in
  `src/ir/irparse.c` (`tests/golden/ir-roundtrip.sh` checks print, parse,
  print);
- the optimizer (`src/opt/opt.c`): the operand walkers (`each_read`,
  `each_label`), `compute_defs`, `build_cfg` (leaders and successors),
  mem2reg's out-of-SSA step, `pass_cfgclean`, PRE, LICM, the unroller,
  the inliner's instruction remapping, and the verifier;
- the register allocator (`src/arch/regalloc.c`): `ra_each_use` and the
  liveness successors;
- every backend's lowering switch: lower it, or refuse it by name (the AVR
  backend's `a_refuse`).

Run the corpus with `EMBCC_VERIFY=1` (the runner always does) and make a
deliberately broken variant to see that the verifier catches it. When
adding a field to `struct ir_ins`, search for instructions built by hand in
passes and backends: they do not go through the IR builder's helpers, and
the field's default must be the safe value.

### A target

A target is a row in each of these:

1. `enum target_arch` in `src/arch/target.h`, and a family file in the
   target database, `src/targets/<family>.def`: its data model
   (`DATA_MODEL`) and its triples (`TRIPLE`), listed in
   `src/targets/targets.def`.
2. The relocation mapping (`target_reloc_type`) and the ELF machine
   (`target_elf_machine`).
3. The predefined-macro tables, generated by `tools/gen-predef.sh` from the
   target's reference compiler, and selected in `src/arch/predef.c`.
4. A backend directory `src/arch/<arch>/`: `codegen.c` (IR to machine code
   and the calling convention), `emit.c` (the encoder), `irgen.c` (the
   target's share of IR generation, `va_arg` at least), and `asm.c` for
   inline and file assembly. The backend declares its entry point in
   `src/arch/backend.h`, and the driver dispatches to it. It uses the shared
   register allocator through a `struct ra_target`
   (`src/arch/regalloc.h`).
5. Runtime routines the backend calls in `lib/rt`, and the target in
   `RT_EMBEDDED` and `tools/build-rt.sh`.
6. Linker support in `src/link/link.c` if EmbLD is to link it.
7. A harness `tests/harness/<arch>/` with `link.sh` and `run.sh`, and golden
   tests: the triples and data model, the encoder against a reference
   assembler, programs run against a reference compiler on the same board,
   the ABI pairing in both directions, and callee-saved register
   preservation.
8. `SRCS` and `EMBLS_SRCS` in the Makefile; the target's section in
   [Targets](../manual/targets.md).

Everything the new backend cannot lower is refused by name from its first
commit. A target is never "the other one" in a two-way test: code that
asked `target == X ? a : b` before the target existed has to be found and
turned into a question by name.

## Recording decisions and facts

A design decision goes into [decisions.md](decisions.md) with the reason
for it and what would reopen it, under a new `D-0NN` identifier;
identifiers are never reused, and code cites them. A decision that is later
reversed is marked superseded, not deleted.

A target fact that cost a debugging session (an ABI rule that is easy to
misread, a tool that misreports a field) goes into the page that describes
that target or component in this manual, so the next person does not pay
for it twice.

## Review and tests before merging

`make test` passing is the merge condition, and `make test-arm64` with it
for any change that can affect aarch64. The reviewer checks above all:

- does anything claim more than it does (an option accepted and ignored, a
  refusal turned into silence, a document or `--help` line ahead of the
  code);
- does a new test fail without the change ([Testing](testing.md#a-new-test-must-fail-first));
- does the change keep target knowledge in `src/arch/` and host knowledge
  in `src/platform/`.

The full matrix is slow, so it is run at milestones, not after every edit:

| Change | Before committing |
|---|---|
| any change, while working | `make check` |
| confined to one backend and its tests | `make check` and that target's golden tests |
| shared code (`src/opt`, `src/ir`, `src/sema`, `src/parse`, `src/arch/regalloc.c`, `src/arch/target.c`) | `make test` and `make test-arm64`; `tools/x86-identity.sh` when x86-64 output is meant to stay the same |
| register allocation, scratch registers, spilling | the above, and the exec goldens with `EMBCC_RA_MAXPOOL` at 3, 1 and 0 |
| a batch of related commits | the full matrix once, before moving to another area |

Say in the commit message which of these ran. Never build `embcc` or edit
`src/` or the test scripts while a suite is running: `make test` rebuilds
the compiler, and the tests read the tree.
