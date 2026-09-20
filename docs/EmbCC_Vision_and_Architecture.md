# EmbCC — Compiler Platform Vision & Architecture Specification

**Status:** Foundational design document — v0.2 (draft)
**Project:** EmbLink / EmbCC
**Languages:** C (primary), C++ (staged, see §12)
**First targets:** `x86_64-emblink` (EMBX), `x86_64-linux-gnu` (ELF, test target)
**Hosts:** macOS, Linux; Windows later; EmbLinkOS (self-hosting)
**Purpose:** Define what EmbCC is, what it is not, the rules it never breaks, its architecture, and the order in which it gets built.

### How to read this document

| Part | Content | Normative? |
|---|---|---|
| I | Vision, non-goals, invariants | Invariants: yes |
| II | Current state and baseline decisions | Yes |
| III | Core architecture | Yes (MUST / SHOULD) |
| IV | Platform capabilities (the differentiators) | Direction, not contract |
| V | Roadmap, testing, documentation, open questions | Yes for exit criteria |

"MUST" means a design that violates it is rejected. "SHOULD" means deviation requires an ADR.

### Changelog

| Version | Change |
|---|---|
| 0.1 | Initial vision |
| 0.2 | Added current-state baseline, non-goals, invariants on determinism and provenance, host/target/format matrix, ABI & runtime, backend internals, preprocessor-configuration problem, staged C++, milestone exit criteria, open questions. Reordered roadmap (IR and self-hosting moved earlier). Merged duplicated sections. |
| 0.3 | **Reconciled with the code (2026-09-20).** §4's table filled in and ADR-0000 resolved (evolve). Added §4.2, an inventory of what has been built against this document's own invariants and milestones, and §4.3, what has not. Target and host matrices updated (aarch64 and EmbLinkOS self-hosting are done, not "later"). §12.2 marked against reality: C++ went past C++2 into parts of C++3, which contradicts Non-goal 2 and open question 6 — both flagged for amendment rather than quietly dropped. §29 mapped onto `docs/ROADMAP.md`'s separate milestone track. No invariant or MUST was weakened. |

---

# PART I — VISION

# 1. Identity

EmbCC begins as a correct, self-hosting C compiler for EmbLinkOS. Its long-term identity is broader:

> **EmbCC is a program understanding, analysis, transformation, compilation, and explanation platform. The compiler is one consumer of a shared semantic foundation.**

A traditional compiler turns source into machine code and discards everything it learned along the way. EmbCC keeps that knowledge and makes it queryable, so that it can eventually answer:

- What does this program mean? Which source construct produced this instruction?
- Why did this warning occur? Why was this file rebuilt?
- Why was this function not inlined? Why was this loop not vectorized?
- What changed between these two binaries — in ABI, size, stack, and optimization decisions?
- What is the worst-case stack usage? Does this program fit the target's RAM/flash budget?
- What are the project-wide consequences of changing this type?

The differentiator is not any single feature. It is that **the compiler records its knowledge and decisions at the moment it has them**, instead of tools re-deriving (or guessing) them later.

# 2. Non-goals

Stating what EmbCC will not do is what keeps the vision achievable.

1. **Not a drop-in replacement for GCC/Clang on all platforms.** EmbCC does not aim to produce Mach-O or PE/COFF binaries for macOS/Windows until an ADR says otherwise. Running *on* those hosts is a goal; *targeting* them is not.
2. **~~No full modern C++ in the foreseeable plan.~~** *Superseded in v0.3 — see §12.2.* C++ is still delivered in explicit subsets (§12), but the target is C++20 with libstdc++ on both architectures (D-013), and C++0 through C++2 plus much of C++3 are built. The non-goal that survives is narrower: **modules are not scheduled**, and conformance is measured against g++ agreement on a test corpus, not claimed in the abstract.
3. **No soundness claims from the security analyzer** unless an analysis is formally sound for a stated subset. Findings are evidence-graded (§23), not proofs.
4. **`embcc` does not build projects.** Project orchestration belongs to EmbBuild. `embcc` compiles translation units and answers questions (§17).
5. **No plugin ABI before the internal APIs have stabilized** (§26).
6. **No feature ships without its test gate** (§30). A feature that cannot be tested is not done.

# 3. Invariants (the rules we never break)

**R1 — Don't duplicate compiler knowledge.**
If the frontend knows something, the language server, analyzers, refactoring engine, and build system reuse it. There is exactly one C/C++ parser and one semantic model.

**R2 — Record decisions where they are made.**
Every pass that makes a decision (inline / not inline, vectorize / not, spill, fold, reject) emits a structured *remark* at that point. "Why?" features are queries over recorded remarks and dependency data — never a separate engine reconstructing reasons after the fact. This cannot be retrofitted; it is a day-one API requirement for every pass.

**R3 — Preserve provenance end to end.**
Every AST node, IR instruction, machine instruction, and emitted byte carries a source location, including the macro-expansion chain. A transformation that drops provenance is a bug, and the IR verifier checks for it.

**R4 — Output is deterministic.**
Same inputs, same compiler, same options ⇒ bit-identical output, on any host. No timestamps, no host paths (unless mapped), no iteration over pointer-keyed or hash-ordered containers where order affects output. Enforced by the bootstrap comparison test (§30).

**R5 — Don't couple the compiler to EmbLinkOS.**
EmbLinkOS is a target and eventually a host. Core code (frontend, semantic model, IR, optimizer, backends) MUST NOT include EmbLinkOS headers or depend on EMBKFS or EmbLinkOS APIs. Everything host-specific lives behind the platform layer (§16).

**R6 — Stages are libraries with contracts.**
Every stage has a documented API, invariants, and a textual dump format. No stage reaches into another stage's internals. The `embcc` executable is a thin driver over these libraries.

---

# PART II — CURRENT STATE AND BASELINE

# 4. Where EmbCC is today

EmbCC is not a greenfield project. EmbLinkOS already has a working toolchain: **EmbCC, EmbLD, EmbBuild, emlibc (including fdlibm), EmbDBG (two milestones), and the EMBX executable format.** This document therefore describes an *evolution*, not a from-scratch design.

This table gated Part III. It is now answered from the code (v0.3); each row
names where to verify it.

| Question | Answer |
|---|---|
| Implementation language of current EmbCC | **C99** — `CFLAGS = -std=c99 -Wall -Wextra -Werror`. No C++ in the compiler itself, which is what makes self-hosting reachable with a C frontend alone (§28.2). |
| C standard level and extensions currently accepted | **C11 in full** on both targets, plus the GNU extensions EmbLinkOS needs: extended `asm`, `__attribute__`, `__builtin_*`, `__atomic_*`, `typeof`, `__int128`, statement expressions, computed `goto`. Per-clause detail: `docs/COMPATIBILITY.md`. C17/C23 are additive from here, not a rewrite. |
| Architecture: single-pass or separate AST / IR stages? | **Separate stages**, already: `src/lex` → `src/cpp` → `src/parse` (AST) → `src/sema` (semantic model) → `src/ir` (EmbIR) → `src/opt` → `src/arch/<arch>` → `src/elf`. This is why ADR-0000 resolves to *evolve* (§4.1). |
| Output: assembly text + external assembler, or direct object emission? | **Direct object emission.** The backend writes ELF objects itself; `src/arch/x86_64/as.c` is additionally a standalone assembler (`embas`) that assembles the kernel's hand-written NASM byte-identically to nasm. `-S` (assembly text) is **not** implemented and is a known gap (§4.3). |
| Object/executable formats produced | **ELF64** relocatable and executable, x86-64 and AArch64; **EMBX** via EmbLD (`embld --embx`), byte-identical to the reference producer. |
| Calling convention and data layout | **SysV AMD64** on x86-64, **AAPCS64** on AArch64 — adopted as-is, no deviation. Data layout LP64 on both; `long double` is x87 80-bit on x86-64 and IEEE binary128 on AArch64; plain `char` is signed on x86-64, unsigned on AArch64. Full matrix: `docs/TARGET_ABI.md`, `docs/COMPATIBILITY.md`. This answers open question 2. |
| Debug info emitted, and what EmbDBG consumes | **DWARF-4** line, frame and local information under `-g`. **EmbDBG** (`tools/embdbg/`) reads it back: symbolize, backtrace, disassemble, inspect locals, analyse a kernel crash dump, a TUI — no gdb in the loop. Aggregate type DIEs are the remaining producer gap. This answers open question 3. |
| Which parts of EmbLinkOS / emlibc it compiles today | **All of both.** The kernel: 89 C units via `embcc`, 6 `.asm` via `embas`, linked by `embld` — no gcc, no nasm, no `ld` — booting to the desktop behaviourally identical to the gcc build, at `-O0`, `-O1` and `-O2`. **emlibc**, including real `fdlibm` floating point (38 units), compiled on the OS and self-hosting. Plus **libstdc++** (193/193 objects on both targets). |
| Status of the KM1 self-build milestone | **Host half done.** `tools/gen-kernel-manifest.sh` generates a 96-target EmbBuild manifest and `tests/golden/x86_64/embbuild-kernel.sh` (opt-in, `EMBCC_KM1=1`) walks it to a higher-half `kernel.elf` that boots in QEMU. KM2/KM3 stay OS-side. |
| Existing test suite and pass rate | 65 golden scripts plus per-target execution corpora: **195/195 on x86_64, 170/170 on AArch64**, run by `make test` / `make test-arm64`. Both are release gates; `EMBCC_VERIFY=1` runs the IR verifier after every optimizing compile for the whole suite. |
| Which hosts it currently runs on | **macOS arm64** (primary development), **Linux x86_64** (CI/test), and **EmbLinkOS x86_64** — where it compiles and links its own sixteen sources into a byte-identical `embcc`. |

## 4.1 Evolve or rebuild — ADR-0000

This is the first decision and it gates everything else.

- **If the current EmbCC already separates parsing, semantic analysis, and code generation**, evolve it in place: extract the frontend into a library, introduce EmbIR between semantics and codegen, and refactor toward Part III.
- **If it is single-pass or fuses parsing with code generation**, keep it as the **stage-0 bootstrap compiler** and build the new architecture alongside it. The old compiler remains the known-good reference until the new one passes the M3 gate (§29). It is never deleted before that.

Either way, the existing compiler is an asset: it is the first differential-testing oracle on EmbLinkOS and the bootstrap path.

**Resolved (v0.3): evolve.** EmbCC already separates parsing, semantic
analysis, IR generation and code generation (§4), so the first branch applies.
There is no stage-0 compiler and none is needed: EmbCC is its own bootstrap
oracle, and the fixed point — the compiler compiling its own sixteen sources
to byte-identical objects, on EmbLinkOS — is a standing test rather than a
milestone that was passed once.

## 4.2 What has since been built

Measured against this document's own invariants and milestones, not against a
wish list. Every claim here has a test named for it.

**Invariants.**

| | State |
|---|---|
| **R1** don't duplicate compiler knowledge | **Held.** `embls`, the language server, does not re-implement anything: it forks a child that runs EmbCC's own preprocessor and parser (C, or the C++ front end by suffix) and indexes what they built. Its diagnostics are `embcc -fsyntax-only -fdiagnostics-format=json`. There is one parser. |
| **R2** record decisions where they are made | **Started (v0.3).** `src/driver/remark.c` is the API and the inliner is the first producer: every decision it makes, taken or refused, is recorded with a stable reason code and the fact that settled it. `-fremarks[=json]` prints them; `embcc why <decision> [subject]` queries them (§19). Every OTHER pass — SCCP, CSE, DCE, mem2reg, the register allocator — still records nothing, so the invariant is not yet held. |
| **R3** preserve provenance | **Partly.** Every AST node carries line and column; every IR instruction carries a line; DWARF line, frame and local info is emitted and read back by EmbDBG. But IR instructions carry no *column*, `struct cexpr` (C++) carries a line and no column at all, and nothing verifies that a pass preserved a location. The verifier does not yet reject an instruction without one. |
| **R4** deterministic output | **Held, and it is the strongest test in the project.** The self-host fixed point is byte-identical objects across host and OS, sixteen sources, checked every release. |
| **R5** don't couple the compiler to EmbLinkOS | **Held.** Nothing in `src/` includes an EmbLinkOS header. |
| **R6** stages are libraries with contracts | **Held structurally** — `embld`, `embas`, `embdbg`, `embls` and `embcc` are thin drivers over the same libraries, which is why the language server can exist at all. **Not held for dump formats**: there is no `embcc inspect`, and no stage has a textual round-trip form (§4.3). |

**Beyond what the roadmap named.** Several things exist that §29 never
scheduled, because the OS needed them:

- **A second architecture, early.** AArch64 is not "later" (§5.2): it is a
  first-class target with its own backend, its own ABI (AAPCS64) and its own
  full test run. §7 of the roadmap (M7) is effectively met, and it was met the
  way the document predicted — a new target description and selection
  patterns, no optimizer changes.
- **An optimizer.** `-O2` carries SSA `mem2reg` (dominance frontiers, phi
  insertion, renaming, out-of-SSA), inlining, SCCP, dominator-scoped global
  CSE, redundant-load elimination and a Chaitin–Briggs register allocator.
  Three silent miscompiles were found on the way; the one the suite could not
  see is why `EMBCC_VERIFY=1` now runs an IR verifier after every optimizing
  compile.
- **The whole toolchain, with no external parts.** `embas` assembles the
  kernel's NASM byte-identically to nasm; `embld` links, and emits EMBX
  directly; `embdbg` reads the DWARF back. No gcc, no nasm, no `ld`, no gdb.
- **C++ far past what §12.2 scheduled.** See §12.2.
- **A diagnostics and tooling layer** (`docs/TOOLING.md`): structured
  diagnostics with stable IDs, `--explain`, fix-its that `--fix` applies,
  error recovery in both front ends, warning groups over real analyses
  including a dataflow `-Wuninitialized`, a language server, and
  `embld --doctor` for why a link failed. This is §13 and much of §24,
  arrived at from the bottom up rather than from this document.

## 4.3 What has not been built

Named so the gap is a decision rather than a surprise. In rough order of what
this document depends on most:

1. **Remarks (R2, §13, ADR-0004) — the API exists; most passes do not use
   it.** The store, the two output formats and `embcc why` are built, and the
   inliner is wired to them. The document's warning about retrofitting proved
   exact, and the shape of the cost is now measured rather than guessed:
   `inlinable()` returned 0 from **a dozen places, each meaning something
   different**, and by the time anyone asked, the dozen answers had collapsed
   into one. Adding the API took an afternoon; giving that ONE function its
   reasons meant going back through every branch and naming it. SCCP, CSE,
   DCE, mem2reg and the register allocator are still silent, and each will
   cost the same kind of pass. Until they are done, `embcc why` answers only
   about inlining, and performance provenance (§25) and half of §22 remain
   blocked.
2. **Stage dump formats (§18, R6) — started.** `embcc inspect ir` and
   `embcc inspect pp` exist (`src/ir/irprint.c`), and the IR's textual form
   is what makes a pass's effect visible: the same command at `-O0` and
   `-O2` shows mem2reg removing stack traffic, the inliner pulling a callee
   in, and immediate folding. The remaining stages (`tokens`, `ast`,
   `symbols`, `types`, `mir`, `cfg`, `callgraph`) and `-S` are not done.

   **The round-trip half of §9.1 is a structural change, not a printer
   feature, and this is where that was discovered.** `struct ir_ins` points
   at `struct func`, `struct global` and `struct type` in seven places
   (`callee`, `glob`, `argv[].ty`, `rety`, `dbgvar.ty`, `src`, `eh_types`).
   EmbIR is therefore not a self-contained module — it is a view over the
   AST. Printing follows those pointers and writes a name; *parsing* would
   have to rebuild them, which means interning names and types into the
   `ir_unit`. Until that is done there is no print→parse→identical test and
   no pass is testable text-in/text-out (§9.1, §30). It is the same shape of
   debt as the missing remarks: a day-one property of the IR that was not
   built in.
3. **The project knowledge graph (§8.2).** No USRs, no interface hashes, no
   cross-TU index. Incremental compilation is at Level 1 (`-MD` file
   dependencies) and Levels 2–3 (§21), `embcc diff` (§22) and project-wide
   refactoring (§24) all wait on it.
4. **`SourceProvider` (§7, §16).** The frontend reads files directly; the
   language server works around it by writing the editor's buffer to a
   temporary file. Cancellation and incrementality (§7) are likewise absent —
   `embls` reparses the whole unit on every keystroke.
5. **Configuration keying (§7.1, ADR-0009).** Semantic facts are keyed by
   file, not by (file, configuration). Nothing reports which configuration a
   claim was verified under.
6. **The security analyzer (§23).** The uninitialized-read analysis exists as
   a warning; there is no evidence grading, no reasoning trace, no labeled
   corpus and no false-positive gate.
7. **Resource intelligence (§20).** No memory report, no worst-case stack
   analysis, no budgets, no target profiles — although frame sizes and a call
   graph, which §20.2 needs, are already computed.
8. **SARIF output (§13)** and the tool-mode subcommands of §17 generally:
   `check`, `inspect`, `why`, `report`, `diff`, `verify-build`. Driver mode is
   GCC-compatible and well past the §17 example; tool mode barely exists.

# 5. Host, target, and output formats

## 5.1 Definitions

- **Host:** where EmbCC executes.
- **Target:** what EmbCC generates code for, identified by a triple `arch-os[-env]`.
- **Build (compiler):** the machine that built EmbCC itself. Relevant for bootstrap (§29, M3).

Host and target are independent everywhere in the code. No `#ifdef __APPLE__` in any stage above the platform layer; no host type (`long`, `size_t`) used to represent a target quantity.

## 5.2 Target matrix

| Target triple | Format | Role | Status |
|---|---|---|---|
| `x86_64-elf` / `x86_64-emblink` | ELF, EMBX | Primary product target | **Done.** Builds and boots the EmbLinkOS kernel; EMBX emitted by EmbLD |
| `x86_64-linux-gnu` | ELF | Test target: conformance and differential suites against GCC | **Done.** Every test runs against `x86_64-elf-gcc` 16.2 under QEMU (natively where the host is x86-64 Linux) |
| `aarch64-elf` / `aarch64-emblink` | ELF, EMBX | Second architecture | **Done, early.** Own backend and AAPCS64; 170/170 its own suite; referee `aarch64-elf-gcc` 16.2 under QEMU `virt` |
| `thumbv7em-none-eabi` / `riscv32-none-elf` | ELF | Bare-metal embedded (where memory/stack budgets matter most) | Later. Note both are 32-bit: the backends assume LP64 today |
| `*-darwin`, `*-windows` | Mach-O, COFF | Not planned (see Non-goals) | — |

The Linux ELF test target is deliberate: it lets thousands of test programs be compiled by EmbCC and GCC, run, and compared on a normal CI machine. Without it, differential testing depends on booting EmbLinkOS.

## 5.3 Host matrix

| Host | Role | Status |
|---|---|---|
| macOS arm64 | Primary development host | **Done** |
| Linux x86_64 | CI and test host | **Done** |
| Windows x86_64 | Portability check | Later |
| EmbLinkOS x86_64 | Self-hosting | **Done.** Compiles and links its own sixteen sources on the metal, byte-identical to the host build |

---

# PART III — CORE ARCHITECTURE

# 6. Pipeline

The pipeline follows the C translation phases: tokenization precedes preprocessing.

```text
Source bytes (via SourceProvider)
     │
     ▼
Lexer ─────────────── pp-tokens with locations
     │
     ▼
Preprocessor ──────── expansion records (macro provenance)
     │
     ▼
Parser ────────────── AST (with error recovery)
     │
     ▼
Semantic analysis ─── name resolution, types, constant evaluation,
     │                implicit conversions, layout
     ▼
Semantic model  ◄──── shared with EmbCC-LS, analyzers, refactoring
     │
     ▼
IR generation
     │
     ▼
EmbIR (SSA) ───────── verifier · textual form · remarks
     │
     ├── Analyses (CFG, dominators, alias, data-flow, call graph)
     ├── Target-independent optimization
     │
     ▼
Instruction selection
     │
     ▼
EmbMIR (machine IR, virtual registers)
     │
     ├── Register allocation
     ├── Frame lowering / prologue-epilogue
     ├── Machine-level optimization, scheduling
     │
     ▼
Emission ──────────── object file (EMBX / ELF) + debug info
     │
     ▼
EmbLD ─────────────── executable / library + link map + resource report
```

Each arrow is a library boundary with a dump format (`embcc inspect <stage>`, §18).

# 7. Frontend as a library

The frontend (lexer, preprocessor, parser, semantic analysis) is a library consumed by `embcc`, EmbCC-LS, analyzers, the refactoring engine, documentation and indexing tools. The compiler executable does not own it.

Serving an editor imposes requirements a batch compiler does not have. They MUST be designed in from the start:

- **Error recovery.** The parser produces a usable partial AST from broken code. One missing `;` must not discard the rest of the file.
- **Unsaved buffers.** Source is read through a `SourceProvider` interface, not the filesystem directly, so the language server can supply in-memory editor contents. (This, not OS portability, is the main reason for the file abstraction.)
- **Cancellation.** Long operations can be abandoned when the user types again.
- **Incrementality.** Reparsing/re-analysing a changed function without redoing the whole translation unit is a SHOULD for the first LS release, a MUST later.
- **Location fidelity.** Every location resolves to (spelling location, expansion location, include stack).

## 7.1 The preprocessor-configuration problem

A C file does not have one meaning. It has one meaning *per configuration* (`-D` flags, target, include paths). `#ifdef CONFIG_SMP` may remove half a file. Consequences:

- The semantic model and project graph are keyed by **(file, configuration)**, not by file alone.
- Each EmbBuild target defines a configuration; the language server picks one active configuration per file and says which.
- Code inside inactive `#if` regions is tokenized and indexed shallowly (for navigation and rename safety) but not semantically analysed.
- Refactoring operations report which configurations they checked. A rename verified only under one configuration says so.

Every "project-wide" claim EmbCC makes carries the set of configurations it was verified against.

# 8. Semantic model and project graph

## 8.1 Per-translation-unit semantic model

Produced by semantic analysis: declarations and definitions, scopes, types and their layouts, expressions with resolved types and conversions, constant-evaluation results, overload resolutions (C++), references, and source locations.

## 8.2 Project knowledge graph

A persistent, cross-TU index built from the per-TU models:

```text
Project
 ├── Configurations (from EmbBuild targets)
 ├── Files / headers / include edges
 ├── Declarations (keyed by stable USR — unified symbol reference)
 │    ├── Types: fields, layout, ABI hash, relationships
 │    ├── Functions: signature, callers, callees, frame size, effects
 │    └── Globals: readers, writers, section, size
 ├── Build targets and artifacts
 └── Build history (§22)
```

Design requirements:

- **Stable symbol identity.** Every entity has a USR that survives unrelated edits. Without this, incremental compilation, build history, and cross-build comparison cannot work.
- **Semantic hashes.** Each declaration has an interface hash (what dependents observe: signature, layout, `inline` body if relevant) separate from an implementation hash. Incremental compilation compares interface hashes (§21).
- **Explicit invalidation.** Every graph fact records what it was derived from, so an edit invalidates exactly the dependent facts.
- **Storage format** is an open decision (§32). It MUST be versioned and MUST be safely discardable (always rebuildable from sources).

Consumers: incremental builds, EmbCC-LS, navigation, refactoring, analyzers, "why" queries, documentation, architecture visualization, debugger.

# 9. EmbIR

EmbIR is the boundary between language semantics and machine code. Everything after it is language-independent.

## 9.1 Required properties (v1)

- **SSA form over a control-flow graph**, typed, with explicit memory operations.
- **Target-parameterized data layout** (pointer size, alignment, endianness) supplied by the target description, never by the host.
- **Every instruction carries a debug location** (R3). The verifier rejects instructions without one, except where explicitly marked compiler-synthesized.
- **Metadata that analyses need and the frontend knows:** type-based alias information, `restrict`, `volatile`, known object sizes and bounds, lifetime start/end markers, `noreturn`, purity. Information the frontend knows and IR drops is information the "why" and security features can never use.
- **Round-trip textual form** (print → parse → identical IR). This makes every pass testable in isolation with text-in/text-out tests.
- **Verifier** run between passes in debug builds and in CI.
- **Remark emission API** (R2) available to every pass.

## 9.2 Levels

One IR is not enough for everything this document wants:

| Level | Purpose |
|---|---|
| Typed AST + semantic model | Source-level analyses: lifetimes, bounds, API misuse, refactoring |
| **EmbIR** (SSA) | Target-independent optimization, data-flow, alias analysis |
| **EmbMIR** (machine IR) | Instruction selection output, register allocation, frame layout, scheduling |

Security and lifetime analyses that need source-level structure run on the semantic model or on EmbIR with preserved metadata — not on EmbMIR.

## 9.3 External use

A documented EmbIR (textual form + specification) enables `embcc → EmbIR → external tool → EmbIR → backend`. This is a later capability; the textual form required in §9.1 is what makes it possible.

# 10. Backend

The backend is described explicitly because it is where most of the engineering effort after the frontend goes.

- **Target description**: registers, register classes, calling conventions, data layout, instruction set, feature flags. One per architecture; shared by codegen, the assembler, and the debugger.
- **Instruction selection**: start with a simple tree/DAG pattern matcher per target. Correctness first.
- **Register allocation**: linear scan first (fast, simple, adequate); a graph-coloring or greedy allocator is a later optimization, not a prerequisite.
- **Frame lowering**: stack layout, callee-saved registers, prologue/epilogue, stack-size recording per function (feeds §20).
- **Inline assembly**: GCC-style extended `asm` with constraints and clobbers. Mandatory — the EmbLinkOS kernel cannot be built without it.
- **Integrated assembler and object writer**: emit EMBX and ELF directly; textual assembly output remains available for inspection.
- **Debug info emission**: DWARF (§14), unless EmbDBG's needs justify a custom format via ADR.

Target-independent optimization happens on EmbIR; the backend interface is designed so that adding AArch64 requires a new target description and selection patterns, not changes to the optimizer.

# 11. ABI and runtime

A compiler that generates correct code but disagrees with the linker, libc, or debugger about the ABI is not correct. Each target MUST document:

- **Calling convention** (for x86_64: SysV AMD64 unless an ADR says otherwise — see §4), including variadic functions and struct passing/return.
- **Data layout**: type sizes and alignments, struct layout, **bit-field layout**, `_Bool`, `long double`, enum underlying types.
- **TLS model**, stack alignment, red zone (disabled for kernel code), code models.
- **Symbol naming** and section conventions.

The compiler also owns runtime pieces that are not libc:

- **Freestanding headers** shipped with the compiler: `stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`, `stdalign.h`, `stdnoreturn.h`, `float.h`, `limits.h`, `iso646.h`.
- **Builtins runtime (`emrt`)**: helper routines the compiler emits calls to (128-bit arithmetic, soft-float on FPU-less targets, overflow-checking helpers). Note that the compiler may emit calls to `memcpy`/`memset`/`memmove`/`memcmp`; freestanding environments must provide them.
- **Startup objects** (crt0 equivalents) per target, in coordination with emlibc and EmbLD.
- **C++ ABI (later):** Itanium C++ ABI — name mangling, vtable layout, RTTI layout, and exception unwinding (`.eh_frame` + unwinder). Exceptions are a runtime and ABI project, not just a frontend feature.

ABI conformance is tested by cross-compiler tests: a caller compiled by EmbCC and a callee compiled by GCC (and vice versa) on the Linux test target, across a generated set of signatures.

# 12. Language support strategy

## 12.1 C

- **Baseline:** C17, with C23 features added individually by demand.
- **Extensions required to build EmbLinkOS** (the real conformance target): extended inline `asm`; `__attribute__` (`packed`, `aligned`, `section`, `noreturn`, `unused`, `used`, `weak`, `always_inline`, `noinline`, `interrupt` as needed); `__builtin_*` (`expect`, `unreachable`, `offsetof`, `va_*`, `clz/ctz/popcount`, `bswap`, overflow builtins); `__atomic_*` / `_Atomic`; `_Static_assert`; `typeof`. The exact list is derived from the EmbLinkOS/emlibc sources, not from GCC's full extension set.
- Compatibility is documented per standard clause and per extension, with the test that proves it.

## 12.2 C++ — staged subsets

Full C++ is the single largest cost in this document. It is delivered in subsets, each independently useful, each with its own gate:

| Level | Contents | Typical use | State (v0.3) |
|---|---|---|---|
| C++0 | Classes, member functions, constructors/destructors, references, namespaces, overloading, `constexpr` basics; **no exceptions, no RTTI** | Embedded-style C++ (common `-fno-exceptions -fno-rtti` practice) | **Done** (CX1–CX3) |
| C++1 | Templates (class/function), basic template deduction, `auto`, range-for, lambdas | Generic containers and algorithms | **Done** (CX4, CX6) |
| C++2 | Exceptions and RTTI (requires §11 C++ ABI + unwinder) | Standard-library compatibility | **Done** (CX5) — exceptions cross EmbCC/g++ frames |
| C++3 | Concepts, modules, coroutines, full modern conformance | Research-scale; not scheduled | **Partly done** (CX7): concepts and `requires`, `<=>`, `consteval`/`constinit`, structured bindings, `if constexpr`, coroutines. **Modules: not done.** |

**This contradicts the document, and the contradiction is the point.** Non-goal
2 says "no full modern C++ in the foreseeable plan" and open question 6 asks
whether C++ goes beyond C++0 at all. The answer, decided by the project owner
and recorded in `docs/DECISIONS.md` D-013, is **C++20 with libstdc++ on both
architectures** — and it is built: CX8 compiles libstdc++ itself (193/193
objects per target) and CX9 compiles C++ on EmbLinkOS, with EmbLD linking it.

Two things follow that this document must now say plainly:

- **Non-goal 2 and open question 6 are superseded.** They should be amended in
  the next revision rather than left standing as false statements about the
  project's direction.
- **The mechanism that made it affordable was not the one §12.2 assumed.**
  `src/cxx/` is a C++ front end that **lowers to C**, and the C it emits goes
  through the existing C pipeline to either backend. That is why a second
  architecture cost nothing in C++ terms, and why "shared frontend, EmbIR"
  (below) held: the architecture did not have to assume C++ would happen,
  because C++ arrived as a *consumer* of it. Detail: `docs/CXX.md`.

The remaining caution stands unchanged: the architecture (shared frontend,
EmbIR) must not *assume* C++, and does not.

# 13. Diagnostics and remarks

Diagnostics and remarks are data first, text second.

- **Every diagnostic** has a stable ID, severity, primary location, secondary locations (notes), optional fix-its, and the configuration it was produced under.
- **Every remark** (R2) has a pass name, decision, subject (function/loop/call site), reason code, and supporting facts (e.g., "call-site cost 340 > threshold 225").
- **Output formats:** human-readable text (default), JSON, and SARIF for integration with CI and editors.
- **Warnings are opt-in by category,** with stable names. The goal is a small number of high-value diagnostics, not volume.

The "why" commands (§19), the language server, EmbStudio, and build history all consume these records. None of them parse human-readable text.

# 14. Provenance and debug information

Provenance is a chain preserved across every stage (R3):

```text
spelling location ─ macro expansion chain ─ AST node
   → EmbIR instruction → EmbMIR instruction → machine bytes
```

Deliverables:

- DWARF line tables, including inlined-function information.
- Variable locations under optimization (location lists), with explicit "optimized out" ranges and, via remarks, the reason.
- An address → source → IR mapping queryable by EmbDBG and `embcc inspect`.

This is what allows EmbDBG to answer "where did this value come from?" and "why can't I see this variable here?".

# 15. Determinism and reproducible builds

Reproducibility is cheap to build in and very expensive to retrofit. From the first commit (R4):

- No timestamps, random seeds, or process IDs in output.
- No output order derived from pointer values or unordered hash iteration.
- Absolute paths are mapped (`-ffile-prefix-map` equivalent); the default for release builds is a relative/mapped form.
- Parallel compilation produces the same output as serial.
- Symbol and section ordering is a deterministic function of the input.

Optionally, each build produces a **manifest**: compiler version and build hash, configuration, target, options, source and dependency hashes, linker configuration. `embcc verify-build <manifest>` rebuilds and compares.

The strongest determinism test is bootstrap: stage-2 EmbCC and stage-3 EmbCC (§29, M3) MUST be bit-identical.

# 16. Platform layer

All host interaction goes through a single platform layer:

```text
             EmbCC core libraries
                      │
               Platform layer
   (files, directories, processes, threads, time,
    memory mapping, environment, console)
       /        |         |          \
    macOS     Linux    Windows    EmbLinkOS
                                      │
                               VFS → EMBKFS / FAT32
```

- The frontend reads sources only through `SourceProvider` (§7), which the platform layer implements for real files and the language server implements for editor buffers.
- The core does not know which filesystem backs the files, including EMBKFS.
- Porting EmbCC to a new host means implementing this layer and nothing else.

## 16.1 The compiler's own engineering

- Memory: arena allocation per translation unit and per function, interned identifiers and types.
- Concurrency: parallelism across translation units first. Intra-TU parallelism only after the data structures are designed for it; thread-safety assumptions are documented per library (§31).
- Performance is tracked: compile time and peak memory on a fixed corpus are recorded in CI, and regressions are visible.

---

# PART IV — PLATFORM CAPABILITIES

These are the differentiators. They are built on Part III and are only as good as its data. Each capability lists what it depends on.

# 17. Command-line interface

Two modes, clearly separated:

**Driver mode** — GCC-compatible enough that existing makefiles and EmbBuild can use it unchanged:

```text
embcc -c main.c -o main.o -O2 -g -Iinclude -DDEBUG -MD -MF main.d
embcc main.o util.o -o app          # invokes EmbLD
```

Dependency-file output (`-MD/-MF`) is required from the first release.

**Tool mode** — subcommands that answer questions:

```text
embcc check     <files>        # parse + analyse, no codegen
embcc inspect   <stage> <file> # tokens | pp | ast | symbols | types | ir | mir | asm | cfg | callgraph
embcc why       <question>     # recompiled | inline | not-inline | vectorize | warning | size-changed | dependency
embcc report    <kind>         # memory | stack | security | remarks
embcc diff      <build> <build>
embcc verify-build <manifest>
```

Project building (`build`, targets, artifacts) belongs to EmbBuild. If `embcc build` exists, it only delegates.

# 18. Inspectable compilation

Every stage in §6 has a dump format available through `embcc inspect`. Example:

```text
$ embcc inspect summary --function process_packet net/packet.c

Function: process_packet(packet_t *)
Calls:    validate, decrypt, parse, dispatch
Memory:   reads packet->payload; writes packet->status
CFG:      7 blocks, 2 conditional branches, 1 loop
Frame:    176 bytes
Remarks:  decrypt not inlined (cost 340 > 225)
```

Depends on: R6 dump formats, semantic model, EmbIR, remarks.

# 19. The "why" system

A query layer over remarks (§13), the dependency graph (§8), and build history (§22):

```text
$ embcc why recompiled net/network.c

net/network.c recompiled because:
  net/network.h changed
    → struct Packet: field `flags` added (interface hash changed)
      → Packet is used by net/network.c (sizeof, field access)
```

```text
$ embcc why not-vectorized process --loop main.c:143

Loop main.c:143 not vectorized:
  possible aliasing between packet->data and output
  (no restrict, types compatible, no runtime check emitted)
  Suggested action: restrict-qualify one of the pointers, if valid.
```

Depends on: R2 remarks emitted by every pass; stable symbol identity; interface hashes. Answers are only as complete as the remarks the passes record — "why" coverage is tracked per pass.

# 20. Resource intelligence

Particularly important for embedded targets.

## 20.1 Memory report

```text
Flash:  code 412 KB   rodata 91 KB
RAM:    data+bss 47 KB   stack (worst case) 16 KB   heap: not statically known
```

Section sizes are **link-time facts**: EmbLD produces them from the final layout, attributed back to symbols and source files via the project graph.

## 20.2 Worst-case stack analysis

Computed from per-function frame sizes (§10) and the call graph. It is exact only under stated conditions and MUST report when they fail:

- recursion → reported as *unbounded*, with the cycle;
- indirect calls → bounded only if the target set is known (annotations or analysis), otherwise *unknown*;
- VLAs / `alloca` → *unbounded* unless a bound is proven;
- interrupt handlers → added per the target's nesting model.

## 20.3 Budgets

Target profiles may define budgets; exceeding one fails the link:

```text
RAM limit: 256 KB — link failed: RAM usage 271 KB (bss 212 KB; top: net_buffers 64 KB, fs_cache 48 KB)
```

## 20.4 Target profiles (hardware-aware compilation)

A target profile extends the triple with: CPU features (SIMD, FPU), memory regions (flash/RAM ranges, attributes), alignment constraints, and optionally cache and DMA characteristics. Profiles feed codegen feature selection, EmbLD region placement, budgets, and diagnostics. For EmbLinkOS, profiles may later be generated from the OS hardware-description system.

Depends on: frame recording, call graph, EmbLD link map, target profiles.

# 21. Semantic incremental compilation (with EmbBuild)

EmbBuild owns the project graph (targets, sources, configurations, artifacts). EmbCC contributes semantic dependency facts back to it:

```text
          Project graph (EmbBuild)
              ▲            │
   semantic   │            │ configurations,
   deps and   │            │ targets
   hashes     │            ▼
              EmbCC (per TU)
```

- Level 1 (first release): classic file/header dependencies via `-MD`.
- Level 2: interface hashes — a header edit that changes no interface hash used by a TU does not rebuild that TU.
- Level 3: per-function granularity with cached EmbIR/object fragments.

Goal: recompile the smallest semantically affected portion of the project, and be able to explain every rebuild (§19).

Depends on: stable symbol identity, interface hashes, determinism (a cache is only safe if output is a pure function of inputs).

# 22. Build history and binary comparison

Optionally store per-build snapshots (semantic hashes, remarks, section sizes, frame sizes, ABI hashes) and compare builds at any level:

```text
$ embcc diff build-104 build-105 --function process_packet

Source:      changed
ABI:         unchanged
Frame:       1.8 KB → 2.1 KB
Code size:   4.2 KB → 4.9 KB
Inlining:    decrypt() no longer inlined (cost 240 > 225 after edit)
```

Depends on: determinism, stable identity, remarks, storage format (§32).

# 23. Security analysis

Integrated analyses, run by `embcc check` / `embcc report security`: out-of-bounds access, use-after-free, double free, uninitialized reads, integer overflow in size computations, format-string misuse, dangerous casts, lifetime violations, and (later) data races and privilege-boundary mistakes.

Findings are evidence-graded with operational definitions:

| Grade | Meaning |
|---|---|
| **PROVEN** | Every execution reaching this point exhibits the defect (e.g., a constant out-of-bounds index). |
| **FEASIBLE-PATH** | A concrete path exhibits the defect; the path's feasibility was checked by the analysis. |
| **POSSIBLE** | The analysis could not rule the defect out. Off by default. |

Each finding includes its reasoning trace:

```text
allocation (net.c:40) → stored in ctx->buf (net.c:52)
  → freed (net.c:88) → dereferenced (net.c:103)
```

The analyzer does not claim soundness (Non-goal 3). False-positive rate is measured on a labeled corpus and is a release gate.

# 24. EmbCC-LS and semantic refactoring

EmbCC-LS is the Language Server Protocol front of the shared frontend (R1): diagnostics, completion, hover, go-to-definition/declaration, references, rename, signature help, semantic highlighting, document/workspace symbols, code actions, and compiler explanations ("why" results shown inline).

Refactorings (rename, extract, move, change signature, generate declaration/definition, and in C++ modernizations such as `(int *p, size_t n)` → `std::span<int>`) are semantic transformations checked against the project graph, and report which configurations were verified (§7.1).

Depends on: error recovery, `SourceProvider`, cancellation, incrementality, project graph.

# 25. Performance provenance, PGO, and adaptive optimization

- **Performance provenance:** join runtime profiles with provenance (§14) and remarks, so a hot loop can be reported with the optimization that did not happen and why.
- **PGO / LTO:** conventional profile-guided and link-time optimization, built on EmbIR serialization.
- **Adaptive optimization (research):** compile → run → profile → transform → recompile loops. It MUST record the evidence and transformations applied so the final binary is reproducible from the manifest.

# 26. Extension API

Plugins (security checks, safety rules such as MISRA-style checks, domain analyses, FPGA/accelerator lowering) are a late capability.

- Extension points, in order of stability: diagnostics/remarks consumers → semantic model queries → EmbIR analysis passes → EmbIR transformation passes → target descriptions.
- Out-of-process first: plugins consume serialized EmbIR / semantic-model exports. This gives isolation and a stable boundary for free. In-process plugins only after internal APIs have been stable across releases.
- The API is versioned independently of the compiler.

# 27. EmbStudio and EmbDBG

**EmbStudio** is a visual client of EmbCC-LS, EmbBuild, and EmbDBG. It shows, for a selected function, the chain source → AST → EmbIR → optimized EmbIR → EmbMIR → assembly, plus remarks, memory, security, and build panels. It adds no compiler knowledge of its own (R1).

**EmbDBG** consumes the debug information and provenance of §14: source/IR/machine correlation, inlined frames, optimized-variable locations with reasons, and type information from the semantic model.

---

# PART V — DELIVERY

# 28. Why the roadmap is ordered this way

Three ordering principles, each a change from v0.1:

1. **EmbIR comes immediately after the C frontend, not after C++.** Every capability in Part IV depends on the IR, remarks, and provenance. C++ depends on none of Part IV.
2. **Self-hosting comes early.** If EmbCC is written in C, a C frontend + x86-64 backend is sufficient to compile EmbCC. The compiler's own source then becomes its largest and most demanding test, and stage-2/stage-3 comparison tests determinism for free.
3. **Every milestone has exit criteria.** A milestone is finished when its gate passes, not when its features exist.

# 29. Roadmap

## 29.0 Two milestone tracks, and which is which (v0.3)

`docs/ROADMAP.md` has its own **M0–M4**, and they are not these. That document
is the **delivery track** — what the compiler had to do next for EmbLinkOS to
exist — and it is closed: M0–M3 done, M4's host half done and its OS half the
one named milestone still open. This document's **M0–M9 are capability
tiers**, and they were written without knowing the delivery track would run
this far ahead of them.

Neither numbering should be renumbered now; both are cited elsewhere. Read
them through this mapping instead.

| This document | State | Where it actually happened |
|---|---|---|
| **M0** baseline and decisions | **Done** (v0.3) | §4 filled; ADR-0000 resolved to *evolve*; open questions 2, 3 and 4 answered below |
| **M1** C frontend library | **Done, and past its gate** | The gate was "EmbLinkOS + emlibc + EmbCC parse and type-check with zero false errors". All three are not merely parsed but *compiled and run*. Error recovery, structured diagnostics and a `SourceProvider`-less language server landed in `docs/TOOLING.md` T1–T5. Still missing from the gate: `inspect tokens\|pp\|ast\|types`, and 24-hour fuzzing |
| **M2** EmbIR v1 and x86-64 backend | **Done, except the IR's own contract** | Codegen, inline asm, ELF and EMBX, DWARF line tables: done. The **verifier** exists (`EMBCC_VERIFY=1`). The **textual form** and the **remark API** — both §9.1 MUSTs — do not (§4.3) |
| **M3** self-hosting and EmbLinkOS | **Done** | `ROADMAP.md` M3. Stage-to-stage byte-identity is a standing test over sixteen sources, on the OS |
| **M4** inspect, diagnostics, first "why" | **Half done, and the wrong half** | Diagnostics went far past the gate (stable IDs, JSON, `--explain`, fix-its, `--fix`, warning groups, a dataflow analysis, `embld --doctor`). `inspect`, `why` and the memory/stack reports have not started, because **no pass emits remarks** |
| **M5** optimizer v1 | **Done as code, not as gate** | mem2reg, SCCP, DCE, CSE/GVN, inlining and a real register allocator all exist and a kernel boots at `-O0`/`-O1`/`-O2`. The gate also requires remarks covering every inlining decision, and differential testing at `-O2` against GCC at scale. Neither is in place |
| **M6** language server and semantic incremental builds | **Language server done; the graph not started** | `embls` answers eight LSP methods from the real frontend. The project graph, USRs and interface hashes (§8.2) do not exist, so incremental builds are Level 1 |
| **M7** second target and profiles | **Target done, profiles not** | AArch64 is first-class and the M2 gates pass on it with no optimizer changes — exactly the predicted outcome. Target profiles and budgets (§20.4) do not exist |
| **M8** analysis | **Not started** | One dataflow warning exists; evidence grades, traces and the labeled corpus do not |
| **M9** C++0 | **Overtaken** | See §12.2: C++0 through C++2 and much of C++3 are done, libstdc++ compiles, and C++ runs on EmbLinkOS |

**What this mapping says about what to do next.** The delivery track pulled
the compiler hard toward *making EmbLinkOS real*, and it succeeded. The cost
is visible above: every unmet gate in this table traces back to one of two
missing foundations — **remarks (R2)** and **the project graph (§8.2)** —
and both are the kind this document warned cannot be retrofitted cheaply.
They, not another language feature, are the next structural work.

### M0 — Baseline and decisions
- Fill in §4; decide ADR-0000 (evolve vs. stage-0).
- Test harness and CI on macOS and Linux; import test corpora (§30).
- Decide ABI (§11) and debug format (§14) for `x86_64-emblink`.
- **Exit:** §4 table complete; ADR-0000..0008 accepted; CI green on the current compiler.

### M1 — C frontend library
- Lexer, preprocessor with expansion records, parser with error recovery, semantic analysis, structured diagnostics, `SourceProvider`, `inspect tokens|pp|ast|types`.
- **Exit:** full EmbLinkOS + emlibc + EmbCC source trees parse and type-check with zero false errors; c-testsuite front-end cases pass; parser and preprocessor fuzzed for 24 h without crashes.

### M2 — EmbIR v1 and x86-64 backend
- IR generation, verifier, textual form, remark API; naive instruction selection; linear-scan allocation; frame lowering; inline asm; ELF and EMBX emission; DWARF line tables.
- **Exit:** c-testsuite and the selected GCC torture subset pass on `x86_64-linux-gnu`; Csmith/YARPGen differential run of ≥ 10 000 programs with zero mismatches vs. GCC at `-O0`; ABI cross-tests pass.

### M3 — Self-hosting and EmbLinkOS
- EmbCC compiles EmbCC (stage 1 → stage 2 → stage 3); EmbCC builds emlibc and the EmbLinkOS kernel; platform layer for EmbLinkOS.
- **Exit:** stage-2 and stage-3 binaries bit-identical; EmbLinkOS built entirely by EmbCC boots and passes its own test suite; EmbCC runs natively on EmbLinkOS and rebuilds itself.

### M4 — Inspect, diagnostics, and first "why"
- All `inspect` stages; JSON/SARIF output; `why recompiled`, `why warning`; memory and stack reports (§20.1–20.2).
- **Exit:** every pass has remark hooks; reports verified against EmbLD link maps on EmbLinkOS.

### M5 — Optimizer v1
- mem2reg, constant propagation (SCCP), DCE, CSE/GVN, inlining, LICM, simple loop transforms — each emitting remarks.
- **Exit:** differential testing repeated at `-O2` with zero mismatches; `why inline / not-inline` covers every inlining decision; compile time and runtime tracked on a benchmark corpus.

### M6 — EmbCC-LS and EmbBuild semantic dependencies
- Language server over the shared frontend; project graph with stable identities and interface hashes; Level-2 incremental builds (§21).
- **Exit:** LS latency targets met on the EmbLinkOS tree (targets defined in the LS design doc); incremental build results identical to clean builds across a scripted edit corpus.

### M7 — Second target and target profiles
- AArch64 (`aarch64-emblink`) or a bare-metal 32-bit target (§5.2); target profiles; budgets enforced by EmbLD.
- **Exit:** M2 gates pass on the new target with no changes to the optimizer.

### M8 — Analysis
- Data-flow and lifetime analysis; security checks with evidence grades; build history and `embcc diff`.
- **Exit:** false-positive rate on the labeled corpus below the threshold set in the analyzer design doc.

### M9 — C++0
- **Exit:** conformance subset for C++0 passes; an EmbLinkOS userspace component written in C++0 builds and runs.

### Later (unscheduled)
C++1–C++2, EmbStudio, PGO/LTO, performance provenance, adaptive optimization, plugin API, Windows host.

# 30. Testing and quality gates

Every layer has unit tests with text-in/text-out fixtures (source → tokens, source → AST dump, IR → IR after pass, IR → assembly). On top of that:

| Kind | What | Corpus / tool |
|---|---|---|
| Conformance | Language rules | c-testsuite, GCC torture tests (selected), own clause-indexed tests |
| Differential | Same program, EmbCC vs. GCC/Clang, compare output | Csmith, YARPGen, on `x86_64-linux-gnu` |
| ABI | Mixed EmbCC/GCC caller–callee | Generated signature matrix |
| Real programs | Build and run | EmbLinkOS, emlibc, EmbCC itself, then e.g. Lua, SQLite |
| Bootstrap | Stage-2 == stage-3 | Every release |
| Fuzzing | Crash/hang freedom | Preprocessor, parser, sema, IR parser/verifier, object writer |
| Incremental | Incremental == clean | Scripted edit sequences |
| Performance | Compile time, memory, generated-code speed and size | Fixed benchmark set, tracked in CI |
| Regression | Every fixed bug | Permanent test, named after the issue |

No milestone exits with a known wrong-code bug open.

# 31. Documentation and ADRs

Documentation is part of the architecture, and the `docs/` tree mirrors the libraries. **The tree below is the intended shape; the actual `docs/` is flat** (`ARCHITECTURE.md`, `COMPATIBILITY.md`, `CXX.md`, `DECISIONS.md`, `ROADMAP.md`, `TARGET_ABI.md`, `TOOLING.md`, `USAGE.md`, and per-directory `README.md` files under `src/`). Decisions live in `docs/DECISIONS.md` as D-NNN entries rather than as ADR files; that is the project's ADR mechanism, and the seed ADRs below map onto it. Restructuring is not urgent, but the mapping should be recorded before the tree grows further. Each subsystem document covers: purpose, architecture, public API, data structures, invariants, input/output contracts, error handling, threading assumptions, performance characteristics, security considerations, examples, tests, rationale, known limitations, and future evolution.

```text
docs/
├── architecture/   overview, frontend, preprocessor, semantic-model, project-graph,
│                   embir, optimizer, backend, abi-x86_64-emblink, platform, determinism
├── language/       c, c-extensions, cpp-levels, compatibility
├── ir/             specification, instructions, types, metadata, passes, remarks
├── tools/          embcc, embcc-ls, embbuild, embld, embdbg, embstudio
├── developer/      contributing, testing, debugging, coding-style, releasing
└── design/         decisions/ (ADRs), proposals/, rejected/
```

Seed ADRs:

| ADR | Decision |
|---|---|
| 0000 | Evolve current EmbCC vs. build alongside it as stage-0 |
| 0001 | EmbCC is host-independent; EmbLinkOS is a target and future host (R5) |
| 0002 | Frontend is a library shared with EmbCC-LS (R1) |
| 0003 | EmbIR is SSA and the primary optimization boundary; EmbMIR is separate |
| 0004 | Passes record decisions as remarks (R2) |
| 0005 | Provenance preserved through all stages (R3) |
| 0006 | Output is deterministic; bootstrap comparison enforces it (R4) |
| 0007 | `x86_64-emblink` ABI and debug-info format |
| 0008 | `x86_64-linux-gnu` is a first-class test target |
| 0009 | Semantic facts are keyed by (file, configuration) |
| 0010 | C++ delivered in levels; C++0 has no exceptions/RTTI |

Architectural changes require an ADR; this document is updated in the same change, and its changelog records it. The design must not drift silently from this document, nor this document from the code.

# 32. Open questions

To be resolved by ADR before the milestone that needs them:

1. ~~Evolve or stage-0 (§4.1)~~ — **answered (v0.3): evolve.** The stages were already separate.
2. ~~Calling convention and data layout~~ — **answered: SysV AMD64 as-is** on x86-64, **AAPCS64** on AArch64, no deviation (§4, `docs/TARGET_ABI.md`).
3. ~~Debug format~~ — **answered: DWARF-4**, emitted under `-g` and consumed by EmbDBG (§4).
4. Is the Linux ELF test target acceptable as a permanent supported target, or test-only? — **still open**, but in practice it is permanent: it is how every test is refereed against GCC on both architectures.
5. Project-graph storage: custom format, SQLite, or append-only log? — **M6**.
6. ~~Does C++ go beyond C++0?~~ — **answered: yes, C++20 with libstdc++** (D-013, §12.2). The justifying component exists: the OS's `cxxdemo`, with `<iostream>`, runs on EmbLinkOS. Non-goal 2 needs amending to match.
7. ~~Second target~~ — **answered: AArch64**, and it is done. The bare-metal 32-bit case for §20 is still unmade, and is now the open part of this question: the backends assume LP64.
8. Language-server latency targets and incremental-analysis granularity — **M6**.
9. Relationship with the existing EmbBuild: which of its components are reused, which are replaced? — **M0 inventory, decision by M6**.

---

# Conclusion

EmbCC starts as a correct, deterministic, self-hosting C compiler for EmbLinkOS, testable against established compilers on Linux. Because every stage is a library, every decision is recorded where it is made, and every artifact carries its provenance, the same foundation grows into the platform described in Part IV:

- **The shared frontend** understands the program.
- **EmbIR and its remarks** record what the compiler did and why.
- **The project graph** is the shared knowledge layer.
- **EmbCC-LS** exposes that knowledge while code is written.
- **EmbBuild** understands the project; **EmbLD** understands the final layout and its budgets.
- **EmbDBG** understands the compiled program.
- **EmbStudio** presents the whole system.

The guiding question remains:

> **What does the compiler already know that developers, tools, and the operating system could use — and are we keeping it?**
