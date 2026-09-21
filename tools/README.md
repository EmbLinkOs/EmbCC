# tools

The tools around the compiler — the standalone front-ends over code that lives
in `src/`, plus the generators.

## The toolchain binaries

Each is a thin `main` over a library in `src/`, because nothing on the target
may be a subprocess (ARCHITECTURE §1).

- **embas/** — `embas -f elf64 foo.asm -o foo.o`, the standalone NASM/Intel
  assembler (`src/as`). Also reachable as `embcc -c foo.asm`. Its correctness
  bar is byte-identity with `nasm -f elf64` on the kernel corpus.
- **embld/** — `embld`, the integrated linker (`src/link`). Emits ET_EXEC ELF
  and EMBX.
- **embread/** — dumps and VERIFIES an EMBX image: the spec's §6 load sequence
  and every §8 parse-time guard, run on the host so a producer bug is found here
  rather than as a one-word refusal from the kernel. It is the verifier EmbLD is
  checked against.
- **embidx/** — `embidx`, the project's cross-TU index (vision §8.2). The
  one tool here that links **none** of `src/`: it runs
  `embcc --emit-interfaces` and stores what comes back, so the text is the
  contract between them and an external tool can consume the same input.

  It exists because two hashes are needed and the gap between them is the
  benefit. A *file* hash says the text changed and decides whether a unit
  is re-examined; an *interface* hash says what dependents observe changed
  and decides whether it is rebuilt. A comment in a shared header moves
  the first and not the second, so `embidx stale` re-examines every unit
  that includes it and rebuilds none — which is what `-MD` cannot do and
  why a one-character header edit costs minutes on a large tree.

  `embidx check` asks what no single translation unit can: two units that
  compiled different views of one struct are named with both hashes,
  before a link that would have succeeded and a program that would have
  been wrong. `embidx who` gives a declaration's definition and every
  observer. The index is sorted, versioned text and safely discardable —
  delete it and `build` reproduces it byte for byte.
- **embdbg/** — EmbDBG, our own debug-info reader. Consumes the DWARF-4 that
  `embcc -g` emits: symbolizes an address to `func:file:line`, inspects frames
  and locals, disassembles, and drives a small TUI — with no gdb in the loop.
  See `docs/tools/embdbg.md`.

## Generators and harness

- **gen-predef.sh** — regenerates `src/arch/<arch>/predef.c` from the reference gcc
  (ARCHITECTURE §5). The only way that table may change.
- **gen-selfhost-ref.sh** — builds the reference objects for the self-hosting
  fixed point (the 16 sources) and relinks stage1. See `docs/developer/selfhost-on-os.md`.
- **gen-embbuild-manifest.sh** — generates `build.ebm`, the EmbBuild manifest
  that builds EmbCC on the OS, with every unit's header closure **derived**
  (`cc -MM`) rather than hand-maintained.
- **gen-kernel-manifest.sh** — the same, for the EmbLinkOS kernel build.
- **embbuild-run.sh** — a host reference EmbBuild: the same typed-manifest walk
  the OS's EmbBuild does, path-mapped onto the host tree. The host half of the
  two-implementation oracle.
- **os-stage.sh**, **os-build-embld.sh** — stage the tree into an OS image and
  cross-build `embld.elf` for the OS.
