# Redesign: from EmbLinkOS's compiler to an embedded toolchain

Status: **approved 2026-10-08, in progress.** Done so far:
- branding: the README, the documentation index, the overview, `--version`;
- `verify/fuzz`: the fuzzers in the tree;
- EmbSim phase 1: modules, and a GDB server;
- the backend registry (`src/arch/backends.c`) and each target's `-m`
  options in `src/arch/<arch>/options.c`;
- the optimizer split: `src/opt/opt.c` is the pass manager and each pass
  is a file of its own (40 files instead of one of 17,551 lines), with
  the compiler's output byte-identical.

## Why

EmbCC began as the compiler for EmbLinkOS on x86-64, and the project
still says so. The README's first paragraph names EmbLinkOS on x86-64
as the primary target. Its target table lists 10 targets and says C++
runs only on x86-64 and AArch64. In fact EmbCC compiles C and C++ for 18
embedded targets, has an assembler for each, and ships a linker, a
debugger, a simulator, a flash programmer, a tracer and seven more tools.

The goal is now to be the best compiler and toolchain for embedded
systems, while staying a very good compiler for operating systems. The
code, the repository and the documentation should be organised around
that goal. Three facts show where the current shape costs most.

1. **A target is spread through the code.** RX, the most recent target,
   is named 44 times in 11 files of `src/`, mostly `arch/target.c` (17)
   and `driver/main.c` (14). It also appears in the linker, EmbDbg,
   EmbSim, the Makefile, `build.ebm`, a harness directory and the docs.
   Adding a target means editing all of these, and two target branches
   always conflict in the same lists.
2. **The largest files hold many concerns.** `opt/opt.c` is 17,551
   lines holding every optimisation pass. `driver/main.c` is 6,820
   lines: options, target selection, the pipeline and three object
   writers. Each backend's `codegen.c` is 5,000-7,500 lines. A change to
   one pass rebuilds and re-reads all of them.
3. **Products and test equipment share `tools/`.** The 14 programs a user
   runs (`embdbg`, `embsim`, `embflash`...) sit beside 25 encoding
   referees (`thumbcheck`, `rvasmcheck`...) that only the test suite
   uses. Each tool also carries its own ELF, DWARF and SVD reading.

## Principles

- **Embedded first, OS still first-class.** The documentation, the
  examples and the defaults speak to firmware engineers. EmbLinkOS, the
  x86-64 and AArch64 kernels, and hosted Linux and macOS stay supported
  and tested as they are now.
- **Nothing breaks along the way.**
  - `make embcc` keeps producing `./embcc` until `make install`
    replaces it, and every existing make target keeps working.
  - The binaries' names and options do not change.
  - The install layout does not change, and `~/EmbLinkOs`, which builds
    with `~/EmbCC/embcc`, keeps working at every commit.
  - Every structural step is checked by byte-identity: the objects
    from `tests/exec` on every target, before and after, must be
    identical.
- **One source of truth per fact.** A target, a board, a register file
  and a cycle cost are each described once and read by every program
  that needs them.
- **Same discipline as now.** Correctness first, a referee for every
  claim, a mutant to prove every test, and no new dependencies, so a
  C99 compiler and `make` still build everything.

## The repository after the move

```text
README.md            the toolchain, embedded first: what it is, the
                     targets, a two-minute firmware example
compiler/            embcc
  driver/            options, target selection, pipeline (main.c split)
  frontend/          lex, cpp, parse, sema, cxx
  ir/                EmbIR, irgen
  opt/               the pass manager and one file per pass (opt.c split)
  backends/<arch>/   isel, frame, emit, asm (each codegen.c split)
  objwriter/         elf, macho, coff, embx, dwarf, eh
targets/             THE TARGET DATABASE (data, one file per family):
                     triples, data model, ABI facts, predefined macros,
                     CPUs and features, boards (memory map, UART,
                     QEMU machine), default link layout
libemb/              libraries every program links: elf read/write,
                     dwarf read, svd, symbolize, target db access, cost
                     model, platform layer, diagnostics
assembler/           gas front end (src/as) + embas
linker/              embld (src/link + tools/embld)
runtime/             librt, libc, libc++, headers (lib/, include/)
sim/                 embsim: core/, cpu/<arch>/, periph/, boards from
                     targets/, gdb/
tools/               the user's tools: embdbg, embflash, embtrace,
                     embmap, embpack, embrt, embsvd, embread, embls,
                     embidx, embar
verify/              test equipment: the *check referees, bench, the
                     fuzzers (gen2/gen3/gen4, today only in a scratch
                     directory) and the ABI fuzzer
tests/               unchanged in shape: exec, cxx, golden, harness
docs/                the handbook (below)
```

Notes on the layout:
- **The target database is the core change.** Today `target.c`,
  `predef.c` with 42 generated tables, the driver's lists, the linker's
  machine checks, EmbSim's board table and the harness each restate
  facts about a target. Moving these facts into `targets/`:
  - makes a new target mostly a new data file plus its backend;
  - makes target branches stop conflicting in shared lists;
  - lets EmbSim, EmbFlash and the linker read the same board definitions
    the compiler uses.
- **`libemb/`** removes the copies: embdbg, embsim, embtrace, embmap and
  embflash each read ELF or DWARF today, and embsvd's parser is what
  EmbSim's peripheral models need.
- **`verify/`** brings the fuzzers and the ABI fuzzer into the
  repository. Today they live only in a scratch directory, and that
  directory has been wiped once already.

## Code architecture changes

In order of value:

1. **The target database** (`targets/`, read through `libemb`). It
   holds what is data today: triples and aliases, the data model (sizes,
   alignments, signedness), ABI facts (argument registers, stack
   alignment, struct return), predefined macros, CPU and feature lists
   (`-mcpu`, `-march`), and the boards. The backends keep the code that
   is genuinely per-architecture. Measure of success: adding a target
   touches the database, its backend directory, and nothing else in
   the compiler.
2. **The optimizer as passes.** A small pass manager with one file per
   pass. Each pass has its documented contract, its `-f` switch and its
   remarks. `opt.c` becomes about a dozen files. Byte-identity
   guarantees the split changes nothing. *(Done: 40 files, see
   [The optimizer](optimizer.md#the-files).)*
3. **The backend interface.** The parts every backend repeats move into
   shared code with per-target hooks:
   - argument placement (`place_arg`), parallel moves, the frame layout;
   - asm operand binding (the dead-output fix had to be made five times);
   - the call sequence.
   Each `codegen.c` is split into instruction selection, frame and
   prologue, and emission.
4. **The driver.** An option table instead of a 2,000-line if-chain, so
   `--help`, the docs and the GCC-flag compatibility list come from one
   place. The object writers move to `objwriter/`.
5. **`libemb` for the tools.** One ELF reader, one DWARF reader with
   line tables, variables and CFI, one SVD model and one symbolizer.
   EmbSim's coverage and profiling and EmbTrace's symbols build on them.

Each step lands on its own, with byte-identity proven, and none blocks
feature work for more than a day.

## Documentation and branding

- **Name.** EmbCC stays the compiler's name. The suite is "the Emb
  toolchain" (compiler, assembler, linker, debugger, simulator, flash,
  trace, map, pack, rt, svd). If you want a different product name, this
  is the moment to choose it; nothing else in the plan depends on it.
- **The README** leads with what firmware engineers need:
  - the targets as families (Cortex-M, RISC-V, AVR, Xtensa/ESP32,
    TriCore, RX, ColdFire, MIPS, PowerPC, SPARC/LEON, LoongArch);
  - building and installing;
  - a firmware example that compiles, links with `embld`, and runs in
    `embsim` without a board.
  The OS and hosted targets get their own section.
- **The handbook**, in `docs/`:
  - **Getting started:** install, then a first firmware run in EmbSim,
    then on a board, written per family (an STM32/Cortex-M, an ESP32,
    an RP2040 or a RISC-V MCU, an AVR).
  - **User guide:** the language (`c-language`, `cxx`, `extensions`,
    `inline-asm`), embedded topics (startup, linker scripts, interrupts,
    RTOS ports, CMSIS, code size, timing), and debugging, simulating,
    tracing and flashing.
  - **Tool references:** one page per tool, as now.
  - **Migration from GCC and clang:** flags, attributes, the
    differences, and how to switch an existing Makefile or CMake
    project.
  - **Internals:** the current `internals/` pages, rewritten around the
    new layout. The `*-plan.md` porting diaries become history.
  - **The scoreboard:** size and speed against GCC and clang, published.
- **Positioning:** a C99 toolchain with no dependencies; every target in
  one binary; every claim checked against GCC, clang, GNU as, llvm-mc
  or QEMU; and a simulator and tools that know the compiler's output.

## EmbSim, the simulator

EmbSim stays in this repository, in `sim/`, because it shares the target
database, `libemb`, the cost model and the test harness with the
compiler. Its roadmap:
1. **Architecture and a GDB server.** In progress now on branch
   `embsim-gdb`: modules (core, bus, peripherals, boards as data), and
   the GDB remote protocol, checked against QEMU's gdbstub with the
   host's gdb, lldb and embdbg.
2. **More cores:** RISC-V RV32 and RV64 with M, A, C, F and D, AVR, then
   the other targets EmbCC compiles for. QEMU referees each one.
3. **Peripherals from SVD:**
   - every register of a vendor SVD file exists and is logged;
   - behavioural models for GPIO, timers, UART input and output, SPI
     and I2C with attachable devices, ADC and DMA;
   - board files in `targets/`.
4. **Testing and analysis:**
   - scripted scenarios for CI (inject input at a time, expect output,
     JUnit reports);
   - code coverage by source line, from DWARF;
   - per-function profiles, written as Perfetto traces through
     EmbTrace's format;
   - stack high-water marks;
   - fault diagnosis that names the source line and the cause;
   - record and replay, and reverse stepping from snapshots.

## Order of work

1. **Now (no conflicts with anything):**
   - the README and the handbook's structure (branding);
   - EmbSim phase 1 (agent);
   - bringing the fuzzers into `verify/`.
2. **When no branch is open (after #116 and `embsim-gdb` merge):** the
   directory move, as one mechanical commit of `git mv` plus Makefile
   paths, proven by byte-identity and the full matrix.
3. **Then, one at a time, each with byte-identity:**
   - the target database;
   - `libemb`;
   - the optimizer split;
   - the driver's option table;
   - the backend split.
4. **Throughout:** EmbSim phases 2-4, and compiler feature work
   (`_Float16`, `asm goto`, C++ exceptions on embedded targets).

## Decisions (2026-10-08)

1. **Layout:** approved as proposed. The move happens once #116 and
   `embsim-gdb` have merged.
2. **Name:** EmbCC stays the compiler's name, and the suite is "the Emb
   toolchain". The binaries keep their `emb*` names.
3. **Default target:** the host, as with gcc and clang. Plain `embcc
   file.c` will build for the machine it runs on, and cross-compiling
   names `--target`. This lands with the test suite's update, and only
   after EmbLinkOS stops relying on the old default. EmbLinkOS's
   Makefile calls `$(HOST_EMBCC) -c ...` with no `--target`, so it
   needs `--target=x86_64-elf` there, or `EMBCC_DEFAULT_TARGET=x86_64-elf`
   in its environment, which EmbCC already honours. That one-line change
   is EmbLinkOS's, made on its side.
