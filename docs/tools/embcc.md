# EmbCC / EmbLD — usage

How to invoke the compiler (`embcc`) and the linker (`embld`). This is the CLI
reference; for *what* is implemented and what is still coming, see
[todo.md](../developer/todo.md); for the design, [ARCHITECTURE.md](../architecture/overview.md).

EmbCC is a self-contained C compiler + linker: `embcc` turns C into ELF objects,
`embld` links them into an ELF executable (or an EMBX binary). No GCC/binutils in
the loop — the pair compiled and linked the EmbLinkOS kernel to a booting desktop.

## `embcc` — the compiler

```
usage: embcc [-E] -c FILE.c [-o FILE.o] [--target=TRIPLE] [-I DIR]... [flags]
       embcc --version | --dump-predef | --emit-empty-object FILE
```

**Modes**

| Flag | Meaning |
|------|---------|
| `-c FILE.c` | Compile one translation unit to an ELF object. |
| `-E` | Preprocess only — write the expanded source to stdout. |
| `--version` | Print the version and exit. |
| `--dump-predef` | Print the built-in predefined macros (what `-E` starts from). |
| `--emit-empty-object FILE` | Write a valid empty ELF object (for empty TUs / build plumbing). |

**Options**

| Flag | Meaning |
|------|---------|
| `--target=TRIPLE` | Which machine to emit for: `x86_64-elf` (default) or `aarch64-elf`. Fixed for the whole compile — it selects the backend, the predefined-macro set, `e_machine` and the relocation types together (D-011). Also accepted before `--version`/`--dump-predef`, which then describe that target. |
| `-o FILE.o` | Output path (default: the input with `.o`). |
| `-I DIR` | Add a header search directory (repeatable). |
| `-isystem DIR` | Add a *system* header search directory (repeatable). |
| `-g` | Emit debug info (DWARF). |
| `-O0` / `-O1` / `-O2` | Optimization level. Bare `-O` = `-O1`. `-O2` enables register allocation. |
| `-funwind-tables` (also `-fasynchronous-unwind-tables`, `-fexceptions`) | Emit unwind tables (`.eh_frame`), so a C++ exception can unwind through the unit's functions. Always on for C++; off by default for C (a C callback library that C++ code may throw through wants it). `-fno-…` turns it off. |
| `-fno-exceptions` | C++ without exceptions: `throw`/`try` are errors and no cleanup regions are made (as g++'s flag). |

**Codegen flags** (for freestanding / kernel targets)

| Flag | Meaning |
|------|---------|
| `-mno-sse` / `-mno-sse2` | No SSE/SSE2 codegen (kernel runs SSE-off until `fpu_init`). |
| `-mno-mmx` | No MMX. |
| `-mno-80387` | No x87 FPU. |
| `-mno-red-zone` | No red zone (required for kernel / interrupt-taking code). |
| `-mgeneral-regs-only` | Integer registers only (implies the above). |

### Examples

```sh
# a normal object
embcc -c hello.c -o hello.o

# preprocess only
embcc -E hello.c

# kernel-grade: freestanding, SSE-off, no red zone, with include roots
embcc -c kernel/mm/pmm.c -Ikernel -mno-sse -mno-sse2 -mno-red-zone -O2 -o pmm.o
```

**Compiling for aarch64**

```sh
# an object for EmbLinkOS's second architecture
embcc --target=aarch64-elf -c prog.c -o prog.o

# what the target's headers say it is
embcc --target=aarch64-elf --dump-predef | grep __aarch64__
```

The result is a real `EM_AARCH64` ET_REL object that `aarch64-elf-ld` links
against stock newlib. `embld` does not read or write aarch64 objects yet, and
`embas` assembles x86-64 NASM syntax only — so an aarch64 link goes through
binutils for now. Extended inline asm works, with the vocabulary the ARM
kernel uses (`src/arch/aarch64/asm.h`) and the constraints `r`, `=r`, `+r` and `i`
plus `register … __asm__("x0")` variables; a template outside it is refused
with the offending statement named. `-g` describes aarch64 frames too (x29
is the frame base), so gdb debugs an aarch64 program in QEMU the same way as
an x86-64 one — `tests/golden/debug-live.sh` does exactly that on both.

To run what it produced, `make test-arm64` links each test into a bare-metal
image and executes it under `qemu-system-aarch64 -M virt` — see
`tests/harness/README.md`.

## `embld` — the linker

```
usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR] [--lma-offset N] INPUT.o|INPUT.a ...
       embld --embx --cap NAME [--cap NAME]... -o OUT.embx INPUT.o ...
```

| Flag | Meaning |
|------|---------|
| `-o OUT` | Output path. |
| `-e ENTRY` | Entry-point symbol (e.g. `_start`). |
| `-Ttext ADDR` | Base virtual address of `.text` (e.g. the higher-half kernel base). |
| `--lma-offset N` | Subtract `N` from each segment's vaddr to get its load address (`p_paddr`) — how a higher-half kernel is loaded low and run high. |
| `--embx` | Emit an **EMBX** binary (the OS's native capability-declaring format) instead of a plain ELF. |
| `--cap NAME` | Declare a capability the EMBX binary requires (repeatable). Names match the OS cap set: `filesystem`, `network`, `gpu`, `audio`, `camera`, `usb`, `serial`, `rawdisk`, `kernel_ext`. The kernel enforces the declared set ⊆ the grantor's at load. |

**Linker-defined symbols** are provided automatically when referenced and
otherwise undefined, so no linker script is needed: `kernel_end`, `_end`, `end`,
`__bss_end`, `__kernel_end` (the vaddr past the last `.bss` byte), and the
`__init_array_start`/`_end`, `__fini_array_*`, `__ctors_*`, `__dtors_*` bracket
family. Each other named section (an *orphan*, e.g. a `section(".embk_exports")`
table) is laid out contiguously and bracketed too: `__embk_exports_start`/`_end`
for a dotted name, `__start_mytab`/`__stop_mytab` for a C-identifier one. A real
definition always wins.

### Examples

```sh
# link a freestanding ELF (e.g. the kernel) at the higher-half base
embld -e _start -Ttext 0xFFFFFFFF80100000 -o kernel.elf *.o

# link a plain executable
embld -o app app.o libfoo.a

# emit an EMBX binary that declares it needs the filesystem capability
embld --embx --cap filesystem -o app.embx crt0.o app.o libc.a
```

## Building for EmbLinkOS

EmbCC is the toolchain for the OS. A typical app is compiled against the sealed
ABI and linked into an ELF (or EMBX):

```sh
embcc -c app.c -I/path/to/abi/include -o app.o
embld -o app.elf crt0.o app.o libc.a           # or: --embx --cap filesystem -o app.embx
```

The OS side of this — where the ABI lives (`/system/abi`), how apps declare their
namespace, and how the build is driven — is documented in the OS repo:
`myos/docs/TOOLCHAIN.md` (building for/on the OS) and `myos/docs/BUILD.md`.

## `embas` — the assembler

```
usage: embas [-f elf64] [-o OUT] INPUT.asm
```

NASM/Intel syntax. The same code runs when you hand `embcc -c` a `.asm` file, so
either entry point works:

```sh
embas -f elf64 boot.asm -o boot.o
embcc -c boot.asm -o boot.o          # equivalent
```

Its correctness bar is byte-identity with `nasm -f elf64`, met on all six of the
kernel's hand-written `.asm`. `-f bin` (flat binary, for the 16/32-bit boot
stages) is **not** implemented and reports an error rather than miscompiling.

## `embdbg` — reading the debug info back

Compile with `-g`, then:

```sh
embcc -c foo.c -g -o foo.o
embld -o foo.elf crt0.o foo.o libc.a
embdbg foo.elf funcs                 # list functions + ranges
embdbg foo.elf line 0x401234         # address -> func:file:line
```

`embdbg FILE` with no subcommand prints the full command list (symbolize,
inspect locals, disassemble, and a small TUI). It reads the DWARF-4 EmbCC emits
— no gdb in the loop. See [EMBDBG_Requirements.md](embdbg.md).

## What is not there

Refused loudly rather than faked, per THE RULE:

- **`embas -f bin`** — flat-binary output; the boot stages still use nasm.
- **`embld -T SCRIPT.ld`** — full linker scripts. Not needed so far: the
  end-of-image and bracket symbols above are auto-provided, which is what let
  the kernel link without one.
- **`embld` for aarch64** — it links x86-64 objects only; the aarch64 kernel
  links with the cross `ld`.
- **`section("name")` on a function or a local** — honored on file-scope
  variables only; elsewhere it is an error rather than a silent `.text`.
- **GNU integer `_Complex`** (`_Complex int`, `2i`) — the C99 floating
  complex types are supported; the integer ones are a GNU extension.
- **`va_arg` of a struct or a complex** — passing one through `...` works;
  reading it back with `va_arg` is refused.
- **A static complex initializer that multiplies or divides two complex
  values** — literals, casts, `+`, `-` and scaling by a real fold; `*` and `/`
  of two complex values are left to run time, which is where Annex G's
  special cases (libgcc) apply.
- **C++ beyond what docs/language/cpp-levels.md marks done** — each refusal names the
  milestone that brings the construct (e.g. "exceptions are not supported
  yet (CX5)").
- **`__thread`/TLS, PIE/PIC output** — out of scope by decision
  (ARCHITECTURE §8, DECISIONS D-008).

Run `embcc`, `embas` or `embld` with no arguments for the current usage line.

## `-S` — the assembly the backend emitted

    embcc -S prog.c -o prog.s

x86-64 only; for aarch64 it refuses, because there is no disassembler here
and text that cannot be verified is worse than no text.

**The acceptance is byte-identity**: assembling the output with the GNU
assembler gives the same `.text` and the same relocations as `embcc -c`, for
every program in the execution corpus (tests/golden/asm-S.sh). That bar is
what shaped the output:

    	.byte	0x48,0x89,0xe5	# mov    %rsp,%rbp
    	.byte	0x48,0x8d,0x05,0x00,0x00,0x00,0x00	# lea    0x0(%rip),%rax
    	.reloc	.-4, R_X86_64_PC32, .LC0-4

Each instruction is its **bytes**, with the disassembly as a comment, and
each relocation is attached with an explicit `.reloc`. Written as ordinary
mnemonics instead, the file does not reassemble to the same object, because
an assembler chooses what the backend already chose: it writes
`sub $0x10,%rsp` in four bytes where the backend wrote seven, shrinks a
`0f 84` rel32 branch to a `74` rel8, and resolves `lea add(%rip)` to a fixed
displacement with no relocation at all. Measured on this corpus, that
version silently miscompiled 10 of 89 programs. Emitting the bytes leaves
the assembler nothing to choose.

The file is still readable — every instruction carries its disassembly — but
it is a record of what was compiled, not a starting point for hand-editing.
Idiomatic assembly would need the backend to record text as it emits bytes,
one call site producing both, which is a larger change to the 71 emit
helpers and the only way to get it without a second implementation that can
drift.

## `--emit-interfaces` — what this unit provides, and what it compiled against

    embcc --emit-interfaces -Iinclude a.c

    provides c:@F@use          e7616081aa18a82b
    uses     c:@F@compute      2037fc2425d84b3d
    uses     c:@V@shared       3769e4c2b874ce54
    uses     c:@S@Point        8109bf0e612d76d5

Each line is a **USR** — a name for a declaration that survives unrelated
edits — and an **interface hash** over what dependents can observe.

`-MD` says this unit read `h.h`, so *any* edit to `h.h` rebuilds it. These
hashes say what it actually depends on, which is what vision §21's Level-2
incremental build needs: **a header edit that changes no hash this unit uses
does not require rebuilding it.**

What is hashed is what a dependent compiles in — a function's signature, a
global's type, a struct's size, alignment, member names, types, offsets and
bit positions. What is **not**: file names, line numbers, declaration order,
and a function's body. Moving a declaration or rewriting a body must not
invalidate anybody, or the exercise buys nothing.

An entity with internal linkage carries its file in its USR, because two
units may each have a `static int count` and they are not the same thing.

The hash is FNV-1a over a canonical string built from the semantic model in
a fixed order: same input, same hash, on any host (R4) — which a build cache
depends on absolutely.
