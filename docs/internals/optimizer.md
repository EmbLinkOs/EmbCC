# The optimizer

This page describes `src/opt/opt.c`, the IR-to-IR optimizer that runs
between IR generation and code generation. It covers the order in which
the passes run, what each pass does and what it requires, the analyses
they share, how the `-O` levels and `-f<pass>` flags select them, the IR
verifier, and what to do when you add a pass or an IR operation. It is
written for people changing EmbCC. The user-facing description of the
optimization levels is in [Optimization](../manual/optimization.md); the
IR itself is described in [EmbIR](ir.md).

The optimizer is purely IR to IR. Register allocation, stack-slot
sharing and instruction selection belong to the backends; see
[Register allocation](register-allocation.md) and
[Backends](backends.md).

## Where the optimizer runs

The driver (`src/driver/main.c`) lowers the translation unit to EmbIR
with `irgen`, then calls `opt_run(iu, level)` once for the whole unit,
then hands the unit to the backend selected by `--target=`.

| Command line | `opt_run` level | Backend `optimize` | Backend `regalloc` |
|---|---|---|---|
| `-O0` (default) | 0: nothing runs | 0 | 0 |
| `-O`, `-O1` | 1 | 1 | 0 |
| `-O2` | 2 | 1 | 1 |
| `-O3` | 3, which the optimizer treats as 2 | 1 | 1 |
| `-Os`, `-Oz` | `OPT_SIZE` (-2): level 2 with the size-trading passes off | 1 | 1 |

Any other spelling after `-O` is rejected with
`embcc: unknown optimization flag '-Ox'`.

At level 0 `opt_run` returns immediately, so `-O0` output does not depend
on anything in this file. The self-hosting fixed point is built at
`-O0` and relies on that.

`-Os` reaches the optimizer as `OPT_SIZE` and sets the file-scope flag
`g_opt_size`. The backends never see a negative level: the driver passes
them `opt_level` (2 for `-Os`) and separately calls
`target_set_opt_size()`, which the backends consult only for function
alignment.

After `opt_run` returns, and only at `-O1` and above, the driver removes
`static` functions that no root reaches (roots are non-`static`
functions, constructors and destructors, `__attribute__((used))`
functions, targets named by top-level asm, and functions whose address
appears in a static initializer). This is not part of `opt.c`, but it is
what deletes a callee that the inliner absorbed.

### Inspecting the result

`embcc inspect ir` prints the IR as it stands at the `-O` level given on
the same command line, so the effect of the optimizer is the difference
between two runs:

```sh
embcc inspect ir -O0 file.c > before.ir
embcc inspect ir -O2 file.c > after.ir
diff before.ir after.ir
```

`embcc inspect cfg` prints each function's basic blocks, predecessors,
successors, immediate dominators and back edges. It is produced by
`opt_cfg_dump`, which uses the same `build_cfg` and dominator code the
passes use.

`-fremarks` (or `-fremarks=json`) prints the decisions passes record with
`remark_add`, for example:

```text
t1.c:1: remark: promoted-to-register 's' [mem2reg/scalar-and-never-addressed]
t1.c:1: remark: vectorized 'sum': 4 lanes of 4 bytes, with a scalar remainder (a sum reduction) [opt/vec/runtime-trip-count]
t1.c:1: remark: optimized 'sum': 31 instructions -> 68 [opt/fixpoint-reached]
```

The passes that run many times inside the fixpoint (value numbering,
global CSE, copy propagation, dead code, load reuse, dead stores,
if-conversion, PRE) do not emit a remark per rewrite. They increment
counters in `g_did`, and `opt_func` emits one `opt/pass-counts` remark per
function at the end.

## Properties of the IR the passes rely on

The passes depend on the following invariants of EmbIR. A change that
breaks one of them breaks passes far away from the change.

- **Locals and temporaries share one numbering.** Vregs `0 .. nvars-1`
  are frame slots (the first `nparams` of them are parameters); temps
  start at `nvars`. `IR_LDVAR`, `IR_ADDR` (operand `a`) and `IR_STVAR`
  (`dst`) are the only operands that name a slot.
- **Temps are normally single-assignment.** Every op listed in
  `writes_temp()` writes a fresh temp, and most passes reason only about
  a temp whose `compute_defs` count is exactly one. Some temps have
  several definitions on purpose: mem2reg's phi copies, PRE's insertions,
  the values the join-copy pass merges. Code that treats a temp as a known
  value must check that it has exactly one definition.
- **A pass renames temps, never slots.** A rewrite that changes a slot
  number in `IR_STVAR`'s `dst` turns a store to one variable into a store
  to another.
- **`IR_LANDING` defines two temps** (`dst` and `b`). `compute_defs`,
  `pass_dce` and the inliner's `remap_ins` handle it explicitly;
  `def_target()` reports only one.
- **Immediates appear only after `pass_immfold`.** Inside the pipeline a
  constant operand is a separate `IR_CONST` temp. After immediate folding,
  `imm_b` is set and `imm` holds the constant for ADD, SUB, MUL, AND, OR,
  XOR, CMP, SHL and SHR. Code that runs before the fold reads constants
  with `get_const()`; `const_b()` accepts either form.
- **Every instruction has a source location** (`line`, `col`) or is
  marked `synth`. A pass that builds an instruction to replace another
  copies the location of the one it replaces.
- **`var_scope_lo` / `var_scope_hi` index the instruction array.** A pass
  that renumbers instructions either remaps them (`remap_scopes`, given a
  `newpos` table) or frees them, which makes every local take its own
  frame slot.
- **Fresh instructions start blank.** `ib_push()` returns an instruction
  whose `dst`, `a`, `b` and `label` are -1, because vreg 0 is a real
  local. The pointer it returns is into a buffer that `xrealloc` may move,
  so it must not be held across another `ib_push()` on the same buffer.

## Shared analyses

| Analysis | Functions | Used by |
|---|---|---|
| Definition counts | `compute_defs` (count and sole defining instruction per vreg; parameters count as defined once) | nearly every pass |
| Use visitors | `each_read` (every vreg operand read), `each_label` (every label operand, including a switch's table), `value_opnds`, `ins_reads` | every pass that counts uses or rewrites operands |
| Known zero bits | `known_zero` (follows up to four single-definition steps through MOV, SHL, SHR, AND, OR, XOR, MUL by a constant and zero-extension) | `pass_fold` (an AND whose mask covers only bits the other operand can never set) |
| Control-flow graph | `build_cfg`, `compute_rpo` | every global and loop pass, `opt_cfg_dump` |
| Dominators | `compute_idom` (Cooper-Harvey-Kennedy), `bb_dominates` | mem2reg, global CSE, PRE, the loop passes, live-range splitting |
| Dominance frontiers | `compute_df` | mem2reg |
| Natural loops | a back edge is an edge whose target dominates its source; `loop_body` collects the blocks | rotation, LICM, idiom recognition, vectorization, strength reduction, unrolling, live-range splitting |
| Alias analysis | `mem_base`, `may_alias` | dead-store elimination, load CSE |
| Available expressions | inside `pass_loadcse` and `pre_one` | load CSE, PRE |
| Liveness | instruction liveness by marking (`pass_dce`); block liveness inside `pass_splitloops` and `pass_joincopies` | those passes |
| Function attributes | `infer_attrs` (unit-wide, `-O2`) | DCE, DSE, load CSE |
| Read-only globals | `ro_globals` (unit-wide, `-O2`) | `pass_roload` |

**The CFG.** A block starts at a label and after every branch, jump,
return, `IR_UD2`, `IR_SWITCH` and `IR_IGOTO`. An `IR_SWITCH` has an edge
to its default label and to every table entry. An `IR_IGOTO` (computed
`goto`) has an edge to every label whose address is taken in the
function, which is the complete set of places it can land. Exception
landing pads are not modelled: a pad is entered from every call in its
region and the CFG records none of those edges.

**Alias analysis.** `mem_base` walks back through address arithmetic to
the object an address is based on: a named global, a numbered frame slot,
or unknown. `may_alias` answers no for two different globals, two
different slots, a global and a slot, and an unknown pointer against a
slot whose address is never taken in the function. Everything else may
alias. There is no type-based aliasing (EmbIR loads carry no type), no
`restrict`, and no field sensitivity.

**Function attributes.** `infer_attrs` starts by assuming every defined
function writes and reads no caller-visible memory, then retracts the
assumption from any function that stores through a pointer, performs a
memory-writing atomic, fence, `memcpy`, inline asm or `va_start`, loads
through a pointer (for "reads"), or calls a function that is not known
clean. An indirect call is not clean. The iteration is capped at 32
rounds. The results are `func.inf_no_write` (gcc's `pure`) and
`func.inf_no_read` (with `inf_no_write`, gcc's `const`), reported with
`-fremarks` as `opt/attr/pure` and `opt/attr/const`.

**Ranges.** There is no value-range analysis. The only interval
reasoning is local to `pass_rangecheck`, where `bound_of` turns a
comparison against a constant into an interval.

## The passes by name

Seventeen passes can be switched individually. They are listed in the
`g_pass[]` table, which is the single list read by `-f<name>`,
`-fno-<name>` and the `-O` levels. `opt_set_pass()` marks a pass
`forced`, and `pass_default()` does not override a forced pass, so an
explicit flag wins over the level in either order on the command line. A
flag can also turn on a pass at a level that leaves it off, for example
`-O1 -fgcse`.

| Name | Pass functions | On at | Off at `-Os` |
|---|---|---|---|
| `mem2reg` | `drop_unreachable`, `pass_mem2reg` | `-O2` | no |
| `gcse` | `pass_gcse` | `-O2` | no |
| `load-cse` | `pass_loadcse` | `-O2` | no |
| `sccp` | `pass_sccp` | `-O2` | no |
| `licm` | `pass_rotate`, `pass_licm`, `pass_ivsr`, `pass_splitloops` | `-O2` | `pass_splitloops` only |
| `vectorize` | `pass_vectorize` | `-O2`, x86-64 only | yes |
| `inline` | `inline_unit` | `-O2` | no (smaller budget) |
| `dse` | `pass_dse` | `-O2` | no |
| `div-magic` | `pass_divmagic` | `-O2` | no |
| `if-convert` | `pass_ifconv` | `-O2` | no |
| `cfg-clean` | `pass_thread`, `pass_cfgclean` | `-O1` | no |
| `tail-recursion` | `pass_tailrec` | `-O2` | no |
| `idiom` | `pass_idiom` | `-O2` | no |
| `sroa` | `pass_sroa` | `-O2` | no |
| `unroll` | `pass_unroll` | `-O2` | yes |
| `pre` | `pass_pre` | `-O2` | no |
| `switch-thread` | `pass_swthread` | `-O2` | yes |

A name that is not in the table is not a pass flag, and the driver
reports `embcc: error: unknown argument '-fno-NAME'`.

The remaining passes have no flag. They run at `-O1` and above:
`pass_storefwd`, `pass_roload` (it has work only at `-O2`, where
`ro_globals` runs), `pass_fold`, `pass_reassoc`, `pass_lvn`,
`pass_idxoff` (RISC-V only), `pass_divmod`, `pass_copyprop`, `pass_copyprop_local`, `pass_dce`,
`pass_rangecheck`, `pass_punfwd`, `pass_immfold`, `pass_joincopies`,
`pass_sinkconst` and `pass_sinkaddr`.

## The pipeline

### Unit-level steps

`opt_run` sets the pass defaults for the level and then, in order:

1. `inline_unit`, if `inline` is on.
2. `infer_attrs`, at level 2. It runs after inlining so that it sees the
   bodies that will be compiled.
3. `ro_globals`, at level 2; at level 1 the read-only list is emptied.
4. `opt_func` for every function of the unit, in order.

### Gates inside a function

`opt_func` computes two conditions before it runs anything:

- `cfg_ok` is true when the function has no exception regions
  (`fn->neh == 0`). `mark_eh_calls` in irgen clears `neh` when no call in
  any region can throw, so a function whose regions cannot be entered is
  optimized like any other. Without `cfg_ok`, the passes that trust the
  CFG to be complete (mem2reg, global CSE, SCCP, DSE, load CSE, join-copy
  coalescing) are skipped. The block-local passes still run, because a
  landing pad starts with a label and is its own block to them.
- `edge_ok` is `cfg_ok` and no `IR_IGOTO` in the function. Without it,
  every pass that places code on an edge or opens a block is skipped:
  tail recursion, mem2reg, PRE, rotation, LICM, idiom recognition,
  vectorization, if-conversion, strength reduction, unrolling, switch
  threading and live-range splitting. An edge out of `goto *p` cannot be
  split, because there is no block between the jump and its target.

### The order

This is `opt_func`, with each pass's flag and gate:

```text
verify("irgen")                               if EMBCC_VERIFY is set

pass_tailrec                                  tail-recursion, edge_ok
pass_divmagic                                 div-magic
pass_sroa                                     sroa
drop_unreachable; pass_mem2reg                mem2reg, cfg_ok, no IR_IGOTO
pass_sroa; pass_mem2reg if SROA changed       sroa and mem2reg, cfg_ok, no IR_IGOTO

repeat (outer round, at most 100 times):
    repeat (inner fixpoint, at most OPT_MAX_ROUNDS = 64 times):
        pass_storefwd
        pass_roload
        pass_fold
        pass_reassoc
        pass_lvn
        pass_idxoff                           RISC-V only
        pass_divmod
        pass_gcse                             gcse, cfg_ok
        pass_sccp                             sccp, cfg_ok
        pass_copyprop
        pass_copyprop_local
        pass_dce
        pass_thread; pass_cfgclean            cfg-clean
        pass_dse                              dse, cfg_ok
        pass_rangecheck
    until no pass reports a change

    pass_punfwd       -> copyprop, dce
    pass_loadcse      -> copyprop, dce        load-cse, cfg_ok
    pass_pre          -> copyprop, dce        pre, edge_ok
    pass_rotate       -> copyprop_local, dce  licm, edge_ok
    pass_licm         -> copyprop, dce        licm, edge_ok
    pass_idiom        -> dce                  idiom, edge_ok
    pass_vectorize    -> dce                  vectorize, edge_ok
until none of these reports a change

pass_ifconv       -> copyprop_local, dce      if-convert, edge_ok
pass_ivsr         -> copyprop_local, dce      licm, edge_ok
pass_unroll                                   unroll, edge_ok
pass_swthread                                 switch-thread, edge_ok
if either of the last two changed anything, repeat (at most 100 times):
    pass_fold, pass_reassoc, pass_lvn, pass_copyprop,
    pass_copyprop_local, pass_dce,
    pass_cfgclean                             cfg-clean
    pass_sccp                                 sccp, cfg_ok
until no change

pass_immfold      -> dce
pass_joincopies   -> cfgclean if cfg-clean    cfg_ok
pass_sinkconst
pass_splitloops, repeated (at most 64 times)  licm, edge_ok, not -Os,
                                              not EMBCC_NO_SPLITLOOPS
pass_sinkaddr

verify("opt")                                 if EMBCC_VERIFY is set
remarks: opt/fixpoint-reached, opt/pass-counts
```

An arrow means the listed cleanup runs only when the pass reported a
change, and the change also starts another outer round.

The placement of a pass follows from what it does:

- **Inner fixpoint.** Cheap passes that work on the existing blocks and
  enable one another. They run to a fixpoint every outer round.
- **Outer round.** Passes that rebuild the instruction array or are
  expensive (a CFG and a dataflow per call). Each is followed by the
  cleanup it needs, and a change sends the function back through the
  inner fixpoint.
- **After the rounds.** Passes that destroy a shape another pass matches.
  Strength reduction turns `base + i*scale` into a walking pointer, which
  the vectorizer can no longer recognise, so it runs once after
  vectorization has settled. Unrolling and switch threading leave copies
  that no longer look like loops. If-conversion matches the diamond that
  phi destruction leaves, and needs the loop passes to have stopped moving
  blocks.
- **Last.** Passes that decide where a value is materialised rather than
  what it is: immediate folding, constant sinking, live-range splitting,
  address sinking. Running them inside the fixpoint would let other passes
  undo them, and they would keep reporting changes.

### Iteration caps and non-convergence

Every loop in `opt_func` is bounded:

| Loop | Cap | When the cap is reached |
|---|---|---|
| Inner fixpoint | `OPT_MAX_ROUNDS` (64) | `opt_no_fixpoint` reports it |
| Outer rounds | 100 | stops silently |
| Cleanup after unrolling and switch threading | 100 | stops silently |
| `pass_splitloops` | 64 | stops silently |

Many passes also bound their own internal loops (for example
`pass_ifconv` 64 rewrites, `pass_vectorize` 32 loops, `pass_unroll` 16
loops).

`opt_no_fixpoint` runs at the start of the 64th inner round. Under
`EMBCC_VERIFY` it is fatal:

```text
internal: the optimizer did not converge on 'NAME' after 64 rounds -- two passes are undoing each other's work
```

Without `EMBCC_VERIFY` it prints a warning and compilation continues,
because every round leaves the function correct:

```text
embcc: warning: the optimizer stopped after 64 rounds on 'NAME' without converging (the code is correct; this is a compiler performance bug worth reporting)
```

A real fixpoint takes a few rounds. Reaching the cap means two passes
are undoing each other's work, and the test suite (which sets
`EMBCC_VERIFY`) fails on it.

## The passes

Each entry gives the purpose, the conditions for the rewrite, and the
analyses used.

### Unit-level

**`inline_unit`** (`inline`). Replaces a direct call to a function
defined in the same unit with the callee's body. The callee's parameters
and locals become caller locals; the caller's temps are renumbered
upward to make room, and the callee's labels and jump tables are copied
with an offset. Each parameter is bound with an `IR_STVAR` of the
argument, and each `IR_RET` becomes a `MOV` of the result and a jump to a
shared exit label. Budgets, in IR instructions:

| Constant | Value | Meaning |
|---|---|---|
| `INLINE_MAX_CALLEE` | 24 | callee size at `-O2` |
| `INLINE_SIZE_CALLEE` | 6 | callee size at `-Os` |
| `INLINE_SOLE_CALLEE` | 2000 | callee size when it is a `static` function with exactly one direct call and its address is never taken (the body moves rather than being copied) |
| `INLINE_MAX_CALLER` | 800 | stop inlining into a caller past this size |
| `INLINE_MAX_PER_FUNC` | 64 | inlines per caller |

`__attribute__((always_inline))` overrides the size budget and nothing
else; `__attribute__((noinline))` refuses. A refusal is recorded as an
`inline/not-inlined` remark with one of these reasons:
`not-a-sole-callee-at-O1` (at `-O1` the inliner takes only a `static`
callee with one caller and an `always_inline` one; `g_inline_o1`),
`callee-not-defined-here`, `would-be-recursive`,
`callee-computes-in-__int128`, `callee-is-noinline`, `callee-is-weak`,
`callee-is-varargs`,
`callee-too-large`, `callee-has-exception-regions`, `returns-a-struct`,
`parameter-is-not-a-simple-scalar`, `returns-a-value-wider-than-a-vreg`,
`callee-uses-va_start`, `callee-has-a-vla`,
`callee-uses-a-computed-goto` and
`callee-calls-a-struct-returning-function`. A caller with exception
regions or `__int128` arithmetic is not inlined into.

### Before the rounds

**`pass_tailrec`** (`tail-recursion`; `edge_ok`). Turns `return f(args)`,
where `f` is the function itself, into parameter assignments and a jump
to the top. The assignments read every argument first and then write
every parameter, so `return f(b, a)` is correct. It refuses varargs
functions, functions with `alloca` or exception regions, and functions
that take the address of a parameter. It runs first so that the loop it
creates is seen by mem2reg and the loop passes.

**`pass_divmagic`** (`div-magic`). Replaces a 32-bit division or
remainder by a constant with a multiply by a magic number and shifts
(Hacker's Delight, chapter 10), using the low half of a 64-bit multiply
as the high half of the 32-bit one. It does nothing on a target whose
pointer is narrower than 8 bytes (Thumb, RV32, AVR); Cortex-M has
hardware `sdiv`/`udiv`, and the sequence would need a 64-bit multiply.
Unsigned division by a power of two is already a shift (`pass_fold`);
signed powers of two take this path.

**`pass_sroa`** (`sroa`). Scalar replacement of aggregates. A local
qualifies when every use of `addr L` is the address of a load or store,
directly or through an add of a constant; no access or the local itself
is volatile; and the accessed `(offset, size)` pieces lie inside the
object and do not overlap. It then becomes one new scalar local per
piece (at most `SROA_MAX_FIELDS`, 16, for an object of at most
`SROA_MAX_SIZE`, 128 bytes) and each access an `IR_LDVAR`/`IR_STVAR`.
Scalars whose address is taken qualify too. Parameters do not, because
the prologue writes them. New locals are appended, so every temp is
renumbered upward. It runs a second time after mem2reg, because mem2reg
can turn a pointer variable that hid the object into a temp.

**`drop_unreachable`** and **`pass_mem2reg`** (`mem2reg`; `cfg_ok`, no
`IR_IGOTO`). SSA construction and destruction for locals. Unreachable
blocks are dropped first, because the dominator tree is incomplete over
them. A local is promoted when it is a scalar integer, pointer or float
of 4 or 8 bytes (or an integer or pointer of 1 or 2 bytes whose every
write covers all of it), is not volatile, is never the operand of
`IR_ADDR`, and every read is full-width and plain. Parameters are
promoted; their initial value is the incoming one. The pass inserts phi
functions at the iterated dominance frontier, renames down the dominator
tree, and destroys SSA by placing each phi as copies on its incoming
edges: a trampoline block for a branch-taken edge, inline copies on a
fall-through, appended copies on a single-successor edge. Each copy set
is read-all-then-write-all through fresh temps. If the function's first
instruction is a label that something branches to, an explicit jump gives
the entry its own block first. Rejections are reported with
`-fremarks` as `mem2reg/kept-in-memory` with the reason (`type-unknown`,
`not-a-scalar-integer-pointer-or-float`, `not-4-or-8-bytes`,
`declared-volatile`, `address-is-taken`, `read-is-volatile`,
`read-is-partial-or-extending`, `write-is-volatile`,
`write-is-partial`).

### The inner fixpoint

**`pass_storefwd`**. Within a block, forwards the value of
`IR_STVAR L, v` to later `IR_LDVAR L` of a local whose address is never
taken, when the load is plain at the stored width (size 8, or size 4
unless sign-extending to 8). Stops at the next store to `L` and at a
label. A volatile local, or a store or load marked `vol`, is never
forwarded: each read of it happens and reads memory.

**`pass_roload`**. Replaces a load from a static global proven read-only
by `ro_globals` with the constant stored there. A global qualifies when
every use of its address, in every function of the unit, is a load
address (directly or at a constant offset), no initializer points at it,
and no inline or top-level asm could reach it. Bytes that a relocation
fills are left alone. A float constant becomes its bit pattern.

**`pass_fold`**. Constant folding at the operation's width and
signedness, folding of conversions between constants, algebraic
identities (`x+0`, `x*1`, `x*0`, `x&0` and others), and strength
reduction: `x*2^k` to a shift, unsigned `x/2^k` to a shift, unsigned
`x%2^k` to a mask. An AND whose mask keeps only bits that `known_zero`
proves zero becomes zero. A copy of a constant defined in another block
becomes the constant (not on AVR). Folds that assume 64-bit arithmetic
refuse a 128-bit (`w == 16`) operation individually.

Inside one block, a temp is also known to hold the constant last written
to it, even when it has other definitions elsewhere (`lk_note`): a
loop-carried temp has one on every edge into its loop, so the
single-definition rule never knows it. Only a 32- or 64-bit compare uses
that knowledge, and only when the constant was written at the compare's
width. The table is forgotten at every label and control transfer, after
inline `asm` or a landing pad, and for a temp written by anything but a
constant or a copy of a known value. This is what decides a rotated
loop's guard when the loop's start and bound are constants.
`EMBCC_NO_LKCONST=1` turns it off, here and in `pass_cfgclean`.

Floating-point operations on constants are folded by `fold_fp`: `+`,
`-`, `*`, `/`, negation and the six comparisons on `float` (`w` 4) and
`double` (`w` 8) operands, which arrive as bit patterns. Each is
computed in the operation's own format with one rounding, in the
default rounding mode (EmbCC does not honour `FENV_ACCESS` and keeps no
exception flags), and the result replaces the instruction as an integer
`IR_CONST` holding its bits; a comparison gives 0 or 1. Division by zero
folds to the infinity the machine produces. Nothing is folded when an
operand or the result is a NaN, because which NaN an invalid operation
produces, and how a payload propagates, differ between machines.
`long double` (`w` 16) is never folded.

**`pass_reassoc`**. `(x op c1) op c2` becomes `x op (c1 op c2)` when both
operations are the same kind and both other operands are constants. Only
constants are moved. The inner operation stays if something else reads
it.

**`pass_idxoff`** (RISC-V only; `EMBCC_NO_IDXOFF=1` turns it off). An
address `base + ((x ± c) << k)` used only by loads and stores becomes
`(base + (x << k)) ± (c << k)`, so the backend folds the constant into
the access's displacement (`ra_fold_memoff`) and value numbering shares
`base + (x << k)` between `a[i - 1]`, `a[i]` and `a[i + 1]`. Every step is
modular at one width, so it holds for every `x`; it needs no extension
between the add and the shift, which leaves `int` indices on 32-bit
targets and `long` ones anywhere. Only when `x ± c` and the shift have
no other use, the displacement is at most 255 bytes, and `k` is at most
3. Thumb, AArch64 and x86-64 scale a register inside the access, where
the rewrite measured slower, so it is not run there.

**`pass_lvn`**. Local value numbering within a block (the table is reset
at every label). Loads and `IR_LDVAR` are keyed with a memory version
that every store and call increments. Volatile accesses are never
numbered.

**`pass_divmod`**. Within a block, when `a / b` and `a % b` (same
operands, width and signedness) both appear, the remainder becomes
`a - q*b`. If the remainder comes first, the quotient is computed there
and the later division becomes a copy. A constant divisor is left to
`pass_divmagic`, except on Thumb (32-bit operations), where nothing turns
a constant divide into a multiply and a remainder is `udiv; mls` either
way: there the pair shares one divide and the remainder is one `mls`
(see `pass_immfold`). `EMBCC_NO_DIVMOD_CONST=1` turns that off.

**`pass_gcse`** (`gcse`; `cfg_ok`). Global value numbering over the
dominator tree: a value computed in a block is available in every block
it dominates. Only arithmetic, comparisons, NEG and BNOT are numbered;
constants, address materialisations, extensions and memory reads are
not, because recomputing them is cheaper than keeping the result live.
A constant operand is keyed by its value. Every operand must be
single-assignment (`vn_stable`). The pass does nothing in a function
with unreachable blocks.

**`pass_sccp`** (`sccp`; `cfg_ok`). Resolves a conditional branch whose
condition is a single-definition `IR_CONST` to a jump (or a
fall-through) and removes every block that is no longer reachable. The
constant half of SCCP is done by `pass_fold` and copy propagation in the
same fixpoint. Each resolved branch is reported as
`sccp/branch-always-jumps` or `sccp/branch-never-jumps`. It frees
`var_scope_lo/hi`. It stays off without `cfg_ok` because a landing pad
looks unreachable.

**`pass_copyprop`**. Replaces uses of the destination of a
single-assignment `MOV` with its source.

**`pass_copyprop_local`**. Copy propagation within one block for copies
whose destination has several definitions, which is what phi destruction
leaves for loop counters and accumulators. The table of current copies is
cleared on every definition and at every block boundary.

**`pass_dce`**. Dead-code elimination by marking. An instruction is live
when it has an effect (a store, branch, label, return, observable call,
volatile `IR_STVAR`, volatile `IR_LDVAR` even when its value is unused,
as in `(void)v;`) or when a live instruction reads what it defines;
everything else is removed, including cycles. A call to a function
inferred to read and write no memory, that cannot throw, whose result is
unused, is removed. DCE remaps `var_scope_lo/hi` as it compacts.

**`pass_thread`** and **`pass_cfgclean`** (`cfg-clean`). `pass_thread`
threads jumps through a merged boolean: when each arm of `a && b` or
`a || b` writes a constant or a comparison into a temp whose only reader
is the branch after the join, each arm jumps directly to its outcome.
`pass_cfgclean` retargets jumps to jumps (following chains up to 16
hops), turns a conditional branch whose target is its fall-through, or
whose two edges reach the same label, into a jump, resolves a second
branch on a condition that the branch just above it already decided
(marking it for removal), decides a branch on a temp that the same block
has just set to a constant (the table `pass_fold` uses; value numbering
gives a guard folded to `1` the name of a counter that starts at 1), rewrites a branch around a jump as one
inverted branch unless the branch or the jump is already marked for
removal, deletes a jump to the label that immediately follows it,
deletes instructions after an unconditional transfer up to the next
label, and removes labels that nothing names, so that the block-local
passes see longer blocks.

**`pass_dse`** (`dse`; `cfg_ok`). Dead-store elimination within a block,
walking backward: a store is removed when a later store to the same
address temp at the same width comes first and no intervening read may
alias it (`may_alias`). Calls to functions inferred to read and write no
memory do not count as reads.

**`pass_rangecheck`**. Not on AVR. Two consecutive compares of the same
value against constants that together test an interval, such as
`c >= '0' && c <= '9'`, become `(unsigned)(c - lo) <= hi - lo` and one
branch. Equality and inequality are not intervals and are left alone.

### The outer round

**`pass_punfwd`**. Not on AVR. Forwarding through a private union or
other object used for type punning: when every use of a local's address
is a load or store address (directly or at a constant offset), a load
that one earlier store in the same block covers becomes that store's
value, reinterpreted, shifted or truncated for a half-width read. A
private local left with no loads loses its stores.

**`pass_loadcse`** (`load-cse`; `cfg_ok`). Global redundant-load
elimination by an available-expressions dataflow. A load is replaced when
an identical load reaches it on every path with no intervening write
that may alias it. The meet requires the same representative temp from
every predecessor, so the reused value has one dominating definition. A
`LOAD` is keyed only when its address temp is single-definition. A store,
`memcpy` or `memzero` with a known base kills only the keys that may
alias it; a call kills everything unless its callee is inferred not to
write memory; inline asm, atomics, `va_start` and fences kill
everything.

**`pass_pre`** (`pre`; `edge_ok`). Partial redundancy elimination for
multiplies. When an expression is computed on some paths into a block and
recomputed in the block, it is inserted at the end of the predecessors
that lack it, so the recomputation becomes a copy. An insertion goes
only into a predecessor whose sole successor is the block; a critical
edge is split first. The result vreg has several definitions, and a
use is replaced when every path has passed a definition under one name.
Operands must be single-assignment; literals and reads of slots that are
never written or addressed are rematerialised; anything else must be
defined in a block that dominates the insertion point. The dataflow is
bounded at `PRE_MAX_KEYS` (400) expressions.

**`pass_rotate`** (`licm`; `edge_ok`). Turns a top-tested loop into a
guard followed by a bottom-tested loop by duplicating the test at the
latch, so each iteration takes one branch, and the header and body become
one block for the block-local passes. One loop per call.

**`pass_licm`** (`licm`; `edge_ok`). Hoists loop-invariant pure
instructions into a preheader that it places immediately before the
header's label: entries from outside the loop are retargeted to the new
label and back edges are not, so no jump is emitted. An instruction moves
when `is_pure` holds (so DIV, MOD and LOAD never move) and every operand
is defined outside the loop or by something already being hoisted.
`IR_LDVAR` moves only when the loop never writes the slot and its address
is never taken. Loops are processed innermost first, one per call. A loop
whose header is fallen into from inside the loop is refused.

**`pass_idiom`** (`idiom`; `edge_ok`). A loop with a constant trip count
that stores a constant into every element, or copies one array to
another, becomes `IR_MEMZERO` or `IR_MEMCPY`. For a copy both bases must
be distinct globals. Nothing else may happen in the body.

**`pass_vectorize`** (`vectorize`; `edge_ok`; on by default for x86-64
only, because the other backends do not lower the vector opcodes). Turns
a rotated single-block loop whose induction variable starts at zero and
steps by one into 16-byte lane-wise operations (`IR_VLOAD`, `IR_VBIN`,
`IR_VSPLAT`, `IR_VSTORE`, `IR_VREDADD`, `IR_VWIDEN`). A constant trip
count that is a multiple of the lane count is rewritten in place. A
runtime trip count gets a vector copy of the loop in front of the
original, which runs the remainder. Requirements: every memory reference
is `base + i*esize` at the same scale; the bases are distinct globals, or
there is exactly one base of any kind; every stored value is a vector
value; every other instruction is pure; nothing computed in the loop is
read after it except the induction variable and a sum reduction (integer
only, including a widening sum). A multiply is vectorized only by a power
of two or by `2^k+1`, as shifts and adds, because SSE2 has no packed
32-bit multiply. `EMBCC_VECDEBUG` prints the reasons for refusals to
stderr.

### After the rounds

**`pass_ifconv`** (`if-convert`; `edge_ok`). The diamond that phi
destruction leaves for `c ? a : b`, where each arm is a single `MOV` of a
value computed before the branch, becomes `IR_SELECT`. Arms that would
need to be evaluated speculatively (a load, anything that can fault) are
not taken, nor are floating-point arms. The arms' width must be 4 or 8
bytes, and so must the branch's `w`, which the select keeps in `size`
as the width its condition is tested at.

**`pass_ivsr`** (`licm`; `edge_ok`). Induction-variable strength
reduction. An address `base + i*scale` (including a widening of `i`, a
shift, or a multiply by a constant, which covers rows of a 2-D array)
with a loop-invariant base becomes a pointer initialised in the
preheader and advanced at the latch. A value read after the loop is not
reduced, because the new pointer ends one step past the old value.

**`pass_unroll`** (`unroll`; `edge_ok`; off at `-Os`). A rotated,
bottom-tested loop whose only control transfer is its back edge, whose
induction variable steps by a constant and is compared against a
loop-invariant bound (`lt`, or `ne` after strength reduction), gets an
unrolled copy in front of it. The copy runs `U` iterations per test
while at least `U` remain and falls into the original loop, which runs the
remainder unchanged. `U` is the largest power of two up to
`UNROLL_MAX_COPIES` (8) with `U * body` at most `UNROLL_BUDGET` (96): 8
copies of a body of up to 12 instructions, 4 of one up to 20
(`UNROLL_MAX_BODY`). The remaining-iteration test is computed
without signed overflow. Loops containing `alloca`, inline asm, a
landing pad or a computed `goto` are refused.

When `unr_trip` proves the trip count constant, the loop is replaced by
that many copies instead, with no test, no branch and no remainder, if
the count is at most `UNROLL_FULL_TRIP` (32) and the copies total at
most `UNROLL_FULL_BODY` (200) instructions. The count is proved from the
induction variable's single definition outside the loop, which must sit
in the block that falls into the header with nothing but the back edge
jumping to the header: `iv = c0` against a constant bound `B` gives
`B - c0`; `iv = X + c1` against a bound `X + c2` (strength reduction's
pointer walk; either side may reach `X` through one more constant add)
gives `(c2 - c1) / step`, when it divides. A body with an operation for
which `target_op_calls_helper()` is true is not copied whole, and
neither is a loop whose test is read after it. Scopes that began or
ended inside the loop are widened to cover all the copies.
`EMBCC_NO_FULLUNROLL=1` turns this off.

**`pass_swthread`** (`switch-thread`; `edge_ok`; off at `-Os`). For a
loop around `switch (state)` whose arms set `state` to known values, the
path from the latch to the switch is copied once per case the state can
arrive at, and each copy jumps directly to that case. What is known is a
forward dataflow over the temps the switch operand is computed from.
Limits: `SWT_MAX_TRACK` 32 temps, `SWT_MAX_PATH` 24 instructions on the
path, `SWT_FEED_DEPTH` 6 latches deep, `SWT_MAX_COPIES` 48 block copies
per switch, `SWT_MAX_ADDED` 400 added instructions per function.

### Last

**`pass_immfold`**. Moves a constant operand of ADD, SUB, MUL, AND, OR,
XOR and CMP into the immediate field (`imm_b`), and a shift count of 0 to
63 into SHL and SHR. A constant in the first operand of a commutative op
or a compare is swapped into place (a compare flips its predicate). The
constant must fit in 32 bits signed and the target must accept it:
`thumb_imm_foldable`, `riscv_imm_foldable` and `a64_imm_foldable` answer
for Thumb, RISC-V and AArch64; x86-64 and AVR accept every 32-bit value.
A 64-bit AND, OR or XOR on Thumb or RV32, which the backend does half by
half, takes any constant whose two halves `thumb_imm_foldable64` or
`riscv_imm_foldable64` accepts. On RV32 that is also any constant one of
whose halves is the identity, because the other half is then at most a
constant built in a register and one instruction.
A constant the target cannot encode stays in a register, where it is
built once and can be hoisted, instead of being rebuilt at every use.
On Thumb, a 32-bit multiply whose only reader is the add or subtract
right after it keeps its constant in a register too (`mla_keeps_reg`),
so the backend fuses the two into `mla` or `mls` rather than lowering the
multiply as shifted adds. `EMBCC_NO_MLAKEEP=1` turns that off.
128-bit operations are not folded.

**`pass_signtest`** (after `pass_immfold`; disabled by
`EMBCC_NO_SIGNTEST=1`). A 64-bit unsigned shift right by 63 whose only
reader is the branch right after it becomes a signed compare with zero:
`if (x >> 63)` is `if (x < 0)`, which a 32-bit target answers from the
high word alone. The branch then tests the compare's result at four
bytes. A 64-bit AND with a constant below 2^32 whose only reader is the
next branch, or an `== 0`/`!= 0` compare, is narrowed to four bytes
along with that reader, because only the low word can be nonzero. So
`if (m & 0x10)` is one `and` and one branch on a 32-bit target, and
`((m >> 52) & 1) == 0` reads only the high word.

**`pass_joincopies`** (`cfg_ok`). Coalesces `%b = mov %a` at a join when
`%a` is a temp read only by that copy, the two have the same width and
class, and they never hold different live values at once (the allocator's
interference test, applied with per-block liveness). Every definition of
`%a` then writes `%b`, the copy disappears, and blocks that held only
copies become empty for `pass_cfgclean`. Functions with inline asm or
exception edges are skipped.

**`pass_sinkconst`**. A constant or address materialisation (`IR_CONST`,
`IR_ADDR`, `IR_GADDR`, `IR_STRADDR`, `IR_FADDR`) with exactly one use
later in the instruction stream moves to just before that use. It moves
only forward. It moves into a deeper loop only when the value costs one
instruction on the target (always on x86-64 and AVR; on AArch64,
Thumb and RISC-V according to the encodable-immediate rules) or under
`-Os`. On RISC-V a nonzero constant whose use is a compare stays out of
a deeper loop whatever it costs: a branch there compares two registers,
so the constant would be rebuilt every iteration, where x86-64 and Arm
have already made it an immediate.

**`pass_splitloops`** (`licm`; `edge_ok`; not `-Os`; disabled by
`EMBCC_NO_SPLITLOOPS=1`). A temp that is live through a loop, crosses a
call outside the loop and no call inside it (other than a call that
receives and returns that very value) gets a new name inside the loop,
copied in on the entry edge and out on every exit edge. The inside name
crosses no call, so the allocator can give it a caller-saved register. A
"call" is an `IR_CALL` or an op for which `target_op_calls_helper()` is
true, the same predicate the allocator uses. Loops with a switch, an
indirect jump, inline asm or a landing pad are skipped. It runs last so
that no copy propagation folds the copies back in.

**`pass_sinkaddr`**. Not on AVR. A single-use chain of address
arithmetic (add, shift or multiply by a constant, widening) defined
earlier in the same block as the load or store it feeds is moved down to
sit directly before the access, so the backends' addressing-mode fusions,
which look only at the immediately preceding instructions, find it. A
chain moves only when no instruction it passes writes one of its
operands.

**`pass_sinkupd`** (`-O2` and `-Os`; disabled by `EMBCC_NO_SINKUPD=1`).
Run per function after `opt_func`, once `pass_latch_copies` and
`pass_thread_copies` have put a loop's back-edge copies into its latch.
An update `d = x OP c` (add, subtract, and, or, xor or a shift by a
constant) whose first reader is the copy `x = mov d` later in the same
block moves down to sit directly before that copy, provided nothing in
between writes `x` or the constant. `b[m++] = c` otherwise leaves `m` and
`m + 1` alive together from the add to the store, so they cannot share a
register and the latch keeps a `mov`; after the move, `m + 1` is born
where `m` dies.

## Target-dependent behaviour

The optimizer is target-neutral except where it asks `src/arch/target.h`:

| Question | Effect |
|---|---|
| `target_get() == TARGET_X86_64` | vectorization defaults on only here |
| `target_ptr_size() < 8` | `pass_divmagic` does nothing; pointer arithmetic built by strength reduction uses the pointer width |
| `target_get() == TARGET_AVR` | `pass_rangecheck`, `pass_punfwd` and `pass_sinkaddr` do nothing; a copy of a constant from another block is not folded |
| `thumb_imm_foldable`, `riscv_imm_foldable`, `a64_imm_foldable` | which constants `pass_immfold` folds |
| `t_imm_ok`, `a64_bitmask_ok` | whether a constant is expensive for `pass_sinkconst` |
| `target_op_calls_helper` | which ops count as calls for `pass_splitloops`; the driver installs the predicate for the target finally chosen, whether it came from `--target=` or the configured default |
| `target_widen_unsigned_fp_cvt` | conversion folding |

## The IR verifier

Setting `EMBCC_VERIFY` (to any value) makes `opt_func` call
`verify_func` twice per function: on the IR from irgen (tag `irgen`) and
on the optimized IR (tag `opt`). The whole test suite sets it. Normal
builds pay nothing. Each check is fatal; the message names the function
and the tag.

| Check | Diagnostic |
|---|---|
| Every temp a surviving instruction reads has a definition | `internal: NAME reads temp %N with no definition (after TAG) — an optimizer pass dropped a value that is still used` |
| Every `IR_SWITCH` has a table | `internal: NAME has a switch with no table (after TAG)` |
| Every label a switch names is placed | `internal: NAME: a switch names label LN, which nothing places (after TAG)` |
| `var_scope_lo/hi` are within the current instruction array | `internal: NAME local N has out-of-range scope [LO,HI] for nins=N (after TAG) — a pass renumbered instructions without remapping var_scope` |
| Every instruction has a source location or is marked `synth` | `internal: NAME instruction N (OP) has no source location after TAG — a pass built it without copying the location of what it replaced; if it corresponds to no source construct, mark it synth` |

Under `EMBCC_VERIFY`, non-convergence of the inner fixpoint is also
fatal (see [Iteration caps](#iteration-caps-and-non-convergence)).

The verifier catches a pass that drops a value still in use. It does not
catch a pass that forgets an operand when counting uses, which shows up
only as wrong output. Run the execution tests for that.

## Debugging environment variables

| Variable | Effect |
|---|---|
| `EMBCC_VERIFY` | run the verifier; make non-convergence fatal |
| `EMBCC_NO_SPLITLOOPS` | skip `pass_splitloops` |
| `EMBCC_VECDEBUG` | print the vectorizer's reasons to stderr |

To find which pass is responsible for a wrong answer, use the pass flags
first: compile with `-fno-<name>` for each named pass in turn, then
compare `embcc inspect ir` output with and without the suspect pass.

## Adding a pass

1. Write `static int pass_NAME(struct ir_func *fn)` that returns nonzero
   exactly when it changed the function. A pass that reports a change
   when it made none prevents convergence.
2. Decide where it runs, using the rules in [The order](#the-order). A
   pass in the inner fixpoint must not undo what another inner pass does.
   A pass in the outer round must be followed by the cleanup it needs and
   must set `outer = 1` when it changes something.
3. Gate it on `cfg_ok` if it trusts the CFG to be complete, and on
   `edge_ok` if it places instructions on an edge or opens a block.
4. If users should be able to switch it, add an entry to `g_pass[]`, a
   `P_` index, a `g_` macro, and a `pass_default()` call in `opt_run` for
   each level. Document the new `-f<name>` in
   [Optimization](../manual/optimization.md).
5. Keep the invariants listed under
   [Properties of the IR](#properties-of-the-ir-the-passes-rely-on):
   copy `line`, `col` and `synth` from the instruction being replaced;
   remap or free `var_scope_lo/hi` when renumbering; build instructions
   with `ib_push` and do not hold the returned pointer across another
   push; rename temps, not slots; count definitions before treating a
   temp as a known value.
6. Count what it does in `g_did` or record a `remark_add` with a stable
   pass and reason name.
7. Run the tests with `EMBCC_VERIFY=1` (the suite does) on every target.
   Check the code size and speed effect on the libraries and benchmarks,
   not only on the test programs.

## Adding an IR operation

The operand and control-flow knowledge of each IR operation is
hand-maintained in switches across the compiler. A new operation, and in
particular a new terminator, has to be added to each of them, or it is
silently mishandled (an operand nothing counts as read is deleted by
DCE; an edge liveness does not see lets a register be reused while live).
To find the sites, search for an existing operation of the same shape:
`IR_SWITCH` and `IR_IGOTO` for terminators, `IR_UD2` for an instruction
with no successor.

In `src/opt/opt.c`:

- `writes_temp`, `is_pure`, `def_target` and `compute_defs` (what it
  defines);
- `each_read` (what it reads) and `each_label` (which labels it names);
- `writes_memory`, `vn_key`, `gcse_numberable`, `lcse_kills_mem` (memory
  effects and value numbering);
- `build_cfg` (block leaders and successors);
- `pass_mem2reg` (its terminator test and per-edge phi copies);
- `pass_cfgclean` (label forwarding, reachability, code after an
  unconditional transfer);
- `pre_tail` and PRE's predecessor retargeting, `licm_one`'s list of
  instructions that leave the loop, `unr_copyable`, the switch-threading
  helpers (`swt_copyable`, `swt_falls`, `swt_from`, `swt_one`),
  `pass_splitloops`, `pass_sinkaddr`;
- the inliner: `inlinable`, `remap_ins` (including the label offset) and
  table copying in `inline_call`;
- `verify_func`.

In `src/arch/regalloc.c`: `ra_ins_def`, `ra_each_use`, the successor
computation in `ra_live_intervals` (a terminator has no fall-through
edge), the block numbering in `ra_coalesce_temps`, and the block-end list
in `ra_fold_memoff`.

In `src/ir/`: the opcode name table in `irprint.c` (`ir_opname`), the
printer and `irparse.c`, so that the textual round-trip
(`tests/golden/ir-roundtrip.sh`) still holds.

In every backend: the main lowering switch (`gen_func` on x86-64 and
AArch64, `gen_ins` on Thumb, RISC-V, MIPS32 and AVR), and any helper that scans
for the operations it fuses or must not cross. A backend that does not
lower an operation must refuse it loudly rather than emit nothing.

Then run with `EMBCC_VERIFY=1`, and make a test that fails when one of
these sites is deliberately left out.
