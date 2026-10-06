# Register allocation

This page describes the register allocator that every EmbCC backend
shares (`src/arch/regalloc.c` and `src/arch/regalloc.h`), the other
machine-independent pieces that live beside it (stack-slot sharing, the
parallel move, the memory-offset fold), and the hooks through which each
backend describes its registers. It is written for people changing
EmbCC. The IR passes that run before allocation are described in
[The optimizer](optimizer.md); how each backend uses the result is in
[Backends](backends.md).

## Overview

The allocator is a graph-colouring allocator in the Chaitin-Briggs
style: backward liveness over the IR, a precise interference graph,
conservative coalescing, Chaitin's simplify order with optimistic
colouring, and colour preferences from copies and from the ABI. It works
on vreg numbers and register numbers and knows nothing about any
machine. A backend describes its machine in a `struct ra_target`, calls
`ra_allocate` (and `ra_allocate_fp` for a floating-point class), and
reads the result as an array indexed by vreg: the register a vreg lives
in, or -1 for a value that stays in its stack slot.

There is no spill code in the usual sense. A value that does not get a
register lives in its frame slot for its whole life, and the backend
reads and writes it there through scratch registers. Live-range
splitting is done before allocation, in the IR, by `pass_splitloops`
(see [The optimizer](optimizer.md#last)).

### When it runs

The driver passes each backend a `regalloc` flag that is true at `-O1`
and above, `-Os` and `-Oz` included (`opt_level >= 1`), and on Thumb,
RISC-V and AVR at `-O0` as well. There the backend pins every source
variable to its slot as under `-g` (`g_t_o0`, `g_rv_o0`, `g_a_o0`:
`ra_debug_pin_vars`; AVR ORs it into `avr_excl`'s map) and turns off
what `-g` turns off -- tail calls, folded offsets, and on Thumb and RV32
the second pair-allocation attempt -- so only the temporaries of
expressions get registers. `EMBCC_O0_NORA=1` restores the old `-O0`. On
x86-64 and AArch64 at `-O0` every vreg lives in a stack slot. Within an allocating build a backend may still
leave a particular function unallocated; the conditions are listed per
backend under [Backend hooks](#backend-hooks).

## The algorithm

Both register classes go through one function, `ra_allocate_class`.
`ra_allocate` calls it for the integer class and `ra_allocate_fp` for the
floating-point class; the classes differ only in which vregs are
eligible, which pool they are coloured from, and which registers survive
a call.

### 1. The pool

`pool_for(fn, &n)` (or `fp_pool_for`) returns the registers this
function may use, in order of preference. It is a callback because the
answer depends on the function: a variadic function, a function with an
atomic or a divide, or a function that needs a register for a fixed
purpose gets a different pool. Put caller-saved registers first, so that
a short-lived value takes one and the prologue saves nothing for it.

`RA_MAXPOOL` (24) is the most registers a pool may hold. It sizes the
per-colour arrays and is bounded at 31 by the `int` bitmasks used in
colouring.

`EMBCC_RA_MAXPOOL=N` truncates the integer pool to its first `N`
registers (a value of `N` at least the pool size has no effect; `0`
leaves no registers). See [Stress testing](#stress-testing-with-embcc_ra_maxpool).

### 2. Liveness

`ra_live_intervals` computes per-instruction live-in and live-out sets
by backward dataflow to a fixpoint. An instruction's successors are the
next instruction (except after `IR_JMP`, `IR_RET`, `IR_UD2` and
`IR_SWITCH`), the target of `IR_JMP`, `IR_BRZ` and `IR_BRNZ`, and the
default and every table entry of `IR_SWITCH`. What an instruction reads
comes from `ra_each_use` and what it defines from `ra_ins_def`; these
are the shared operand switches, and they must agree with what the
backends actually load and store.

It also returns `first[v]` and `last[v]`, the lowest and highest
instruction index at which `v` is live. Because they come from the
dataflow and not from where `v` appears, a value carried around a loop
is live over the whole loop body.

The results are shared: slot coalescing, the pair passes and
`ra_reserve` use the same numbering.

### 3. Values that cross a call

A vreg is marked as crossing a call when it is live out of an
`IR_CALL`, or out of an instruction for which the backend's
`op_calls_helper` says yes (an operation lowered to a runtime-helper call
that the IR does not show as a call: soft-float arithmetic, a 64-bit
divide on a 32-bit machine, `__int128` arithmetic, binary128 `long
double`). A call's own result does not cross that call. A crossing value
may take only a register for which `is_callee_saved` (or
`is_fp_callee_saved`) is true. `saved_only(fn)` lets a backend add vregs
to this set for its own reasons.

### 4. Eligibility

A temp is eligible. A local (including a parameter) is eligible when it
is a scalar integer or pointer, or a scalar float on a backend with
`float_in_gpr` or `fp_reads_gpr`, of 1, 2, 4 or 8 bytes. In the floating-point class,
exactly the vregs in `fltmap` (from `cg_float_vregs`) are eligible.

A vreg is then made ineligible, and stays in memory, when:

- it is marked in `wide` (a value too large for a register: a `long
  double`, an `__int128`, a vector, or on a 32-bit target a 64-bit value
  that the pair pass handles);
- it is in `fltmap` and this is the integer class (a value belongs to one
  class only);
- in the integer class, it is an operand or result of a floating-point
  operation, including the integer side of a conversion, unless the
  backend sets `float_in_gpr` or `fp_reads_gpr`;
- it is the operand of `IR_ADDR` or `IR_VA_START`, the result of
  `IR_FRAMEADDR`, or an operand of `IR_CAS16`;
- it is an operand of `IR_XCHG`, `IR_XADD`, `IR_ARMW`, `IR_CMPXCHG` or
  `IR_CAS` and the backend does not set `atomic_in_reg`;
- it is an address operand of `IR_MEMCPY` or `IR_MEMZERO` and the backend
  does not set `memcpy_addr_in_reg`;
- it is returned by `IR_RET` from a function returning a struct, or is a
  scalar return value and the backend does not set `ret_scalar_in_reg`;
- it is the target of an indirect call (argument setup could overwrite
  it), a struct argument or result, an integer argument the backend
  cannot read from a register (`call_int_arg_in_reg`), or, in the integer
  class, a float argument or result (unless `float_in_gpr`);
- it is an operand or the result of inline asm, or is live across an
  `IR_ASM` (the asm's clobbers are not visible here) -- unless the
  backend sets `asm_in_reg`, which says its asm operands are values it
  moves itself and its asm cannot touch a callee-saved register. Then,
  in the integer class only, its operands and result are ordinary
  values, and a value live across it (or an output's address, read after
  the template) may not take a register in the asm's `clob` mask -- or,
  when the mask is 0, is treated as crossing a call. A continuation
  (`ir_asm.cont`) constrains nothing itself;
- it is never live.

The three `..._in_reg` flags and `atomic_in_reg` say what a backend has
been taught to read from a register. A backend that reads an operand
from its stack slot must leave the flag at 0, or the allocator will put
the value in a register and the backend will read a slot nothing wrote.

### 5. The interference graph

Edges are added Chaitin's way. At each instruction that defines an
eligible vreg, the definition interferes with every eligible vreg live
after the instruction, including when the definition itself is dead
(it still writes its register). There are two exceptions:

- For a copy, the source does not interfere with the destination,
  because both hold the same value. A copy is `IR_MOV`, an `IR_LDVAR`
  for which `ldvar_plain(size, sign, w)` says the load is a plain
  register move on this machine, an `IR_EXT` for which `ext_plain` says
  the same, and an `IR_STVAR` of 8 bytes or more.
- Values live into the first instruction (parameters, and anything read
  before it is written) have no definition to find them, so they are made
  a clique.

A value dying at an instruction can share a register with the value
defined there.

### 6. Preferences

A preference edge joins two vregs that do not interfere and would save
a move by sharing a register:

- the two ends of a copy, as defined above;
- the destination and first operand of ADD, SUB, MUL, DIV, AND, OR, XOR,
  SHL and SHR, and also the second operand of ADD, MUL, AND, OR and XOR
  when it is a register.

`abi_hints(fn, hint)` fills a per-vreg register the ABI would like:
a parameter's incoming register, a call's result and a returned value in
the return register, a call argument in its argument register. A hint
is a preference, not a constraint.

### 7. Coalescing

Before colouring, the ends of a copy that do not interfere are merged
into one node, so they cannot be coloured differently. When the backend
sets `alu_dst_is_lhs` (a two-operand machine, where `d = a op b` is
`d = a; d op= b`), the destination and first operand of an ALU operation
are merged the same way. Only copies of the whole value are merged; a
narrowing copy must still be emitted.

The test is Briggs's: merge only when the combined node has fewer than
`NP` (the pool size) neighbours of significant degree. It is one pass
over the instructions, not iterated. A merged node crosses a call if any
member does, and takes the first hint any member has.

One merge is refused: when one node crosses a call and the other does
not and lives deeper in a loop. That is the shape `pass_splitloops`
creates on purpose, and merging it would put the loop's value back in a
callee-saved register.

### 8. Spill cost and simplify order

Each node's cost is the number of its reads and writes, each weighted by
`4^depth`, where the depth is the number of loops around the instruction
(capped at 5) and a loop is the span from a backward branch's target to
the branch.

The simplify phase repeatedly removes a node whose remaining degree is
less than the number of registers it may take: `NP`, or for a node that
crosses a call the number of callee-saved registers in the pool, since
colouring offers it no others. When none remains, it removes the node
with the lowest `cost / degree^2`. Ties go to the lowest node index, so
the result is deterministic. `EMBCC_RA_DEGREE_SPILL=1` selects the
highest-degree node instead, and `EMBCC_RA_POOL_K=1` counts the whole
pool for call-crossing nodes too, each for bisecting a difference to
that choice.

### 9. Colouring

Nodes are coloured in the reverse of the order they were removed, so a
node removed as a spill candidate may still find a free register. For
each node:

1. `taken` is the set of pool registers held by already-coloured
   neighbours, the registers reserved for pairs over any part of the
   node's live range (see [Pairs](#pairs-on-32-bit-and-8-bit-targets)),
   and, if the node crosses a call, every caller-saved register.
2. `want` is the set of free registers held by a coloured preference
   partner, plus the node's hint if it is free.
3. On a three-operand machine (`alu_dst_is_lhs` is 0) a free hint is
   taken first, so that an operand preference cannot outvote a
   parameter's own register. Then, on either kind of machine, the first
   register of `want` in pool order is taken.
4. Otherwise the first free register in pool order is taken, skipping
   any register that an uncoloured interfering neighbour has as its hint,
   unless avoiding it would mean taking a callee-saved register when the
   first free register is caller-saved.
5. With no free register, the node is not coloured and its vregs live in
   memory.

Merged vregs take the colour of their node.

### 10. What the backend gets back

`ra_allocate` returns the per-vreg register array and fills `used_out`
with the callee-saved registers the function uses, for the prologue to
save. A pool register that an inline-asm operand names explicitly is
counted as used, because the asm writes it whether or not the allocator
handed it out.

When values do not get a register, the allocator records a
`regalloc/spilled-to-stack` remark ("N of M values did not get one of
the K allocatable registers"), shown by `-fremarks`.

## The floating-point class

`ra_allocate_fp` runs the same algorithm over the vregs in `fltmap`
(`cg_float_vregs`: floats and doubles, never `long double`), with
`fp_pool_for` and `is_fp_callee_saved`. A backend with no FP pool
returns `NULL` from it. The integer pass excludes those vregs, so a value
has one home.

A soft-float backend sets `float_in_gpr` instead: a float is then bits
in an integer register, eligible in the integer class like an int, and
its arithmetic is helper calls that `op_calls_helper` reports.

A value that floating-point and integer operations both touch is in the
integer class (`cg_float_vregs` takes it back at the first integer use),
and fdlibm's are: the double whose words `EXTRACT_WORDS` reads, the one
`INSERT_WORDS` builds. A backend that sets `fp_reads_gpr` (AArch64) can
reach such a value's general-register home from its floating-point
lowering -- `fld_slot`, `fst_slot`, `frd`, `fwrote` and `fmove` move it
across with one `fmov` -- so the integer class allocates it like an int,
where without the flag it stays in memory and every crossing is a store
and a load. A float argument of a call and a call's float result stay
in memory even so: the argument setup writes x0-x7, which may be such a
value's home. A double parameter whose home is a general register is
moved there after the integer parameters, and before the v registers
are permuted (the prologue's `pgfmv` list).

AArch64 decides such a value's class by its uses instead
(`cg_float_vregs_by_cost`), because its integer lowering can also reach
an FP-register home: `ld_slot`, `rd`, `rd_ext`, `st_slot`, `wrote` and
`wrote_n` fmov to and from one. An integer use an fmov cannot serve -- an
address, a narrow access, an integer result narrower than eight bytes --
still decides for the integer class; every other integer use is a vote,
each floating-point use is a vote the other way, the votes are pooled
over the copies (one value), and the float class wins ties. fdlibm's
`x`, read by a dozen float operations and by the shift that takes its
high word, lives in a d register and crosses once. x86-64 keeps the
rule above: any integer use decides.

`cg_float_vregs` decides the classes from each instruction's operands as
`ra_each_use` lists them. It read the `a`, `b` and `c` fields of every
op, and a two-operand op leaves `c` at 0, so one `and #imm` or `ext`
anywhere in a function took vreg 0 -- the first parameter -- out of the
float class.

## Stack slots

The allocator decides which values need no slot; the backends lay out
frames. Three shared helpers connect the two:

- **`ra_coalesce_temps(fn, nvars, slots, &npool)`** assigns each temp an
  index in a pool of shared slots. A temp in a register (`slots->loc` or
  `slots->floc`) gets no slot. With `opt_frames` set (x86-64 and
  AArch64 set it at `-O1` and above, the embedded backends at `-O2`), a
  temp that appears in no instruction gets none. Temps are
  visited in order of first appearance; a temp whose first and last
  appearance are in one basic block reuses a slot freed by a temp that is
  already dead, and any other temp gets a slot of its own. With
  `has_cgoto` set, nothing is shared.
- **`ra_locals_referenced(fn, want_debug)`** reports which locals any
  instruction still names (parameters always count; under `-g`, in a
  variadic function, and with `alloca`, every local counts). A local that
  nothing names needs no slot; SROA leaves such locals behind.
- **`ra_slot_dead(fn, loc, floc, v, want_debug)`** says that local `v`
  needs no slot because it lives in a register of the right class, is a
  scalar of at most 8 bytes, and its address is never taken. It is false
  in a variadic function, with `alloca`, and under `-g`.

## Pairs on 32-bit and 8-bit targets

On Thumb, RV32 and MIPS32 a 64-bit value (a `long long`, or a `double`
on a soft-float target) needs two registers. Those backends run a separate
**pair pass** before the integer pass: `ra_allocate` with a pool whose
entries are the low registers of aligned pairs, over only the 64-bit
vregs. Each allocated pair is then registered with `ra_reserve(ranges,
n)`, a list of `{reg, first, last}`: during the next `ra_allocate` call,
both registers of each pair count as taken for any node whose live range
overlaps `[first, last]`, and are free elsewhere. The list is consumed
by that call.

`ra_narrow_hishift(fn)` marks a 64-bit shift right by 32 to 63 whose
every reader uses at most four bytes. Such a value is the source's high
word and needs one register, not a pair.

AVR uses the shared allocator differently: a "register" is the low
register of a run of 2, 4 or 8 bytes, and the backend calls the allocator
once per run width. See [AVR](#avr).

## The parallel move

`ra_parallel_move(dst, src, n, scratch, out_dst, out_src, max)` orders
a set of simultaneous register moves `dst[i] <- src[i]` into a sequence
that is safe to execute top to bottom. It repeatedly emits a move whose
destination no pending move still reads; when only cycles remain, it
copies one source to `scratch` and redirects the moves that read it.
Moves with `dst == src` are dropped. It returns the number of moves, or
-1 if `max` is too small or a destination appears twice (a caller bug).
`n + 1` output entries are always enough. `scratch` must be a register
that is no destination and holds nothing live.

The Thumb, RISC-V, MIPS32 and AVR backends use it for a call's argument setup,
for placing incoming parameters in their allocated registers, for the
operands of runtime-helper calls, and (on AVR) for moving an indirect
call's target into Z. x86-64 (`emit_reg_parallel_move`,
`emit_fp_parallel_move`) and AArch64 (`a64_parallel_move`,
`a64_fp_parallel_move`) have their own routines.

`tests/golden/parallel-move.sh` builds `tools/pmovecheck` and checks the
routine by simulation: every mapping of up to five destinations drawn
from five registers is executed against a model register file, and every
destination must end up holding its source's original value.

## The memory-offset fold

`ra_fold_memoff(fn, lo, hi, w_addr, max_size, wide)` runs before
allocation on targets with base-plus-offset addressing. An `ADD` of a
constant whose every use is the address of an integer load or store of
at most `max_size` bytes is removed, and the accesses take its base and
add the constant to `ir_ins.memoff`, when every resulting offset lies in
`[lo, hi - size]`. The base must have one definition, or every access
must follow the `ADD` in the same block with no write to the base in
between (the shape of an unrolled loop's `[p+4]`, `[p+8]`). Wide stores
and loads are excluded.

| Backend | Call |
|---|---|
| AArch64 | `ra_fold_memoff(fn, -256, 4095, 8, 8, wide)` |
| Thumb | `ra_fold_memoff(fn, 0, 4095, 4, 4, wide)` |
| RISC-V | `ra_fold_memoff(fn, -2048, 2047, XLEN/8, XLEN/8, wide)` |
| MIPS32 | `ra_fold_memoff(fn, -32768, 32759, 4, 4, wide)` |
| AVR | `ra_fold_memoff(fn, 0, 64, 2, 4, wide)` |

`EMBCC_NO_MEMOFF` turns it off on RISC-V, MIPS32 and AVR (and on Thumb; see its
section). x86-64 folds addresses during instruction selection instead.

## Debug information

Under `-g`, the Thumb, RISC-V and MIPS32 backends pass `ra_debug_pin_vars(fn)` as
the `fltmap` argument of `ra_allocate`, which keeps every source variable
in its frame slot so that the `DW_AT_location` naming the slot is
correct. Temporaries are still allocated. AVR does not allocate under
`-g`. x86-64 and AArch64 allocate as usual under `-g` and keep every
local's slot (`ra_slot_dead` is false).

## Backend hooks

Each backend defines one `struct ra_target` (two on Thumb, RISC-V and
MIPS32, whose pair passes use a second one).

| Field | x86-64 `X86_RA` | AArch64 `A64_RA` | Thumb `THUMB_RA` | RISC-V `RISCV_RA` | MIPS32 `MIPS_RATGT` | AVR `AVR_RA` |
|---|---|---|---|---|---|---|
| `call_int_arg_in_reg` | 1 | 1 | 1 | 1 | 1 | 1 |
| `ret_scalar_in_reg` | 1 | 1 | 1 | 1 | 1 | 1 |
| `memcpy_addr_in_reg` | 1 | 0 | 0 | 1 | 1 | 0 |
| `atomic_in_reg` | 0 | 0 | 1 | 1 | 1 | 0 |
| `alu_dst_is_lhs` | 1 | 0 | 0 | 0 | 0 | 0 |
| `float_in_gpr` | 0 | 0 | 1 | 1 | 1 | 1 |
| `fp_reads_gpr` | 0 | 1 | 0 | 0 | 0 | 0 |
| `asm_in_reg` | 0 | 0 | 1 (not the pair pass) | 1 (not the pair pass) | 0 | 0 |
| `ldvar_plain` | size 8, or size 4 not sign-extended to 8 | as x86-64 | size 4 at width 4 | full register width; also a sign-extending 4-byte read at RV64 | size 4 at width 4 | size equals width, or size at most 2 |
| `op_calls_helper` | `__int128` operations | binary128 `long double` and `__int128` operations | floating-point arithmetic, comparisons and conversions not executed by the FPU; 64-bit divide and remainder | floating-point arithmetic, comparisons and conversions; 64-bit divide and remainder at RV32 | floating-point arithmetic, comparisons and conversions; 64-bit divide and remainder | float operations, conversions, divide, remainder, multiply except by a small constant |
| FP class | xmm0-xmm6 | v18-v31, v0-v7 | s16-s31, with an FPU | none | none | none |
| FP callee-saved | none | none | all | no FP class | no FP class | no FP class |
| `saved_only` | none | none | none | none | none | `a_saved_only` |
| `ext_plain` | none | none | none | none | none | `a_ext_plain` |

### x86-64

**Pool** (`x86_pool_for`). The base pool depends on the function, and
caller-saved registers come first:

| Function | Pool |
|---|---|
| variadic | r10, r11, rbx, r12-r15 |
| contains an atomic | r8-r11, rbx, r12-r15 (rsi holds `&expected` and rdx the desired value in `cmpxchg`) |
| contains an integer divide or remainder | rsi, r8-r11, rbx, r12-r15 (`idiv` writes rdx:rax) |
| otherwise | rsi, rdx, r8-r11, rbx, r12-r15 |

rdi is added after the leading rsi/rdx (or first, in the atomic pool)
when the function is not variadic, does not return a struct, makes no
call that returns one, and has no `__int128` operation. The largest pool
is twelve registers. Callee-saved: rbx and r12-r15.

**Floating point.** xmm0-xmm6. None is callee-saved under System V, so a
float live across a call stays in memory. The Microsoft ABI makes
xmm6-xmm15 callee-saved and EmbCC does not save them on the Windows
target; the `-Wwindows-abi` warning, issued on every Windows compile,
names rsi, rdi, xmm6 and xmm7 as not preserved.

**Scratch.** rax and rcx are in no pool: rax is the accumulator and the
parallel-move cycle breaker, rcx the second operand, the shift count and
the jump-table base. xmm7 is the float scratch and xmm15 a second one;
xmm8-xmm13 hold the vectorizer's accumulators and widening values.

**Hints** (`x86_abi_hints`, not under Win64 or in a variadic function):
an integer parameter its System V argument register, a float parameter
its xmm register, a float result and a float return value xmm0, and each
scalar call argument its argument register. Integer results are not
hinted, because rax is not in the pool.

**Not allocated.** A function containing `IR_IGOTO` or `IR_LABELADDR`,
or with exception regions, keeps every value in memory.

**Two-operand.** `alu_dst_is_lhs` is 1, so the destination and first
operand of an ALU operation are coalesced when they do not interfere.

**Prologue moves.** Incoming parameters are placed with the backend's
own `emit_reg_parallel_move` (cycles broken through rax) and
`emit_fp_parallel_move` (through xmm7).

### AArch64

**Pool** (`a64_pool_for`):

| Function | Pool |
|---|---|
| variadic, no atomic | x13, x14, x15, x20-x28 |
| variadic, with an atomic | x15, x20-x28 |
| with an atomic | x15, x0-x7, x20-x28 |
| with `alloca` | x13, x14, x15, x0-x7, x20-x28 |
| otherwise | x13, x14, x15, x0-x7, x19, x20-x28 |

Callee-saved: x19-x28. x19 is left out when the function uses `alloca`,
because it then holds the frame base. The atomic lowerings use x13 and
x14.

**Floating point.** v18-v31, then v0-v7. v8-v15 are not in the pool,
because AAPCS64 preserves their low halves and each would need a save
and an unwind rule; so no FP pool register is callee-saved, and a float
live across a call stays in memory. v16 and v17 are the float scratch.

**Scratch.** x9 (accumulator), x10 (second operand), x11 (addresses,
indirect call target), x12 (far offsets, parallel-move cycle breaker,
jump-table entry, `stxr` status). x8 is the indirect result register and
is never in a pool; x16 and x17 are the pointers of the block-copy loop;
x18 (the platform register) is never used.

**Hints** (`a64_abi_hints`): a scalar parameter its argument register,
a call's scalar result and a returned value x0 (or v0), each scalar call
argument its argument register.

**Helper calls.** `a64_op_calls_helper` is true for every binary128
`long double` operation (arithmetic, comparison, conversion, and the
loads, stores and moves of 16-byte values) and every `__int128`
operation. Some of these call libgcc (`__addtf3`, `__multi3`,
`__divti3` and the others) and some are inline; the predicate answers for
both families as a whole, which costs at most a register for a value
that did not need a callee-saved one, where the opposite error would let
a helper clobber a live register.

**Not allocated.** A function with exception regions, `IR_IGOTO` or
`IR_LABELADDR`.

**Order.** `ra_fold_memoff(fn, -256, 4095, 8, 8, wide)`, then
`ra_allocate`, then `ra_allocate_fp`. Parameters and call arguments are
placed with the backend's own parallel-move routines (scratch x12 and
v17).

### Thumb

**Pool** (`t_pool_for`):

| Function | Pool |
|---|---|
| default | r0-r8 |
| variadic | r4-r8 (the prologue pushes r0-r3 for `va_arg`) |
| with `alloca` | r0-r6, r8 (r7 is the frame base) |
| variadic with `alloca` | r4, r5, r6, r8 |

Callee-saved: r4-r11.

**Extended pool** (`g_t_ext`). Each function is also generated with
r9-r11 added to the pool (r0-r11, and r4-r11 when variadic) and with
the pairs r8:r9 and r10:r11 added to the pair pool. The three scratch
roles below are then not fixed: `t_roles_from` gives TMP, ADDR and SCR
the registers of r11, r10, r9 (in that order of preference) that
`t_busy` finds free -- not live into or out of the instruction or the
two after it, not read or written by them, and in the prologue not a
parameter's home. An attempt in which a role finds no register sets
`g_t_role_fail` and is dropped. `EMBCC_T_EXT=0` turns the extended
attempts off; `EMBCC_T_EXT=1` keeps one whenever it succeeds (the exec
tests run that way, tests/golden/thumb-ext-pool.sh).

**Floating point.** With an FPU (`target_thumb_fpu()`), s16-s31, all
callee-saved and saved with `vpush`/`vpop`. Without one the FP pool is
empty and floats are bits in core registers (`float_in_gpr`). Only
single-precision values the FPU computes with, float arguments and
results, and float locals are in the FP class (`t_float_map`); doubles
are never in it. s0 and s1 are the VFP scratch.

**Scratch.** r9 (`T_SCR`, also the parallel-move cycle breaker), r10
(`T_ADDR`), r11 (`T_TMP`) and r12 (`T_ACC`) -- the first three per
instruction in the extended pool above. Four are needed because a
64-bit operation holds both halves of both operands. r9-r11 are
callee-saved, so every use is recorded through `t_scr()` and the prologue
saves only the ones the body used. lr holds the `strex` status in atomic
loops.

**Low scratch.** Many 16-bit Thumb encodings take only r0-r7.
`lo_free` finds a low register that holds no live value at the current
instruction and the next (r0-r3, and r4-r7 when the function already
saves them), and the operand accessors compute in it.
`EMBCC_T_NOLO` turns this off.

**Hints** (`t_abi_hints`): a scalar parameter of at most 4 bytes its
argument register; a scalar return value and a call's scalar result r0;
the operands of a soft-float helper r0 and r1 and its result r0; each
call argument its argument register.

**Pairs.** Without an FPU, `long long` and `double` values get register
pairs from a pair pass (`THUMB_PAIR_RA`) before the integer pass. The
pairs are r0:r1, r2:r3, r4:r5 and r6:r7 (a variadic function uses only
r4:r5 and r6:r7; a function with `alloca` loses r6:r7). Each pair is
reserved by live range with `ra_reserve`; a pair that dies at a 64-bit
compare marks its range `born`, so the compare's 0 or 1 may take its
registers (cmp64 reads both operands before set_cc writes the result),
and `return a < b` computes the result in r0. Wide locals accessed at a size
other than 8 and a variadic function's parameters stay in memory.
`gen_func_best` compiles each function with and without the pair pass,
with and without `t_lowregs`, and in the extended pool, and keeps the
shortest result (the earlier attempt on a tie); it compiles once, with
pairs, when the allocator is off, under `-g`, or with an FPU. `EMBCC_T_PAIRS` forces the choice and
`EMBCC_T_PAIRS_ONLY=FUNC` uses pairs only in the named function.

**Inline asm** (`asm_in_reg`). Its operands come from r0-r3 and r12, and
irgen refuses a template or clobber naming r4-r11. irgen records in
`ir_asm.clob` what each asm may change -- its operand registers, its
clobber list, the registers its template names, the scratch its outputs
are stored through (`ir_asm.scr`), and r0-r3, r12 and lr when the
template contains `bl`, `blx` or `svc` -- and a value live across it
keeps out of exactly those. An operand pinned to r6 or r7 by the letters `S`
and `D` takes that register out of the function's pool and is saved by
the prologue (`t_asm_saved_regs`). The lowering moves register-resident
inputs into place as one parallel move, and its value outputs -- the
instruction's `dst` and any continuations after it -- out the same way.

**Debug.** Under `-g` the source variables are pinned to their slots
(`ra_debug_pin_vars`, merged with the FP map), and there is no memory
offset folding and no tail call.

**Other.** `ra_fold_memoff(fn, 0, 4095, 4, 4, wide)` runs before
allocation (not under `-g`; `EMBCC_NO_MEMOFF` turns it off).
`EMBCC_T_RA_MAX=N` sends every vreg numbered `N` or higher back to
memory, for bisecting a miscompile to one value.

### RISC-V

**Pool** (`rv_pool_for`): a0-a7, t3, s1, s2-s11, twenty registers. A
variadic function's pool is t3, s1, s2-s11 (the prologue spills a0-a7).
At RV32, a function with any 64-bit operation loses t3, which is then the
scratch for the high word of a second operand. s0 is never in the pool:
it is the frame pointer by convention and the frame base in a function
with `alloca`. Callee-saved: s0-s11.

**Floating point.** The backend is soft-float. There is no FP class;
`float_in_gpr` puts floats and doubles in integer registers, and
`rv_op_calls_helper` reports their arithmetic, comparisons and
conversions as helper calls.

**Scratch.** t0 and t1 (first operand, low and high word), t2 and t3
(second operand), t4 (`SCR`, the parallel-move cycle breaker), t5, and t6
(`FAR`), which is used only to build `sp` plus an offset too large for an
immediate.

**Hints** (`rv_abi_hints`): a single-register scalar parameter its
argument register, a returned value and a call's result a0, a helper
operation's operands a0 and a1 and its result a0, and each call argument
its argument register.

**Pairs.** At RV32, 64-bit values get register pairs from a pair pass
(`RV_PAIR_RA`) before the integer pass: a0:a1, a2:a3, a4:a5, a6:a7,
s2:s3, s4:s5, s6:s7, s8:s9, s10:s11 (a variadic function uses only the
s-pairs), reserved by live range with `ra_reserve`. `gen_func_best`
compiles each function with and without pairs and keeps the shorter
(without on a tie). `EMBCC_RV_PAIRS` forces the choice and
`EMBCC_RV_PAIRS_ONLY=FUNC` limits pairs to one function. At RV64 the
integer pass allocates 64-bit values directly.

**Inline asm** (`asm_in_reg`), as on Thumb: operands come from t0-t6 and
a0-a7, irgen refuses s0-s11 in a template or clobber list and records
each asm's `clob` (adding ra, t0-t6 and a0-a7 when the template calls),
and the lowering moves operands in and value outputs out as parallel
moves whose cycle breaker is a backend scratch (t4, t2, t1, t0 or t5) no
operand of that asm uses.

**Debug.** Under `-g` source variables are pinned to their slots, and
there is no memory-offset folding and no tail call.

**Other.** `ra_fold_memoff(fn, -2048, 2047, XLEN/8, XLEN/8, wide)` runs
before allocation (`EMBCC_NO_MEMOFF` turns it off). `EMBCC_RV_RA_MAX=N`
sends every vreg numbered `N` or higher back to memory.

### MIPS32

**Pool** (`mips_pool_for`): v0, v1, a0-a3, t7, t8, s0-s7, sixteen
registers. A variadic function's pool leaves out a0-a3 (the prologue
spills them into the caller's home area, where `va_arg` walks them).
Callee-saved: s0-s7, and fp, which is never in the pool: it is the frame
base in a function with `alloca`, saved like any callee-saved register.
t9 is the register an indirect call goes through and `$at` holds a
comparison's result for the branch after it; neither is allocated.

**Floating point.** The backend is soft-float, as RISC-V: no FP class,
`float_in_gpr`, and `mips_op_calls_helper` reports the arithmetic,
comparisons and conversions as helper calls, with 64-bit divide and
remainder.

**Scratch.** t0 and t1 (first operand, low and high word), t2 and t3
(second operand), t4 (`SCR`, the parallel-move cycle breaker), t5
(`SCR2`), and t6 (`FAR`), which builds `sp` plus an offset too large for
a 16-bit immediate.

**Hints** (`mips_abi_hints`): a single-register scalar parameter its
argument register, a returned value and a call's result v0, a helper
operation's operands a0 and a1 and its result v0, and each call argument
its argument register.

**Pairs.** 64-bit values get register pairs from a pair pass
(`MIPS_PAIR_RA`, `mips_pair_pool_for`) before the integer pass: a0:a1,
a2:a3, v0:v1, s0:s1, s2:s3, s4:s5, s6:s7 (a variadic function starts at
v0:v1), with o32's 64-bit arguments hinted to their pair. `gen_func_best`
compiles each function with and without pairs and keeps the shorter
(without on a tie). `EMBCC_MIPS_PAIRS=0` or `=1` forces the choice.

**Debug.** Under `-g` source variables are pinned to their slots, and
there is no memory-offset folding and no tail call.

**Other.** `ra_fold_memoff(fn, -32768, 32759, 4, 4, wide)` runs before
allocation, leaving room under the 16-bit limit for an `lwl`/`lwr`
pair's `+3` (`EMBCC_NO_MEMOFF` turns it off). `EMBCC_MIPS_RA_MAX=N` sends
every vreg numbered `N` or higher back to memory.

### AVR

AVR's registers are 8 bits wide, an `int` is 2 bytes and the IR computes
at 4. The backend uses the shared allocator, but a "register" in its
pools is the low register of a run.

**The register plan.**

| Registers | Use |
|---|---|
| r0 | scratch (`R_TMP`), SREG save, parallel-move cycle breaker |
| r1 | always zero (`R_ZERO`) |
| r2-r17 | homes, call-saved; pushed by the function that uses them |
| r18-r21 | bank A, the accumulator; part of the eight-byte home at r18 |
| r22-r25 | bank B, the second operand; also homes (r22, r24, the quad at r22) |
| r26:r27 (X) | scratch pointer, or a home for a value that crosses no call |
| r28:r29 (Y) | frame pointer |
| r30:r31 (Z) | address scratch; never a home |

**Passes per attempt** (`avr_ra_try`). The allocator runs once for each
run width, each pass with its own pool (`avr_ra_pass`):

| Width | Pool |
|---|---|
| 8 bytes | r18-r25 (call-clobbered), then r10-r17 and r2-r9 |
| 4 bytes | r22-r25 (call-clobbered), then r2, r6, r10, r14 |
| 2 bytes | r24, r22, X (call-clobbered), then r2, r4, ..., r16 |

The call-clobbered entries in bank B are offered only while homes there
are allowed (`EMBCC_AVR_NO_VOL` removes them), and X only while it is
offered as a home.

A call-saved run must lie entirely below the lowest argument register
any call in the function loads (`avr_arg_low`), and the number of
call-saved pairs is limited by a budget (`g_a_cap`). A run handed out in
one pass is withdrawn from the later ones. `ext_plain` makes a 2-byte
extension a copy in the pair pass, so a parameter's pair reaches the
values made from it. `avr_excl` keeps out values read wider than the
pass's width, rematerialised constants and the operands of `IR_SELECT`.

**Trying modes and keeping the shortest** (`gen_func_best`). The
eight-byte pass runs first; the order of the other two is the mode:

| Mode | Order |
|---|---|
| `AVR_RA_QUADS_FIRST` | 4-byte, then 2-byte |
| `AVR_RA_PAIRS_FIRST` | 2-byte, then 4-byte |
| `AVR_RA_PAIRS_ONLY` | 2-byte only |

Each mode is tried with pair budgets of 8, 3 and 1, nine attempts in all.
Each attempt generates the whole function (with branch relaxation) and
measures its length; the shortest wins, and on a tie the earliest
attempt. A discarded attempt is undone by truncating the code buffer and
the relocation-site lists. If the winner was not the last attempt
generated, it is generated again. Because any of the losing modes could
be chosen for another function, tests force each mode
(`EMBCC_AVR_RA_MODE`) so that a miscompile in one of them is not hidden.

**Proof from the emitted bytes.** After each IR instruction, `a_verify`
decodes the bytes just emitted with `avr_insn_writes` and checks that no
register holding a value that is still live (other than the
destination) was written, and that no operand's home was overwritten.
A conflict in r22-r27 marks the value `saved_only` (call-saved registers
only) for the next attempt; any other conflict takes its home away. The
attempt is then regenerated, up to `2 * nvregs + 2` times, after which it
is an internal error: `avr: FUNC: values kept being overwritten in their
homes after N attempts`. An attempt that gave X to a value and then used
X as scratch is redone without X as a home.

**Not allocated.** Under `-g`, in an interrupt
handler, and in every function but the one named by
`EMBCC_AVR_RA_ONLY` when that is set.

**Other.** A temp whose single definition is a constant gets no home and
no slot when allocating; it is rebuilt where it is read.
`ra_fold_memoff(fn, 0, 64, 2, 4, wide)` runs first (`EMBCC_NO_MEMOFF`
turns it off).

**Knobs.** All are read through `avr_knob`, which treats an empty value
as unset.

| Variable | Effect |
|---|---|
| `EMBCC_AVR_RA` | trace homes and sizes to stderr |
| `EMBCC_AVR_RA_MODE=N` | try only mode `N` (1, 2 or 3) |
| `EMBCC_AVR_RA_CAP=K` | use a single pair budget `K` |
| `EMBCC_AVR_RA_ONLY=FUNC` | allocate only in `FUNC` |
| `EMBCC_AVR_RA_LIMIT=K` | keep only the first `K` homes |
| `EMBCC_AVR_NO_VOL` | no homes in bank B |
| `EMBCC_AVR_NO_OCT` | no eight-byte homes |
| `EMBCC_AVR_NO_XHOME` | never use X as a home |
| `EMBCC_AVR_REMAT_MAX=K` | rematerialise only the first `K` constants |

## Debugging and stress testing

| Variable | Effect |
|---|---|
| `EMBCC_RA_MAXPOOL=N` | use only the first `N` registers of the integer pool |
| `EMBCC_RA_WHY=1` | for each function, print to stderr one line per value left in memory, with the rule that excluded it and how many instructions touch it (`ra-why FUNC RULE COUNT`), and a summary when values were spilled (`ra-spill int\|fp FUNC N of E eligible, pool NP`) |
| `EMBCC_RA_TRACE=1` | print each function's pool and, for every eligible or allocated vreg, whether it crosses a call, its hint, its register and its live range |
| `EMBCC_RA_DEGREE_SPILL=1` | choose spill candidates by highest degree instead of lowest cost per degree squared |
| `EMBCC_RA_POOL_K=1` | treat a call-crossing node as colourable below the whole pool's size, not the callee-saved count |

The rule names `EMBCC_RA_WHY` prints are the IR opcode that made a value
ineligible, or one of `local-odd-size`, `local-not-scalar`, `16-byte`,
`float-class`, `live-across-asm` and `no-rule`.

Example:

```sh
EMBCC_RA_TRACE=1 embcc -O2 --target=riscv64-unknown-elf -c t.c
```

```text
ra sum pool(20): 10 11 12 13 14 15 16 17 28 9 18 19 20 21 22 23 24 25 26 27 nins=45 E=30
ra sum v0 cross=0 hint=10 loc=10 [0,6]
```

### Stress testing with `EMBCC_RA_MAXPOOL`

With a full pool, the paths in each backend that handle an operand
living in a stack slot (the spilled-operand paths) run only in the few
functions that run out of registers, so a lowering that clobbers a
scratch register on such a path can go unnoticed. Shrinking the pool
forces those paths everywhere. After changing a backend's scratch
registers, its pool, or its spilled-operand handling, run the execution
tests with the variable set in their environment, for several small
values:

```sh
# x86-64 and AArch64: the compiled-and-run corpus
EMBCC_RA_MAXPOOL=2 tests/run.sh --exec-only
EMBCC_RA_MAXPOOL=2 tests/run.sh --target=aarch64-elf --exec-only
# the embedded targets: their execution goldens, from the repository root
EMBCC=$PWD/embcc EMBCC_VERIFY=1 EMBCC_RA_MAXPOOL=2 sh tests/golden/thumb-exec.sh
EMBCC=$PWD/embcc EMBCC_VERIFY=1 EMBCC_RA_MAXPOOL=2 sh tests/golden/riscv-exec.sh
EMBCC=$PWD/embcc EMBCC_VERIFY=1 EMBCC_RA_MAXPOOL=2 sh tests/golden/mips-exec.sh
```

The variable affects the shared integer pool only. AVR's run widths and
the pair passes' pools are separate; their own knobs are listed in the
backend sections.

### Tests

- `tests/golden/regalloc-O2.sh` compiles every program in `tests/exec/`
  at `-O2`, runs it, and requires the same exit status and output as
  gcc's build of the same program.
- `tests/golden/parallel-move.sh` checks `ra_parallel_move` exhaustively
  (above).
- `tools/x86-identity.sh` compiles a corpus with a baseline revision and
  the working tree and requires byte-identical x86-64 objects, for
  changes to shared code that must not alter x86-64 output.
- `tests/golden/avr-calleesave.sh` and `tests/golden/thumb-calleesave.sh`
  check that callee-saved registers survive calls.

## Changing the allocator

- An IR operation that reads or defines a vreg must be in `ra_each_use`
  and `ra_ins_def`; a terminator must also be in the successor logic of
  `ra_live_intervals` and the block-end lists of `ra_coalesce_temps` and
  `ra_fold_memoff`. See
  [Adding an IR operation](optimizer.md#adding-an-ir-operation).
- A new opaque use (a lowering that reads a vreg from its slot) must make
  the vreg ineligible here, or be gated by a flag in `struct ra_target`
  so each backend can say what it supports.
- Measure the effect on code size for all six targets, not one; the
  heuristics (spill choice, coalescing test, hint order) trade against
  each other differently on two-operand and three-operand machines.
