# src/arch/aarch64

AArch64, **AAPCS64** — the EmbLinkOS ARM64 kernel and userspace, QEMU `virt`
(`--target=aarch64-elf`, D-011). What it supports against x86-64:
[docs/COMPATIBILITY.md](../../../docs/COMPATIBILITY.md).

| file        | what it is |
|-------------|------------|
| `codegen.c` | IR -> AArch64 machine code: AAPCS64 argument placement, frame layout, atomics, libgcc calls for binary128 |
| `emit.c/.h` | the instruction encoder |
| `asm.c/.h`  | the inline-asm template assembler (GNU syntax) |
| `irgen.c`   | AAPCS64 `va_arg`, and extended asm through `asm.c` |
| `predef.c`  | predefined macros, generated from `aarch64-elf-gcc` |

## codegen.c

A peer of the x86-64 backend, not a layer over it — same contract
(`../backend.h`). It is deliberately at the stage the x86 backend began at:
every vreg in a stack slot, every operation through `x9` (with `x10` for a
second operand, `x11`/`x12` for addresses, `v16`/`v17` for floats). The x86
backend's quality passes are x86-shaped and not shared yet; D-011 records when
that should change.

Two things differ from x86-64 by necessity:

- **Slots are `[sp, #off]` with a non-negative offset**, not `[rbp-N]`: the
  scaled 12-bit unsigned-offset load reaches 32 KiB from `sp`, the signed form
  only ±256 from `x29`. A function with a VLA moves `sp`, so it pins the
  post-prologue `sp` in callee-saved `x19` and addresses its slots from there.
- **Argument placement is computed to AAPCS64** (`a64_place`, shared by calls
  and the prologue), ignoring irgen's SysV classification: eight `x` and eight
  `v` argument registers on their own counters, a Homogeneous
  Floating-point Aggregate one member per `v` register, other composites of 16
  bytes or less in consecutive `x` registers, anything larger passed as a
  pointer to a caller-made copy (B.3), results larger than 16 bytes through
  `x8`, and no back-filling once a register file is spent (C.3, C.11).

## Types and the ABI

- Plain `char` is **unsigned**; `wchar_t` is `unsigned int`.
- **`long double` is IEEE binary128.** There is no quad-precision hardware, so
  — like gcc — every operation calls libgcc (`__addtf3`…, `__eqtf2`…,
  `__floatditf`, `__fixtfdi`, `__extend`/`__trunc`), values passed and returned
  in `q` registers; a call in the middle of an IR instruction is safe because
  nothing lives in a register across instructions.
- `_Complex` of any precision is an HFA of two: `s`, `d` or `q` registers.
- Variadic functions save `x0`–`x7` and `q0`–`q7` in the prologue (not the `q`
  registers under `-mgeneral-regs-only`), and `va_start` fills AAPCS64's
  32-byte record; `va_list` stays a `char *` pointing at it, which B.3 makes
  ABI-compatible with gcc's struct `va_list`.
- The atomics are `ldxr`/`stxr` retry loops between full barriers
  (sequentially consistent by fences).
- `-mgeneral-regs-only` (the kernel's mode): any floating-point operation is
  refused, as gcc refuses it.
- `-g`: each variable's DWARF location is relative to `x29`; the prologue
  leaves `sp` (and `x19`) exactly the frame size below it.

## emit.c — the encoder

Same contract as the x86 encoder: exactly the encodings codegen emits, and an
instruction it cannot encode is a missing function, not a wrong word. **Every
encoding is refereed by `aarch64-elf-objdump`**: `tools/a64check` emits each
one with what it claims to be, and tests/golden/aarch64/arm64-encoding.sh
disassembles and diffs them. A backend that assembles its own instructions has
no assembler to catch a wrong bit — that test caught a signed 4-byte load
encoded into an unallocated word, which the CPU traps on.

## asm.c and irgen.c — extended asm

irgen substitutes the operands (`%0`, `%w0`, `%x0`, `%[name]`, constraints
`r`, `=r`, `+r`, `i`, and `register … __asm__("x0")` variables) and hands
`asm.c` plain GNU text; it returns bytes, or a message naming the statement it
could not assemble. The vocabulary is a measurement, not an ambition: exactly
what the EmbLinkOS ARM kernel's inline asm contains (67 distinct templates:
system registers, `tlbi`, barriers, hints, exclusives), and
tests/golden/aarch64/arm64-asm.sh requires the same bytes as `aarch64-elf-as`
for every one and for every entry of the assembler's own tables. The backend
loads inputs into their registers, pre-loads `"+"` outputs, splices the bytes
and stores outputs back; operand registers never include `x12` (a far slot's
scratch) or anything callee-saved.
