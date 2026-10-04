# Testing

This page describes how EmbCC is tested: the make targets, the test runner
and the directives it reads, the golden tests grouped by area, the QEMU
harnesses that run compiled code on each target, the reference tools the
tests compare against, and the environment variables that steer the suite
or stress the compiler. It is for anyone changing EmbCC who needs to know
which tests a change must pass and how to add one.

## Principles

- **The tests that count run the program.** A test that only checks that
  the compiler exits 0, or that an object looks well-formed, proves the
  object writer, not the compiler. Most of the suite compiles a program,
  runs it on the architecture it was compiled for, and asserts its exit
  status or its output.
- **The expected answer comes from a reference.** Where another
  implementation exists, the test compares against it: the target's gcc or
  clang, g++, nasm, llvm-mc, a disassembler, gdb, a Linux kernel, or the
  host's own arithmetic. See [Referees](#referees).
- **Agreeing with yourself proves nothing at a boundary.** Calling
  conventions, layouts and unwind tables are checked by linking an EmbCC
  half with a half built by another compiler and calling in both
  directions.
- **A skipped test is not a pass.** A test that cannot run on the current
  host says so and is counted as SKIP, with the reason.
- **A test is evidence only after it has failed.** See
  [A new test must fail first](#a-new-test-must-fail-first).

## Make targets

| Target | Builds first | Runs | When |
|---|---|---|---|
| `make check` | `embcc`, `libc-x86_64`, `libcxx-x86_64` | `tests/run.sh --exec-only`: the compiled-and-run corpus (`tests/exec/*.c`, `tests/exec/x86_64/*.c`, `tests/cxx/*.cc`) for `x86_64-elf` | while working |
| `make test` | `embcc`, `embread`, `embld`, `embdbg`, `embls`, `embas`, `libc-x86_64`, `libcxx-x86_64`, `libc-linux-x86_64`, `libcxx-linux-x86_64` | `tests/run.sh`: every shell test in `tests/exec`, `tests/compile`, `tests/golden` and `tests/golden/x86_64`, then the corpus, for `x86_64-elf` | before a commit |
| `make test-arm64` | `embcc`, `libc-aarch64`, `libcxx-aarch64`, `libc-linux-aarch64`, `libcxx-linux-aarch64` | `tests/run.sh --target=aarch64-elf`: `tests/golden/*.sh`, `tests/golden/aarch64/*.sh`, and the corpus compiled for `aarch64-elf` | before a commit that can affect aarch64 or shared code |
| `make test-libstdcxx` | `embcc` | `tests/golden/cxx-libstdcxx-embcc.sh` for `x86_64-elf` and `aarch64-elf`, with `EMBCC_LIBSTDCXX=1` | opt-in; builds libstdc++ from GCC's sources and takes minutes |

`make check` is not a unit-test suite. There is no separate unit-test
layer: the fast loop is the program corpus, which catches most front-end
and code-generation regressions because they show up as a program
returning the wrong answer. What `make check` does not check is what the
golden tests exist for: agreement with gcc, `-S` against `-c`, object
formats, interoperability, and every target other than `x86_64-elf`.

The embedded targets (ARMv7-M, ARMv8-M, RV32, RV64, AVR) have no make
target of their own. Their suites are golden tests that pick their own
`--target=` and run under `make test`; each one skips, with a reason, when
its QEMU or its reference compiler is missing.

`make test` does not build `embidx`, so `tests/golden/index.sh` reports
SKIP unless it exists; `make all` builds it. (Plain `make` with no target
builds only `build/embdbg_core.o`, the first rule in the Makefile, so
always name the target.)

Two further checks are run by hand:

- `EMBCC_KM1=1 sh tests/golden/x86_64/embbuild-kernel.sh` builds the
  EmbLinkOS kernel from its EmbBuild manifest with EmbCC, EmbAS and EmbLD
  alone, and boots it. It needs an EmbLinkOS tree (`EMBCC_MYOS`) and takes
  over a minute.
- `tools/x86-identity.sh [REV]` builds `embcc` at a baseline revision
  (default `HEAD`) and requires every x86-64 object it produces from the
  exec corpus (at `-O0`, `-O1`, `-O2` and with `-g`), and from the
  EmbLinkOS kernel when one is found, to be byte-identical to the working
  tree's. On a host that cannot run x86-64 code natively, this is how a
  change to shared code shows that it left the x86-64 backend alone. A
  change meant to alter x86-64 output fails it by design; inspect the
  difference, then move the baseline.

## The runner

```sh
tests/run.sh [--target=x86_64-elf|aarch64-elf] [--exec-only]
```

`tests/run.sh` must be run from a tree where `./embcc` is built; it exits
with an error otherwise. The default target is `x86_64-elf`.
`--exec-only` skips every shell test and runs only the corpus.

The runner exports these variables to every test:

| Variable | Value |
|---|---|
| `EMBCC` | the absolute path of `./embcc` |
| `EMBCC_TARGET` | the `--target=` the suite runs for |
| `EMBCC_VERIFY` | `1`: every optimizing compile runs the IR verifier (see [Compiler knobs](#compiler-knobs)) |
| `EMBCC_QEMU_TIMEOUT` | seconds a QEMU guest may run; `20` unless already set |
| `EMBCC_X86_RUNNER` | `host` on a Linux x86-64 host, `qemu` anywhere else, unless already set |

### Shell tests

Every `*.sh` under `tests/exec`, `tests/compile` and `tests/golden`, and
`tests/golden/<arch>` for the selected architecture, is a shell test. With
`--target=aarch64-elf` only `tests/golden/*.sh` and
`tests/golden/aarch64/*.sh` run; the refusal tests and the x86-64
toolchain tests drive the default target.

A shell test passes when it exits 0 **and** its output contains the line
`TEST-MARKER NAME`, where `NAME` is the script's file name without `.sh`.
Exit 0 without the marker is a failure ("did it run?"): the marker catches
a test that silently never ran.

A test that cannot run on this host prints a line beginning `skipped:`,
followed by the reason, and exits 0. The runner reports it as SKIP and
does not count it as a pass. Any other spelling (`SKIP:`, `skipping`) is
not recognized, and a test that uses one and exits 0 is counted as a
pass.
<!-- Note for the lead: 44 golden tests print "SKIP: ..." rather than
"skipped: ...", so on a host without their QEMU or reference compiler they
are reported as PASS. Listed in the report. -->

Shell tests run concurrently, `EMBCC_JOBS` at a time (default: the number
of online processors; `1` runs them serially, which is the setting to use
when debugging one test's output). Results are reported in list order, so
two runs can be compared line by line, and a failing test's whole output is
printed under its FAIL line.

### The corpus

`tests/exec/*.c` and `tests/exec/<arch>/*.c` are compiled by EmbCC at
`-O0`, linked, and run; `tests/cxx/*.cc` are compiled by EmbCC and linked
against the reference libstdc++ built by `tools/build-ref-gxx.sh` (SKIP
when it is absent). Each object is rebuilt from scratch on every run, so a
stale artifact cannot be measured.

Where the program runs depends on the target and the host:

- `x86_64-elf` with `EMBCC_X86_RUNNER=host`: linked with the host `cc
  -no-pie` and executed directly.
- `x86_64-elf` with `EMBCC_X86_RUNNER=qemu`, and `aarch64-elf`: linked by
  `tests/harness/<arch>/link.sh` into a bare-metal image and run by
  `tests/harness/<arch>/run.sh` under QEMU. C++ programs always use the
  harness.

The program's exit status must equal its `// expect-exit:` value. A run
that ends in status 124 is reported as "timed out after N s under QEMU";
status 125 as "the guest crashed or reset before exiting".

Directives the runner and the golden tests read from a corpus file:

| Directive | Meaning |
|---|---|
| `// expect-exit: N` | Required. The exit status the program must return. A file without it fails: it cannot assert anything. |
| `// target: TRIPLE` | Run the program only for this target; elsewhere it is reported as SKIP. Used for programs that contain one machine's inline assembly. |
| `// no-gcc-reference: REASON` | The gcc-differential tests (`agrees-with-gcc`, `optimizer`, `regalloc-O2`, `exec-Os`) skip the file and print the reason. Used where the cross gcc cannot build a reference, for example because it would call `libatomic`. |

Exec programs must be valid strict C11: `agrees-with-gcc` compiles each
one with the target's gcc at `-std=c11` and fails a program gcc rejects.

The final line is `P/T passed`, with the skip count when there were skips.
The runner exits non-zero on any failure, and also when it found no tests
at all.

## Writing a test

### An exec test

Add `tests/exec/NAME.c` with a `// expect-exit: N` line. Prefer a program
that computes its answer and returns a value that is only reached when
every check passed (42 is the convention), and that prints what it
computed: `agrees-with-gcc` and `regalloc-O2` compare stdout with gcc's
build as well as the exit status. Put a program that only one architecture
can compile in `tests/exec/<arch>/` or mark it `// target:`.

### A refusal test

THE RULE (see [Contributing](contributing.md#the-refusal-rule)) has its
own test: `tests/compile/reject-unimplemented.sh`. Each `check` case gives
a source, a pattern the diagnostic must contain, and optional flags. A
case passes only if the compile fails, the diagnostic matches, and the
diagnostic carries a `file:line` position. Cases are compiled with
`-Werror`, so a construct that is warned about rather than refused also
fails to compile. When a feature is implemented, its case graduates out of
this file and into an exec test. Target-specific refusals live in the
target's golden test (`avr-refuse.sh`, `thumb-codegen.sh`, ...).

### A golden test

A golden test is a POSIX `sh` script in `tests/golden/` (every target) or
`tests/golden/<arch>/` (one architecture). It must:

1. Print `TEST-MARKER NAME` first, then source `tests/lib.sh`:

   ```sh
   #!/bin/sh
   # What property this checks, and against what reference.
   set -u
   echo "TEST-MARKER NAME"
   . "$(dirname "$0")/../lib.sh"
   ```

2. Write only inside `tests/golden/out/NAME/`. Golden tests run in
   parallel, and the harness link scripts take a directory variable
   (`EMBCC_THUMB_HARNESS`, `EMBCC_RISCV_HARNESS`, `EMBCC_M33_HARNESS`,
   `EMBCC_AVR_HARNESS`, `EMBCC_HARNESS_WORK`) so that harness objects are
   built in the test's own directory rather than shared.
3. Say `skipped: REASON` and exit 0 when a prerequisite (a QEMU binary, a
   reference compiler, a tree) is missing.
4. Exit non-zero with a one-line explanation on the first disagreement.

`tests/lib.sh` provides the host's tool paths (from `tools/hostpaths.sh`)
and target-generic helpers for tests that run under any `--target=`:

| Helper | Does |
|---|---|
| `t_gcc_c SRC -o OBJ [FLAGS]` | compile with the target's reference gcc |
| `t_link EXE OBJ...` | link a hosted test program for the target (host `cc` or the harness) |
| `t_run EXE` | run it; prints its stdout and returns its exit status |
| `x86_gcc_c`, `x86_link`, `x86_run`, `x86_gcc_exe` | the same, for x86-64 only |
| `pinned_elsewhere FILE` | true if FILE says `// target:` for another target |
| `no_gcc_reference FILE` | true, and prints why, if FILE says `// no-gcc-reference:` |

It also sets `TARGET` and `ARCH` from `EMBCC_TARGET`, and `EMBCC_ROOT` to
the repository root.

To run one golden test by hand, run it from the repository root with the
variables the runner would set:

```sh
EMBCC=$PWD/embcc EMBCC_VERIFY=1 sh tests/golden/riscv-exec.sh
EMBCC=$PWD/embcc EMBCC_TARGET=aarch64-elf EMBCC_VERIFY=1 sh tests/golden/regalloc-O2.sh
```

The library tests link the archives under `build/`, so rebuild them
(`make libc-x86_64 libcxx-x86_64`, and the `-linux-` and `aarch64`
variants) before trusting a hand-run result; `make test` does that itself.
A fresh checkout also needs `embld`, `embidx` and the other tools built
before a FAIL from a test that uses them means anything.

### A new test must fail first

A new test is evidence only after it has been seen to fail against the
code it is meant to catch. Revert the fix (or build the previous revision
in a separate worktree), run the test, confirm that it fails for the
reason it was written for, then restore the fix. A test that passes either
way is worse than none, because it is reported as coverage. The usual
ways a test turns out vacuous:

- the optimizer folds the case away, so the code path is never reached
  (a `static` function inlined into a constant);
- the check reads back nothing and compares nothing (a pipeline that
  silently produced empty output, decoded as zeros on both sides);
- the property the test prints does not depend on the code under test.

When a change removes a refusal, search `tests/` for the refusal's text:
a test that pinned the refusal now fails, as it should, and has to be
changed into a test of the new behaviour.

## Golden tests

One line per test, grouped by area. The header comment of each script says
in full what it checks and why.

### The exec corpus, differentially

| Test | Checks |
|---|---|
| `agrees-with-gcc` | every exec program built by EmbCC (`-O0`) and by the target's gcc (`-std=c11`); both run; exit status and stdout must match |
| `optimizer` | `-O0` output is byte-identical to the no-flag output; every exec program keeps its `expect-exit` value at `-O1`; dead-code cases |
| `regalloc-O2` | every exec program at `-O2` against gcc: exit status and stdout |
| `exec-Os` | every exec program at `-Os` against gcc |
| `libc` | the exec corpus linked against EmbCC's own C library with no newlib at all |
| `cxx-agrees-with-gxx` | every `tests/cxx` program built by EmbCC and by the reference g++, run on the harness; exit status and output must match |

### Interoperability and calling conventions

| Test | Checks |
|---|---|
| `sysv-abi` | struct passing under System V: half the program from gcc, half from EmbCC, calling each other |
| `struct-abi-edges` | struct and float arguments at the edges of the convention (too few registers left, over-aligned, packed, padding eightbytes), both directions, `-O0` and `-O2` |
| `stack-args` | the seventh parameter onward, arriving on the stack, at `-O2` |
| `cross-varargs` | varargs and floating aggregates across the EmbCC/gcc boundary, including `va_list` handed to newlib's `vsnprintf` |
| `complex-abi` | `_Complex` float, double and long double across the boundary |
| `int128-abi` | `__int128` across the boundary, including AAPCS64 even-register pairs and stack alignment |
| `ldouble-abi` | `long double` across the boundary (x87 extended, IEEE binary128) |
| `ldouble-same` | `long double` where it is a double (ARM) or a float (AVR) |
| `struct-layout` | struct, union and bit-field layout on every target against the reference compiler of that target's data model |
| `win-abi` | the Microsoft x64 convention, argument placement checked against clang by disassembly |
| `win-abi-run` | Win64 code called from gcc-built code through `ms_abi` pointers in an x86-64 test image, `-O0` to `-O2` |
| `darwin-abi` | Apple's arm64 convention and data model, clang half and EmbCC half, run natively (macOS on Apple silicon only) |
| `cxx-abi` | EmbCC's C++ linked with g++'s: mangling, layouts, vtables, VTTs, classes by value, in both directions |
| `eh-regions` | EmbCC's exception regions against exceptions thrown by g++-built code |
| `unwind-through` | a C++ exception unwinding through EmbCC-compiled C frames using only their `.eh_frame`, at `-O0` and `-O2` |

The embedded ABI pairing (`tests/golden/embedded-abi-caller.c`,
`embedded-abi-callee.c`, `embedded-abi.h`) has no script of its own: it
runs inside `thumb-exec`, `riscv-exec` and `avr-wide`, which build every
pairing of EmbCC and the reference compiler for caller and callee. AVR is
paired against the documented avr-gcc rules instead (`avr-abi`), because
clang's AVR struct convention is not avr-gcc's.

### ARMv7-M and ARMv8-M

| Test | Checks |
|---|---|
| `thumb-target` | the ARMv7-M triples and data model |
| `thumbv8m-target` | ARMv8-M Mainline (Cortex-M33) as a level of the Thumb target: triples, macros, object attributes, an image run on the M33 board |
| `thumb-codegen` | objects are ELF32 ARM, every `.text` instruction decodes, relocations are ARM ones, refusals fire by name |
| `thumb-encoding` | every Thumb-2 encoder, disassembled by `llvm-objdump` and compared with what it was meant to be |
| `thumb-vfp` | the VFP instruction vocabulary against `llvm-mc` |
| `thumb-asm` | the inline-assembly vocabulary against `llvm-mc`, operand handling, refusals |
| `thumb-exec` | programs compiled by EmbCC and by clang, linked by `embld`, run on the Cortex-M3 board at `-O0`, `-O1`, `-O2`, `-Os`; 64-bit, float, aggregate, varargs and ABI programs against the host |
| `thumb-relax` | 16-bit branch forms at both sides of every reach limit |
| `thumb-far` | conditional branches beyond the 1 MB reach of `B<c>.W` |
| `thumb-fpu` | single-precision arithmetic on the Cortex-M4F FPU, run on an M4 board |
| `thumb-hardfp` | `-mfloat-abi=hard` against clang in both directions on the M4F and M33 boards |
| `thumb-atomic` | C11 atomics and `__sync` builtins under a SysTick interrupt updating the same variables |
| `thumb-calleesave` | every function preserves r4-r11 and sp, checked by an assembly probe |
| `arm-abi-tags` | `.ARM.attributes` contents and `embld`'s refusal to link mismatched float ABIs |

### RISC-V

| Test | Checks |
|---|---|
| `riscv-target` | the RV32 and RV64 triples and data models |
| `riscv-encoding` | every RISC-V encoder round-tripped through `llvm-mc` and a disassembler |
| `riscv-compressed` | the C extension against `llvm-mc` over tens of thousands of instructions |
| `riscv-asm` | the inline-assembly vocabulary against `llvm-mc` |
| `riscv-exec` | programs compiled by EmbCC and by clang, run on QEMU `virt` at both widths, plus the ABI pairing |
| `riscv-relax` | branch and jump relaxation at the reach limits |
| `riscv-atomics` | the A extension at both widths |

### AVR

| Test | Checks |
|---|---|
| `avr-target` | the AVR data model (2-byte `int` and pointers, 4-byte `double`) and target refusals |
| `avr-encoding` | the instruction vocabulary against `llvm-mc`, including decoding EmbCC's bytes back for branch forms |
| `avr-asm` | EmbCC's AVR assembler against `llvm-mc` |
| `avr-exec` | programs run on the ATmega328P board against the same source run on the host |
| `avr-abi` | the calling convention against avr-libc's documented rules, with hand-written assembly on the other side |
| `avr-calleesave` | every function preserves r2-r17 and Y |
| `avr-float` | software binary32 on the part, bit for bit against the host |
| `avr-softfp-host` | the same float routines built for the host, against native float |
| `avr-inline-asm` | operand constraint classes, byte modifiers, refusals |
| `avr-io16` | 16-bit I/O registers written high byte first, read low byte first |
| `avr-isr` | `signal` and `interrupt` handlers and the vector table |
| `avr-narrow` | computing only the bytes a use reads |
| `avr-narrow-cmp` | narrow comparisons at every level against the host |
| `avr-refuse` | what the backend refuses, by name |
| `avr-shared` | the shared embedded programs (64-bit, aggregates, varargs) on the part |
| `avr-wide` | 64-bit integers, varargs and aggregates by value, plus the ABI pairing |

### Every embedded target

| Test | Checks |
|---|---|
| `embedded-runtime` | every runtime routine each embedded backend can call is in that target's `librt.a`, exactly once |
| `bitops-width` | `ctz`, `clz`, `popcount`, `ffs`, `parity`, `clrsb` at each target's type widths |
| `cvt-fold` | constant conversions folded by the optimizer agree with run-time conversion and the host |
| `double-pairs` | 64-bit values in register pairs on RV32 and Cortex-M3, with the pair allocation forced on and off |
| `vla-exec` | variable-length arrays on Cortex-M3, RV32 and RV64 |
| `ptr-compound` | pointer `+=` and `-=` at the target's address width |
| `debug-embedded` | `-g` on the embedded targets: DWARF checked by `llvm-dwarfdump` and gdb |
| `embdbg-remote` | EmbDBG's remote-protocol client stopping a QEMU guest at a function and reading its arguments |

### Language and front end

| Test | Checks |
|---|---|
| `c23` | the C23 features, each checked for its effect |
| `gnu-c` | GNU C extensions used in kernel macros, against the host compiler |
| `has-feature` | the `__has_*` operators in C; `__has_builtin` agrees with the builtin dispatch |
| `typeof-attrs` | `typeof` on parameters and expressions; attributes on typedefs |
| `attrs` | attributes whose effect is in the object (`used`, `visibility`) |
| `alignment` | object alignment carried into section and symbol alignment |
| `overflow-builtins` | `__builtin_{add,sub,mul}_overflow` at the boundaries, against the host |
| `float-bits` | `fabs`, `copysign`, `signbit` and the classification builtins on every target |
| `float-types` | `_Float32`, `_Float64`, `_Float128` and friends, per target |
| `int-constants` | integer literal types and constant expressions on every target |
| `stdint` | `<stdint.h>` widths and limits on every target against the reference compiler |
| `rodata-const` | const objects in `.rodata`, non-const in `.data` |
| `static-init` | constant initialization versus `.init_array`, and that a null dereference still faults in the harnesses |
| `include-next` | `#include_next` |
| `predef` | the predefined-macro table of each target against its reference compiler |
| `atomic-operators` | operators on `_Atomic` objects are atomic read-modify-writes on every target |
| `volatile-access` | every volatile access happens, on every target, at every level |

### Optimizer

| Test | Checks |
|---|---|
| `algebra` | algebraic identities and constant reassociation, checked on the IR and by running against gcc |
| `alias` | alias analysis and the passes that rely on it, weighted toward references that do alias |
| `loopopt` | LICM, loop rotation and the value numbering rotation depends on |
| `pre` | partial redundancy elimination |
| `sroa` | splitting private aggregates into scalars |
| `unroll` | loop unrolling |
| `vectorize` | auto-vectorization fires and computes the same checksums as `-O0`, `-O1` and gcc |
| `tailcall` | sibling calls run in constant stack |
| `frameless` | leaves with no frame, and the conditions that make it safe |
| `branches` | short branch encodings where they reach |
| `parallel-move` | `ra_parallel_move` simulated over every small shape (`tools/pmovecheck`) |
| `remarks` | optimization remarks and `embcc why`: different causes give different reasons |
| `provenance` | every IR instruction keeps a source location through every pass |
| `ir-roundtrip` | EmbIR's textual form: print, parse, print reproduces the text |
| `inspect` | `embcc inspect` shows the optimizer's work between two levels |

### Driver, output and object formats

| Test | Checks |
|---|---|
| `asm-S` | `-S` output assembled by the GNU assembler reproduces the `-c` object (x86-64 only; skips elsewhere) |
| `asmout-roundtrip` | the same property on every target |
| `gas-file` | `.s` and `.S` input files on the embedded targets |
| `triple` | target triples parse to architecture, OS and object format; old spellings keep their meaning |
| `default-target` | `DEFAULT_TARGET`, `EMBCC_DEFAULT_TARGET` and `--target=` precedence, and the separation from `EMBCC_TARGET` |
| `install` | the compiler finds its headers and libraries relative to its own binary |
| `version` | `--version` names the target and the language and admits what is missing |
| `driver-deps` | `-M`, `-MM`, `-MD`, `-MMD`, `-MF`, `-MT`, `-MP` and `-fsyntax-only` |
| `stack-usage` | `-fstack-usage` output in gcc's format |
| `sanitize` | `-fsanitize=undefined` in trap mode on every target |
| `tools-build` | every tool in `make all` links |
| `host-agnostic` | EmbCC's output does not depend on which host compiler built EmbCC |
| `macho` | the Mach-O writer, judged by the macOS SDK's structures and the system linker (macOS only) |
| `windows` | the COFF writer, judged by independent readers and a real linker |
| `linux` | the Linux target: a static image with no glibc, run as PID 1 on a real Linux kernel |
| `interfaces` | stable symbol identity and interface hashes (`--emit-interfaces`) |
| `index` | `embidx`: a header edit that changes no interface rebuilds nothing |

### Diagnostics and tools

| Test | Checks |
|---|---|
| `diagnostics` | located headings, source line and caret, across includes |
| `diagnostics-explain` | `--explain` entries exist for every diagnostic id the compiler can print |
| `diagnostics-fix` | `-fdiagnostics-parseable-fixits` and `--fix`: the fixed file compiles |
| `diagnostics-json` | `-fdiagnostics-format=json`; applying its fix-its makes the file compile |
| `diagnostics-recovery` | several independent errors reported in one run, and no invented ones |
| `warnings` | each warning's name, group and switch; EmbCC warns where gcc warns |
| `warnings-uninit` | `-Wuninitialized` and `-Wmaybe-uninitialized`, mostly cases that must not warn |
| `format-check` | `-Wformat`: every line EmbCC warns about, gcc warns about too |
| `embls` | the language server over a real LSP session |
| `embld-doctor` | `embld --doctor` names the cause of a failed link |
| `link-dwarf` | EmbLD carries DWARF into the image; gdb on a QEMU guest |
| `debug-line` | DWARF line tables read by gdb; `-g` off leaves the object unchanged |
| `debug-locals` | subprogram, parameter and local DIEs; frame offsets checked against codegen's slots |
| `debug-live` | gdb stops a running program on the harness and reads its actual state |

### Libraries and runtime

| Test | Checks |
|---|---|
| `libc-emblinkos` | the same portable library builds for EmbLinkOS with one different file |
| `malloc` | the allocator: disjoint blocks, content kept, time linear in the work |
| `threadsafe` | `malloc`, stdio and function-local statics under threads |
| `rt` | `lib/rt`, the compiler runtime, against the host's runtime and libgcc |
| `unwind` | EmbCC's unwinder against libgcc's, by a program whose output records what ran |
| `libcxx` | EmbCC's C++ runtime (`lib/libcxx`): new/delete, guards, RTTI, the personality routine |
| `libcxx-std` | `<type_traits>`, `<utility>`, `<limits>` and the `<c*>` headers |

### C++

| Test | Checks |
|---|---|
| `cxx-access` | `private` and `protected` enforced, and every valid use still accepted |
| `cxx-format-check` | `std::format` strings checked at compile time, agreeing with g++ |
| `cxx-libstdcxx` | `tests/libstdcxx` programs against the reference libstdc++ |
| `cxx-libstdcxx-embcc` | libstdc++ and libsupc++ built by EmbCC, then every C++ program linked with them (opt-in) |
| `cxx-libsupcxx` | libsupc++ compiled by EmbCC from GCC's sources |
| `cxx-no-rtti` | `-fno-rtti` as g++ means it |
| `cxx-noexcept` | an exception leaving a `noexcept` function calls `std::terminate` |
| `cxx-reject` | ill-formed C++ refused with a located diagnostic |
| `cxx-std` | `__cplusplus` and feature macros per `-std=`, against g++ |

### x86-64 only (`tests/golden/x86_64/`)

| Test | Checks |
|---|---|
| `assembler` | EmbAS output byte-identical to `nasm -f elf64` |
| `data-symbols` | data symbols, sections and their relocations, with `readelf` |
| `relocations` | external calls as `R_X86_64_PLT32` against undefined globals |
| `symtab` | symbol binding, sizes and intra-unit calls |
| `weak-undef` | weak references emitted as weak undefined symbols |
| `empty-object` | the ELF writer's minimal object accepted by `readelf` and `objdump` |
| `inline-asm` | extended inline asm with fixed-register constraints |
| `inline-asm-kernel` | the kernel's privileged inline-asm instructions, decoded by `objdump` |
| `newlib-headers` | real newlib headers through the preprocessor and predefined macros |
| `embld-link` | EmbLD links freestanding programs that run on the host |
| `embld-sections` | orphan sections gathered contiguously, with bracket symbols in both spellings |
| `embld-b1` | EmbLD links a program against the EmbLinkOS runtime and newlib |
| `embld-embdbg` | EmbLD writes the `.embdbg` sidecar at link time |
| `embld-embdbg-multi` | several `-g` objects merged into one `.embdbg` |
| `embdbg-symbolize` | addresses to function and `file:line` |
| `embdbg-inspect` | parameters and locals in scope with their types |
| `embdbg-disasm` | instruction boundaries against `objdump` |
| `embdbg-crash` | a kernel fault dump turned into a diagnosis |
| `embdbg-embdbg` | the native `.embdbg` format round-tripped |
| `embdbg-tui` | the TUI's non-terminal fallback |
| `embx-embread` | `embread` against real EMBX images, intact and corrupted |
| `emblinkos-sdk` | the EmbLinkOS sval SDK compiled by EmbCC, run against a gcc-built driver |
| `emblinkos-cxx` | the OS's C++ demo compiled by EmbCC, run by the OS |
| `emblinkos-cxx-onos` | C++ compiled on EmbLinkOS by the EmbCC that runs there |
| `self-host` | EmbCC compiles all of its own sources and EmbLD links them; codegen is deterministic |
| `embbuild` | EmbCC built from its EmbBuild manifest |
| `embbuild-kernel` | the EmbLinkOS kernel built from its manifest and booted (opt-in, `EMBCC_KM1=1`) |

The OS-dependent tests in this directory skip when no EmbLinkOS tree or
cross runtime is present.

### aarch64 only (`tests/golden/aarch64/`)

| Test | Checks |
|---|---|
| `arm64-encoding` | every aarch64 encoder, disassembled by `aarch64-elf-objdump` |
| `arm64-asm` | the inline-assembly vocabulary and the ARM kernel's templates against `aarch64-elf-as` |

## Harnesses

`tests/harness/<arch>/` holds what it takes to run one program on one
machine: a `link.sh` that turns an EmbCC object into a bootable image, a
`run.sh` that boots it under QEMU, and the start-up code between reset and
`main`. Only the object under test comes from the test; the harness is
scaffolding.

| Harness | Machine | Program output | How the result gets out | Harness built with |
|---|---|---|---|---|
| `x86_64` | `qemu-system-x86_64 -cpu max -m 128M`, a Multiboot image loaded with `-kernel` | the debug console (`-debugcon stdio`) | `sys.c` prints `@@EMBCC-EXIT n@@`; `run.sh` strips it and exits with `n` | `x86_64-elf-gcc` and newlib |
| `aarch64` | `qemu-system-aarch64 -M virt -cpu cortex-a72 -semihosting` | semihosting | semihosting carries the exit status | `aarch64-elf-gcc` and newlib |
| `thumb` | `qemu-system-arm -M lm3s6965evb -cpu cortex-m3` | UART | output sentinel | EmbCC and `embld` |
| `thumb-m4f` | `qemu-system-arm -M mps2-an386 -cpu cortex-m4` (with FPU) | UART | output sentinel | EmbCC and `embld` |
| `thumb-m33` | `qemu-system-arm -M mps2-an505 -cpu cortex-m33` | UART | output sentinel | EmbCC and `embld` |
| `riscv` | `qemu-system-riscv32` or `-riscv64 -M virt -bios none -m 8` | UART | output sentinel; `boot.c` stops QEMU through the SiFive test device | EmbCC and `embld` |
| `avr` | `qemu-system-avr -M uno` (ATmega328P), image loaded with `-bios` | USART0 | output sentinel | EmbCC (including `boot.S`) and `embld` |
| `linux` | a real Linux kernel under QEMU, the program as `/init` | the console | the kernel's panic message for PID 1 carries the exit status | the program is a static `*-linux-gnu` image |

Exit statuses from `run.sh`:

- `x86_64` and `aarch64` return the guest's exit status, `124` when the
  run timed out, and `125` when the guest crashed or reset. Address zero is
  unmapped in both, so a null dereference faults; on aarch64 the vector
  table prints `ESR`, `FAR` and `ELR` before exiting 125.
- `thumb`, `thumb-m4f`, `thumb-m33`, `riscv` and `avr` always exit 0. A
  bare-metal image on these boards has nowhere to return to, so the run is
  judged by its output: each test program prints a sentinel when `main`
  finishes (`==END==` in the shared embedded programs), and the test fails
  when the sentinel is missing.
- `linux` exits 126 when no kernel is available, so the caller skips.
  Supply one as `tests/harness/linux/vmlinuz-x86_64` or `-aarch64`, or
  name it with `EMBCC_LINUX_KERNEL_X86_64` or `EMBCC_LINUX_KERNEL_AARCH64`.

`tests/harness/qrun.sh SECONDS [--until TEXT] CMD...` runs QEMU under a
hard timeout. With `--until TEXT` it polls the guest's output and kills
QEMU as soon as `TEXT` appears, which turns a run that would cost the full
timeout into one that ends when the program does. The `thumb-m33` and
`avr` run scripts pass `EMBCC_QEMU_UNTIL` through as `--until`.

`tests/harness/crt.c` is the start-up shared by the `x86_64` and `aarch64`
harnesses: it registers the unwind tables, runs `.ctors` and
`.init_array`, calls `main`, and leaves through `exit` so that `atexit`
handlers and stdio flushing run.

## Referees

| Reference | Used for | Tests |
|---|---|---|
| the target's gcc (`x86_64-elf-gcc` or host `cc`, `aarch64-elf-gcc`) | what a C program computes; half of each SysV/AAPCS64 ABI pairing; warnings that must agree | `agrees-with-gcc`, `optimizer`, `regalloc-O2`, `exec-Os`, `sysv-abi`, `struct-abi-edges`, `cross-varargs`, `complex-abi`, `int128-abi`, `ldouble-abi`, `format-check`, `warnings` |
| the reference g++ and libstdc++ (`tools/build-ref-gxx.sh`) | what a C++ program computes; mangling, layout and EH interoperability | `cxx-agrees-with-gxx`, `cxx-abi`, `cxx-libstdcxx`, `cxx-std`, `cxx-format-check`, `eh-regions`, `unwind-through` |
| clang | the embedded targets' reference compiler; Win64 and Apple arm64 conventions; predefined macros and data models for the targets gcc is not installed for | `thumb-exec`, `riscv-exec`, `thumb-hardfp`, `win-abi`, `darwin-abi`, `predef`, `stdint`, `struct-layout` |
| `llvm-mc` | the bytes of every instruction form the assemblers and encoders produce | `thumb-asm`, `thumb-vfp`, `riscv-asm`, `riscv-compressed`, `riscv-encoding`, `avr-encoding`, `avr-asm` |
| `llvm-objdump`, `aarch64-elf-objdump`, `objdump` | what the emitted bytes decode to | `thumb-encoding`, `thumb-codegen`, `arm64-encoding`, `inline-asm-kernel`, `embdbg-disasm` |
| `aarch64-elf-as`, the GNU assembler | assembling the same text EmbCC assembles; reassembling `-S` output | `arm64-asm`, `asm-S` |
| `nasm` | EmbAS | `assembler` |
| `readelf`, `nm`, `file`, the platform linkers | object and image structure | `relocations`, `symtab`, `data-symbols`, `macho`, `windows`, `linux` |
| gdb | DWARF, read by a real debugger, on a running program | `debug-line`, `debug-locals`, `debug-live`, `link-dwarf`, `debug-embedded` |
| the host | arithmetic that has one answer at any register width: 64-bit integers, IEEE results bit for bit | `avr-exec`, `avr-float`, `avr-softfp-host`, the 64-bit and float halves of `thumb-exec` and `riscv-exec`, `cvt-fold`, `double-pairs` |
| a Linux kernel | syscall numbers, flags and structure layouts in `lib/libc/os/linux` | `linux` |
| written rules | where no compatible compiler exists: avr-libc's calling-convention rules | `avr-abi` |
| simulation | `ra_parallel_move` over every small shape | `parallel-move` |

A referee grades only what it is shown. A vocabulary generated from the
encoder's own tables (`tools/thumbcheck`, `riscvcheck`, `avrcheck`,
`vfpcheck`, `a64check`, `tasmcheck`, `rvasmcheck`, `avrasmcheck`) cannot
miss a form that was added to the encoder; an encoder function that is not
in the vocabulary is unchecked however many forms the vocabulary reports.
Where a reference assembler emits a placeholder (a relocated branch), the
comparison runs the other way: the reference disassembles EmbCC's bytes.

## Environment variables

### Suite and harness

| Variable | Effect |
|---|---|
| `EMBCC` | the compiler under test; set by the runner, required by most golden tests when run by hand |
| `EMBCC_TARGET` | the target the suite runs for; read by `tests/lib.sh`. Not read by the compiler (see `EMBCC_DEFAULT_TARGET`) |
| `EMBCC_JOBS` | how many shell tests run at once |
| `EMBCC_QEMU_TIMEOUT` | seconds a guest may run (runner default 20; the embedded `run.sh` scripts default to 10 when run alone) |
| `EMBCC_QEMU_UNTIL` | a sentinel that ends a `thumb-m33` or `avr` run as soon as the guest prints it |
| `EMBCC_X86_RUNNER` | `host` or `qemu`: where x86-64 programs run |
| `EMBCC_KM1` | `1` enables `embbuild-kernel` |
| `EMBCC_LIBSTDCXX` | `1` enables `cxx-libstdcxx-embcc`; `EMBCC_LIBSTDCXX_LIB` names a library already built |
| `EMBCC_QEMU_X86`, `EMBCC_QEMU_AARCH64`, `EMBCC_QEMU_ARM`, `EMBCC_QEMU_RISCV`, `EMBCC_QEMU_AVR` | the QEMU binary for each harness |
| `EMBCC_X86_NEWLIB`, `EMBCC_AARCH64_NEWLIB`, `EMBCC_X86_GCC`, `EMBCC_AARCH64_GCC`, `EMBCC_X86_LD`, `EMBCC_AARCH64_LD`, `EMBCC_REF_GXX` | the cross toolchains and newlib the x86-64 and aarch64 harnesses link with |
| `EMBCC_MYOS`, `EMBCC_MYOS_BUILD` | the EmbLinkOS tree and its build directory |
| `EMBCC_LINUX_KERNEL_X86_64`, `EMBCC_LINUX_KERNEL_AARCH64` | the kernels the `linux` harness boots |
| `EMBCC_REF_GCC_THUMB`, `EMBCC_REF_GCC_RISCV`, `EMBCC_LLVM_MC`, `EMBCC_LLVM_OBJDUMP` | the reference compiler and LLVM tools the embedded tests use |

`tools/hostpaths.sh` is the one place the host layout is known: each path
tries the current machine's layout first and can be set outright from the
environment.

### Compiler knobs

These are read by the compiler itself. They exist for testing and for
finding a miscompile; none of them is needed to use EmbCC.

| Variable | Effect |
|---|---|
| `EMBCC_VERIFY` | when set, the IR verifier runs after IR generation and after the passes of every optimizing compile, and stops the compile with an `internal:` error if a pass dropped a live value, left a stale scope index, or built an instruction without a source location; an optimizer that fails to converge is an error instead of a warning. The runner always sets it. |
| `EMBCC_RA_MAXPOOL=N` | the shared register allocator hands out only the first `N` registers of each pool. Run the exec goldens with small `N` (3, 1, 0) to reach the spilled-operand paths that a full pool reaches only in rare functions. |
| `EMBCC_RA_WHY=1` | one line per function on stderr accounting for every value that did not get a register |
| `EMBCC_RA_TRACE=1` | every allocated or eligible value, with its live range, hint and location |
| `EMBCC_RA_DEGREE_SPILL=1` | the previous spill choice (highest degree first), for bisecting a difference to it |
| `EMBCC_RA_POOL_K=1` | the previous colourability test for a value that crosses a call (the whole pool, not the callee-saved registers), for bisecting a difference to it |
| `EMBCC_T_PAIRS`, `EMBCC_RV_PAIRS` | `1` forces the 64-bit register-pair allocation on, `0` forces it off, on Thumb and on RISC-V; unset, the backend generates each function both ways and keeps the shorter. `EMBCC_T_PAIRS_ONLY=FN` and `EMBCC_RV_PAIRS_ONLY=FN` turn pairs on in function `FN` only. |
| `EMBCC_T_RA_MAX=N`, `EMBCC_RV_RA_MAX=N` | keep only the first `N` values in registers on Thumb or RISC-V; bisecting `N` names the one value whose register breaks a program |
| `EMBCC_AVR_RA_MODE=n` | force one AVR allocation mode; each mode is otherwise generated and the shortest kept, so a mode that never wins is still tested |
| `EMBCC_AVR_RA=1`, `EMBCC_AVR_RA_ONLY=FN`, `EMBCC_AVR_RA_LIMIT=K` | AVR allocation report, one function only, first `K` homes only |
| `EMBCC_T_NOWIDEIMM`, `EMBCC_NO_SIGNTEST` | build a 64-bit constant whole on Thumb, and keep `x >> 63` a shift before a branch, for bisecting |
| `EMBCC_NO_MEMOFF`, `EMBCC_NO_RMW`, `EMBCC_NO_MLA`, `EMBCC_NO_SPLITLOOPS`, `EMBCC_NO_TAILCALL` | turn off one transformation (constant offsets folded into loads and stores on Thumb and RISC-V; x86-64 read-modify-write fusion; multiply-accumulate fusion on aarch64 and Thumb; loop live-range splitting; tail calls on aarch64, Thumb and RISC-V) for bisecting |
| `EMBCC_RV_JAL_RANGE=BYTES`, `EMBCC_RV_LONG_CALLS` | shrink the reach the RISC-V backend assumes for `jal`, or (when set) call functions in the same unit with `auipc`+`jalr` instead of `jal`, to test the long-call path without a megabyte of code |
| `EMBCC_T_FPU` | `1` or `0` overrides whether the Thumb backend uses the FPU |
| `EMBCC_VECDEBUG` | the vectorizer prints, on stderr, each loop it considers and why it rejected it |

`EMBCC_DEFAULT_TARGET` and `EMBCC_PREFIX` are user-facing and documented in
[Invoking EmbCC](../manual/invoking.md).
<!-- UNVERIFIED: the AVR knobs EMBCC_AVR_NO_OCT, EMBCC_AVR_NO_VOL,
EMBCC_AVR_NO_XHOME, EMBCC_AVR_RA_CAP and EMBCC_AVR_REMAT_MAX exist in
src/arch/avr/codegen.c; their exact meaning was not checked, so they are
not listed. -->

## Which tests a change needs

| Change | Run |
|---|---|
| anything, while working | `make check` |
| one backend (`src/arch/<arch>/` and its tests) | `make check`, then that target's golden tests (for Thumb: `thumb-*`, `arm-abi-tags`; for RISC-V: `riscv-*`; for AVR: `avr-*`), plus `regalloc-O2` and the embedded shared tests it can affect |
| shared code (`src/opt`, `src/ir`, `src/sema`, `src/parse`, `src/arch/regalloc.c`, `src/arch/target.c`) | `make test` and `make test-arm64`, and `tools/x86-identity.sh` when x86-64 output is not meant to change |
| register-allocator, scratch-register or spill changes | the above with `EMBCC_RA_MAXPOOL` at 3, 1 and 0 |
| C++ front end or `lib/libcxx` | `make test`, `make test-arm64`; `make test-libstdcxx` at milestones |

Capture a suite's full output to a file and read the FAIL lines from it;
do not pipe a suite through `tail` or `head`, which cuts the failures (they
come before the summary) and can kill the run early. Do not rebuild
`embcc` or edit the tree while a suite is running: `make test` rebuilds
the compiler, and the tests read the sources.
