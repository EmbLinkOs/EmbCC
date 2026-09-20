# EmbIR

*The boundary between language semantics and machine code. Everything after
it is language-independent — which is why the C++ front end could be added
without the optimizer or either backend knowing.*

Code: [`src/ir/ir.h`](../../src/ir/ir.h) (the form),
[`src/ir/irgen.c`](../../src/ir/irgen.c) (built from the AST),
[`src/ir/irprint.c`](../../src/ir/irprint.c) (printed),
[`src/ir/irparse.c`](../../src/ir/irparse.c) (read back).
Library notes: [`src/ir/README.md`](../../src/ir/README.md).

## 1. The form

A **linear three-address IR over virtual registers**, typed by width rather
than by a type graph. Not SSA: SSA is built on demand by the optimizer
(`pass_mem2reg`) and taken down again, because carrying it everywhere would
cost more than the two passes that want it.

That decision held. The IR did not need replacing when the optimizer
arrived, and `docs/architecture/overview.md` §3 records why.

Each instruction has an opcode, up to three virtual-register operands, a
destination, an operation width, and — since it is an invariant, not a
nicety — **a source location** (§4).

## 2. What it carries, and what it does not

EmbIR is a **module**, not a view over the parse tree. It was the latter
once: `struct ir_ins` and `struct ir_func` pointed into the AST in seven
places, which is why the textual form could be printed and never read back.
Now:

| the IR carries | so that |
|---|---|
| the function's name, file, line, linkage, parameter and variable counts | no pass has to reach back for them |
| an interned **symbol table** — every name it mentions, with the linkage facts the backends read | a call's target is a name, resolvable without a parse tree |
| **per-slot type facts**: size, alignment, volatile, long-double, `__int128`, integer-or-pointer | the questions codegen and the optimizer actually asked of `struct type` |
| **the ABI answers**: SysV classification, AAPCS64 HFA element count and size, by-reference passing | classification happens once, at irgen, where the types still exist |

What it deliberately does **not** carry is the **type graph** — member
names, nested types, array bounds. Code generation needs none of it. DWARF
does, so the debug emitter still reads the AST, and a *parsed* unit (whose
`src` is NULL) can be printed, analysed and transformed but not handed to
the DWARF writer. That boundary is a decision, not an oversight: carrying a
full type graph in EmbIR is a much larger commitment than the handful of
facts above.

## 3. The textual form

One thing per line. `embcc inspect ir FILE.c` prints it;
`embcc inspect ir FILE.ir` reads it back.

```
; EmbIR
func @h ;decl
data @g ;decl

func @f nparams=1 nvars=3 vregs=19 labels=3 {
  local v0 size=4 align=4 intptr scalar
  local v1 size=4 align=4 intptr scalar
  local v2 size=4 align=4 intptr scalar
  %3 = const.4s 0	; 3:21
  stvar:4s v1, %3	; 3:19
L0:
  %5 = ldvar.4:4s v2	; 3:36
  %7 = cmp.4s lt %5, %6	; 3:36
  brz.4s %7 -> L2	; 3:32
  %10 = call @h(%9)	; 3:48
  ret %11	; 3:60
}
```

**Declarations.** `func @name` and `data @name`, with flags (`defined`,
`weak`, `varargs`, `sret`, `nothrow`), one per symbol the unit mentions.
`strN = "..."` for each string literal.

**A function header** carries its shape: `nparams`, `nvars`, `vregs`,
`labels`, and optionally `scratch`, `outgoing`, plus the flags `static`,
`varargs`, `alloca`, `i128`.

**`local vN`** describes one frame slot — `size`, `align`, optional
`user_align`, and the flags `volatile`, `ldouble`, `int128`, `intptr`,
`scalar`. This is what a stack-layout question always comes down to.

**Operands.** `%N` a virtual register, `vN` a frame slot, `LN` a label,
`[%N]` an address, `@name` a symbol, `#N` a folded immediate operand,
`strN` a string.

**Suffixes** read left to right as `.<result width>:<memory width>` then
flags: `s` signed, `f` floating point, `v` volatile. So `ldvar.4:4s` is a
sign-extending 4-byte load into a 4-byte result, and `ldvar.8:4s` widens to
eight. An absent width means the instruction has none — it is not a
default.

**A trailing `; line:col`** is the source position; `; line N` where only a
line is known; `; compiler-synthesized` for an instruction that corresponds
to no source construct.

## 4. Provenance (R3)

Every instruction carries a line and a column, stamped per *expression* so
`a[i] + b[j]` blames the right subscript. **The verifier enforces it**: an
instruction with no location and no `synth` mark fails the compile under
`EMBCC_VERIFY`, which the whole test suite sets.

That rule is not decorative. Turning it on found five passes silently
dropping locations — the inliner's parameter stores and exit jump, the
out-of-SSA phi copies, the trampoline blocks, mem2reg's entry seed, and
SCCP's replacement jump. A line table with a hole still links, so nothing
else could have noticed.

The `synth` mark is the §9.1 exception, and it is a *mark*, not an absence:
the verifier has to be able to tell a deliberate case from a pass that
forgot.

## 5. The round-trip

> §9.1: *"Round-trip textual form (print → parse → identical IR). This makes
> every pass testable in isolation with text-in/text-out tests."*

The check is that **printing a parsed unit reproduces the text it was parsed
from, byte for byte**. That proves the form is a lossless encoding of
everything it claims to carry, and it fails the moment the printer emits
something the parser cannot read back — which is the drift the requirement
exists to prevent.

[`tests/golden/ir-roundtrip.sh`](../../tests/golden/ir-roundtrip.sh) runs it
over EmbCC's own 55 sources at `-O0` and `-O2`: **110 round-trips, all
identical**. `-O2` matters, because that is where the inliner, mem2reg,
SCCP and the out-of-SSA rebuild all construct instructions.

## 6. Remarks (R2)

Passes record what they decided, where they decided it — see
[`docs/tools/diagnostics.md`](../tools/diagnostics.md). `inline`, `mem2reg`,
`sccp`, the per-function optimizer summary and the x86-64 register allocator
are producers. CSE, DCE, copy propagation, load elimination and store
forwarding are not yet: they run inside a fixpoint where `changed` doubles
as loop control, so counting them means restructuring the loop.

## 7. Verifier

`EMBCC_VERIFY=1` checks, after irgen and again after optimization:

- every read is of a defined virtual register;
- `var_scope` ranges are in range for the current instruction count — a pass
  that renumbers instructions without remapping them silently breaks
  local-slot coalescing, which is how one miscompile got in;
- every instruction has a location, or says it is synthesized (§4).

## 8. Known gaps

- **No `mir`, `cfg` or `callgraph` dump.** §18 lists them.
- **The parsed unit is not code-generated.** The parser exists to test
  passes; producing an object from `.ir` would need the DWARF boundary in §2
  resolved or explicitly skipped.
- **The type graph** (§2) — the one thing the round-trip cannot reconstruct.
