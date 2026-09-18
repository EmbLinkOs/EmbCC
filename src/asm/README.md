# src/asm

Instruction encoding — ../../docs/ARCHITECTURE.md §2 (integrated; there is no
external assembler on the target).

`emit.c` encodes what codegen emits, width-parameterized (REX.W for the 64-bit
forms): mov/movsx/movzx/movsxd at 1/2/4/8 bytes, the alu ops, both the division
and shift families (signed and unsigned), lea, setcc over the signed and
unsigned condition sets, call/jmp/jcc rel32, the SSE scalar float ops and the
int↔float conversions, and the push/pop forms the prologue needs.

`topasm.c` is the two-pass mini-assembler for **file-scope** `__asm__` — crt0's
`_start` stub vocabulary: `.global`/`.globl`, named and numeric-local labels,
`and $imm,%reg`, `call sym` (PLT32), `jmp local-label`, `ret`.

Anything unencodable is a missing function — a build failure, never a wrong
byte.

Two neighbours do related work: `../ir` assembles *extended* inline asm
(operands bound from constraints), and `../as` is EmbAS, the standalone
NASM/Intel-syntax assembler for whole `.asm` files.

## emit_arm64.c

The AArch64 encoder, the counterpart of `emit.c`. Same contract: exactly the
encodings the aarch64 codegen emits, and an instruction it cannot encode is a
missing function that fails at build time rather than a silently wrong word.

**Every encoding is refereed by `aarch64-elf-objdump`.** `tools/a64check`
emits each one and prints what it CLAIMS the instruction is;
`tests/golden/arm64-encoding.sh` disassembles the bytes and diffs the two — 98
instructions including the displacement of every branch. A backend that
assembles its own instructions has no assembler to catch a wrong bit, and a
wrong bit is a silently wrong program rather than a build failure. That test
has already earned its place: it caught a signed 4-byte load being encoded
into an unallocated word (there is no "load signed word into a W register" on
AArch64 — a 32-bit load already delivers every bit), which objdump prints as
`.inst 0x… ; undefined` and the CPU traps on.

## asm_arm64.c

The aarch64 inline-asm assembler — the counterpart of irgen's x86
`asm_assemble`, in its own file rather than inside irgen. irgen substitutes the
operands (`%0`, `%w0`, `%x0`, `%[name]`) and hands it plain GNU text; it
returns bytes or a message naming the statement it could not assemble.

It is sized to a measurement, not an ambition: the vocabulary is what the
EmbLinkOS ARM kernel's inline asm actually contains — 67 distinct templates,
collected by preprocessing every C file the aarch64 kernel build compiles —
and nothing past it. `tests/golden/arm64-asm.sh` feeds those templates, plus
one line per entry of the assembler's own tables (every system register,
`tlbi` operation, barrier option and hint, generated so none can go
unchecked), to both this and `aarch64-elf-as`, and requires identical bytes.
