# src/codegen

IR → x86-64, System V AMD64 — ../../docs/ARCHITECTURE.md §4.

## The value model

The baseline (`-O0`) is deliberately simple: every vreg lives in a stack slot,
every operation goes through `rax` (and `rcx` for a second operand), so all
values are in memory across statements. Correct and easy to reason about. On top
of that the codegen carries several quality passes, each gated by optimization
level so the lower levels stay reproducible (`-O0` output is byte-identical
across changes, which is what keeps the self-hosting fixed point stable):

- **Stack-slot coalescing (always on).** Temporaries whose live ranges do not
  overlap share one slot (`coalesce_temps`, single-basic-block liveness); local
  variables whose *lexical scopes* are disjoint, or — for non-address-taken
  locals — whose *liveness* is disjoint, share a slot (`coalesce_locals`,
  interval colouring). This keeps frames near GCC's; a stack pointer used past
  its scope is UB, so scope-based sharing is sound even for address-taken arrays.
- **RAX residency cache (`-O1`).** A value just computed into `rax` is not
  reloaded from its slot to be used again; a zero-extend-aware relaxation lets a
  narrow store be reused by a wider read.
- **Register allocation (`-O2`).** Eligible vregs (temps, and scalar
  int/long/pointer/char/short locals, params, and scalar-integer call arguments)
  live in the five **callee-saved** registers (rbx, r12–r15) instead of memory —
  callee-saved so a value survives a call untouched, with no spill-around-call
  logic. Real backward-liveness dataflow builds a precise interference graph
  (interfering within live-in and within live-out, so a dying operand and a
  fresh result can share); Chaitin-Briggs optimistic colouring assigns registers
  with move-coalescing bias and spills the most-constrained node. Any vreg
  touching an opaque raw-slot site (a float op, address-of, atomic, va_start, a
  struct/float call arg, or inline asm — or live across an asm) stays in memory.
  Store and `memcpy`/`memzero` addresses are allocated too; non-leaf functions
  additionally get the caller-saved `r10`/`r11` for values that do not cross a
  call, and `r8`/`r9` join the pool with call arguments set up by a parallel
  move. Values are materialised straight into their home register — constants,
  addresses, loads, widening loads, comparison booleans — rather than detouring
  through `rax`.

## Target details

- Integer args in the six SysV registers; return in `rax`/`rdx`. Floats in
  `xmm0–7`. 16-byte stack alignment at every `call`.
- `-mno-sse` (kernel mode): the varargs prologue skips its xmm register-save
  spill and any float op is refused loudly, so nothing #UDs before CR4.OSFXSR is
  set.
- `__attribute__((aligned(N)))` is honoured in layout — struct member offsets,
  struct size/align, and stack-slot alignment (N ≤ 16; larger is refused).
- Intra-unit calls are patched here (rel32 once all functions are placed);
  external call sites, and string/global/function-address sites, become
  relocations the driver hands to the ELF writer.

Inline asm carrying explicit register constraints is assembled in `../ir` (see
its README); its operands reach codegen already bound to fixed registers.

## The aarch64 backend

`codegen_arm64.c` is the second backend — IR → AArch64, AAPCS64 (D-011). It
is a peer of `codegen.c`, not a layer over it: same signature, same site
lists, so the driver picks one on `--target=` and nothing downstream knows
which machine produced the image.

It is deliberately at the stage `codegen.c` began at — every vreg in a stack
slot, every operation through `x9` (with `x10` for a second operand, `x11`/`x12`
for addresses, `v16`/`v17` for floats). None of the quality passes above are
shared yet, because all of them are x86-shaped in their current form; D-011
records when that should change and why not yet.

Two things differ from the x86 backend by necessity rather than by stage:

- **Slots are `[sp, #off]` with a non-negative offset**, not `[rbp-N]`. The
  scaled 12-bit unsigned-offset load reaches 32 KiB from `sp`; the signed form
  reaches ±256 from `x29`. Frame-slot access is one instruction because of
  that choice.

- **Argument placement is recomputed to AAPCS64**, ignoring `ir_arg`'s
  `on_stack`/`stk_off`, which irgen fills in with the SysV classification.
  Eight integer argument registers rather than six, floats on their own NSRN
  counter, a Homogeneous Floating-point Aggregate (one to four same-typed
  floats) one member per `v` register, other composites of 16 bytes or fewer in
  consecutive `x` registers, anything larger passed as a pointer to a copy the
  caller makes (stage B.3), and no back-filling once a register file is spent
  (C.3, C.11). One classifier, `a64_place`, serves calls and the prologue
  alike. The outgoing area is sized from
  those rules too, not from `fn->outgoing_bytes`.

Inline asm arrives assembled (irgen's `gen_asm_arm64`, over
`../asm/asm_arm64.c`): the backend loads each input into the register its
constraint chose, pre-loads each `"+"` output with the lvalue's current value
(`ir_asm_op.inout` — the x86 path does not, see the README), splices the bytes
and stores the outputs through their addresses. Operand registers never
include `x12`, which a far stack slot borrows, or anything callee-saved.

Variadic functions save `x0`–`x7` and `q0`–`q7` in the prologue (not the `q`
registers under `-mgeneral-regs-only`) and `va_start` fills AAPCS64's 32-byte
record; `va_list` stays a `char *` pointing at it, which B.3 makes
ABI-compatible with gcc's struct `va_list`. The atomics are `ldxr`/`stxr`
retry loops between full barriers.

Refused loudly rather than emitted wrong: `-g`.
