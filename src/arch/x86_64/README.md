# src/arch/x86_64

x86-64, **System V AMD64 ABI** — the EmbLinkOS x86-64 kernel and userspace
(`--target=x86_64-elf`, the default). What it supports against aarch64:
[docs/language/compatibility.md](../../../docs/language/compatibility.md).

| file        | what it is |
|-------------|------------|
| `codegen.c` | IR -> x86-64 machine code: the calling convention, frame layout, register allocation, the x87 unit for `long double` |
| `emit.c/.h` | the instruction encoder |
| `irgen.c`   | SysV `va_arg`, and the extended inline-asm assembler |
| `topasm.c`  | file-scope `__asm__` (crt0's `_start`) |
| `as.c/.h`   | EmbAS, the NASM-syntax assembler behind `embas` and `embcc -c foo.asm` |
| `predef.c`  | predefined macros, generated from `x86_64-elf-gcc` (tools/gen-predef.sh) |

## codegen.c — the value model

The baseline (`-O0`) is deliberately simple: every vreg lives in a stack slot,
every operation goes through `rax` (and `rcx` for a second operand), so all
values are in memory across statements. On top of that sit quality passes,
each gated by optimization level so the lower levels stay reproducible (`-O0`
output is byte-identical across changes, which keeps the self-hosting fixed
point stable):

- **Stack-slot coalescing (always on).** Temporaries whose live ranges do not
  overlap share one slot (`coalesce_temps`); locals whose lexical scopes — or,
  when not address-taken, liveness — are disjoint share one (`coalesce_locals`).
- **RAX residency cache (`-O1`).** A value just computed into `rax` is not
  reloaded from its slot to be used again.
- **Register allocation (`-O2`).** Eligible vregs live in the callee-saved
  registers (rbx, r12–r15), plus caller-saved r8–r11 where no call intervenes:
  backward-liveness dataflow, a precise interference graph, Chaitin-Briggs
  colouring with move-coalescing bias. Anything touching an opaque raw-slot
  site (a float op, address-of, an atomic, `va_start`, a struct/float call
  argument, inline asm, a long double) stays in memory. An `ADD` whose sole
  use is the next load or store folds into its addressing.

## Calling convention and types

- Integer arguments in `rdi rsi rdx rcx r8 r9`, floats in `xmm0–7`, returns in
  `rax`/`rdx` and `xmm0`/`xmm1`; aggregates classified by eightbyte (INTEGER /
  SSE / MEMORY), 16-byte stack alignment at every `call`. Plain `char` is
  signed; `wchar_t` is `int`.
- **`long double` is x87 80-bit extended** in 16 bytes. Its values live in
  16-byte slots (never a register) and are computed on the x87 stack, empty
  between IR instructions: `fld`/`fstp tword`, `faddp`/`fsubp`/`fmulp`/`fdivp`,
  `fchs`, `fucomip` (whose flags read exactly as `ucomisd`'s), `fild`, and
  `fistp` under a truncating control word (no SSE3 `fisttp`). It is X87 class:
  passed in memory at 16-aligned slots, returned in `st0`. A struct that is
  exactly one long double also returns in `st0`, and a `long double _Complex`
  in `st0`/`st1` (COMPLEX_X87).
- `_Complex` float/double are SSE class: one packed `xmm` or two.
- `-mno-sse` (kernel mode): the varargs prologue skips its xmm spill and any
  SSE float op is refused; the x87 stays available, as in gcc.
- `__attribute__((aligned(N)))` up to 16 is honoured in layout; beyond that is
  refused. A VLA moves `rsp`: frame slots are `rbp`-relative and `leave`
  restores `rsp`, so an allocation only has to sit above the outgoing-argument
  area.

## emit.c — the encoder

Exactly the encodings codegen emits, width-parameterized (REX.W for the 64-bit
forms): mov/movsx/movzx/movsxd at 1/2/4/8 bytes, the ALU ops, division and
shift families, lea, setcc, call/jmp/jcc rel32, SSE scalar float ops and
conversions, the x87 memory and register forms, push/pop. Anything unencodable
is a missing function — a build failure, never a wrong byte. (The byte buffer
itself is target-neutral: `../code.c`.)

## irgen.c — va_arg and inline asm

`va_arg` walks SysV's `__va_list_tag` (gp_offset / fp_offset /
overflow_arg_area / reg_save_area); a long double comes from the overflow area
at a 16-aligned slot. `va_list` is `char *` pointing at the tag, which is what
newlib's `vfprintf` reads.

Extended inline asm is assembled here: the kernel's full x86-64 vocabulary in
AT&T syntax, every encoding byte-verified against objdump
(tests/golden/x86_64/inline-asm*.sh), operand registers resolved from constraints
(fixed `a/b/c/d/S/D`, allocatable `r`/`m`/`i`, `x` for xmm, `"+"` operands
loaded with the lvalue's value first), clobbered and template-written
registers excluded from allocation.

## topasm.c — file-scope asm

The two-pass mini-assembler for file-scope `__asm__`: crt0's `_start`
vocabulary — `.global`/`.globl`, named and numeric-local labels,
`and $imm,%reg`, `call sym` (PLT32), `jmp local-label`, `ret`. (aarch64 has no
file-scope asm yet; it refuses one.)

## as.c — EmbAS

A standalone **NASM/Intel-syntax** assembler, so the kernel's hand-written
`.asm` builds without nasm. Two entry points, one implementation
(`as_assemble`): `embas -f elf64 foo.asm -o foo.o`, and `embcc -c foo.asm`.

`preprocess` (strip comments, expand `%macro` — nested, depth-limited — and
mangle `.local` labels) -> parse each line into an item -> `place` (relax jumps
to a rel8/rel32 fixpoint, as nasm does) -> `emit_bytes` -> ELF through the
shared writer. Directives: `section`, `global`/`extern`, `align`,
`db/dw/dd/dq`, `resb/w/d/q`, `incbin`, `%macro`. Operands include `[gs:disp]`
segment overrides, `[rel sym]` RIP-relative references and `~` in immediates;
relocations are `R_X86_64_PC32` and `R_X86_64_64`. `-f bin` (the flat boot
stages) is not implemented and says so.

The bar is byte-identical to nasm: all six kernel `.asm` files assemble to the
same code, symbols and relocations (tests/golden/x86_64/assembler.sh), and the
kernel built from them with embcc + embas + embld boots
(tests/golden/x86_64/embbuild-kernel.sh, opt-in).
