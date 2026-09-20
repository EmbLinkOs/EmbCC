# EmbCC documentation

Laid out as [design/vision.md](design/vision.md) §31 asks: the tree mirrors
the libraries, and each subsystem's document says what it is, how it works,
and which test proves it.

## Start here

| | |
|---|---|
| [design/vision.md](design/vision.md) | **The specification.** What EmbCC is, the invariants it never breaks, and the order it gets built in. Everything else is subordinate to it |
| [architecture/overview.md](architecture/overview.md) | How the compiler is actually put together, phase by phase |
| [tools/embcc.md](tools/embcc.md) | Using it: the command line |

## architecture/

| | |
|---|---|
| [overview.md](architecture/overview.md) | one process, the phases, and why each is shaped the way it is |
| [abi.md](architecture/abi.md) | the target ABIs: SysV AMD64 and AAPCS64, data layout, calling convention |

Per-library detail lives with the library, as a `README.md` beside the code:
[src/platform](../src/platform/README.md) · [src/lex](../src/lex/README.md) ·
[src/cpp](../src/cpp/README.md) · [src/parse](../src/parse/README.md) ·
[src/sema](../src/sema/README.md) · [src/ir](../src/ir/README.md) ·
[src/opt](../src/opt/README.md) · [src/arch](../src/arch/README.md) ·
[src/elf](../src/elf/README.md) · [src/link](../src/link/README.md)

## language/

| | |
|---|---|
| [compatibility.md](language/compatibility.md) | what C EmbCC accepts, per clause and per extension, per architecture |
| [cpp-levels.md](language/cpp-levels.md) | C++: the staged milestones CX1–CX9 and how the lowering works |

## ir/

| | |
|---|---|
| [specification.md](ir/specification.md) | EmbIR: its form, its textual syntax, and the round-trip that keeps them honest |

## tools/

| | |
|---|---|
| [embcc.md](tools/embcc.md) | the driver: options, targets, what it accepts |
| [diagnostics.md](tools/diagnostics.md) | diagnostics, `--explain`, fix-its, warnings, remarks, the language server, `embld --doctor` |
| [embdbg.md](tools/embdbg.md) | the debugger's requirements and what it consumes |

## developer/

| | |
|---|---|
| [selfhost-on-os.md](developer/selfhost-on-os.md) | building EmbCC with EmbCC, on EmbLinkOS |
| [todo.md](developer/todo.md) | the running list of known gaps |

Testing is documented with the tests: [tests/README.md](../tests/README.md).

## design/

| | |
|---|---|
| [vision.md](design/vision.md) | the specification (v0.3) |
| [decisions.md](design/decisions.md) | the D-NNN decision records — **this project's ADRs** |
| [roadmap.md](design/roadmap.md) | the delivery track, M0–M4, and what each milestone proved |
| [workplan.md](design/workplan.md) | the work streams behind the milestones |
| [vision-first.md](design/vision-first.md), [vision-longterm.md](design/vision-longterm.md) | the earlier vision documents, kept for provenance |

## Where this differs from §31, and why

The vision's §31 sketches a tree this one follows but does not match
exactly. The differences are deliberate and are listed so the gap stays a
decision:

- **ADRs are `design/decisions.md`, not `design/decisions/` as separate
  files.** This project records decisions as numbered D-NNN entries in one
  document, which is its ADR mechanism; the seed ADRs §31 lists map onto
  those entries. Splitting them into files is not obviously an improvement
  and would break every existing citation.
- **`tools/diagnostics.md` is not split per tool.** §31 asks for
  `embcc-ls`, `embld` and friends separately. That document is one
  narrative — the staged T1–T6 work, each stage building on the last — and
  cutting it into three would lose the thread for no gain. The per-tool
  split is worth doing when the per-tool content outgrows it.
- **`architecture/` is thin.** §31 lists chapters for the frontend,
  preprocessor, semantic model, project graph, optimizer, backend, platform
  and determinism. Most of that content exists as a `README.md` beside its
  library, which keeps it next to the code it describes and is where it has
  stayed accurate. `project-graph` has no chapter because
  [it does not exist yet](design/vision.md) (§8.2).
- **No `embstudio` or `embbuild` chapter**: neither is part of this
  repository.
