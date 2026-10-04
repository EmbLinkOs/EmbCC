# Optimization

This page describes the options that control optimization: the `-O`
levels EmbCC accepts and exactly what each one turns on, the `-fNAME` and
`-fno-NAME` options that switch one optimizer pass on or off, the
assumptions the optimizer makes about a program (signed overflow,
aliasing, floating point, `volatile`), the `-fsanitize=` checks, and the
ways to see what the optimizer did. It is written for people who compile
code with EmbCC and want to know what an optimization level costs and
what it promises.

## Overview

Optimization happens in two places.

- The **IR optimizer** (`src/opt/opt.c`) rewrites EmbIR, the compiler's
  intermediate representation, one function at a time, after inlining.
  Its passes iterate until nothing changes. Each pass that has a name can
  be switched on or off with `-fNAME` / `-fno-NAME`.
- The **code generator** of each target (`src/arch/TARGET/codegen.c`)
  changes what it emits by level: register allocation, tail calls,
  frameless functions and function alignment are decided there. These
  have no individual switches.

The default is `-O0`: no IR optimization and no register allocation.

## Optimization levels

| Option | Effect |
|---|---|
| `-O0` | No optimization. The default. |
| `-O`, `-O1` | The local passes, CFG cleanup, removal of unreachable `static` functions, a few code-generation improvements on x86-64 and AArch64. |
| `-O2` | Everything in `-O1`, the global, loop and interprocedural passes, inlining, register allocation and tail calls. |
| `-O3` | Identical to `-O2`. |
| `-Os` | `-O2` without the passes that trade size for speed, a smaller inlining budget, denser jump tables, and no function alignment padding. |
| `-Oz` | Identical to `-Os`. |

### `-O0`

Do not optimize. This is the default.

The IR optimizer does not run at all, so `-f` pass options have no effect
at this level. Every local variable and every temporary lives in a stack
slot, and every C operation is a separate load, operation and store. Code
is large and slow, and each source variable is in memory at all times,
which is what a debugger wants (see [Debugging](debugging.md)).

Some decisions are made at every level, `-O0` included, because they are
taken when the IR is first generated or when code is emitted:

- `switch` statements are lowered to a jump table or a tree of compares
  by the rules in [Switch lowering](#switch-lowering).
- A `static` function that nothing calls is not emitted.
- On x86-64 and AArch64, functions start on a 16-byte boundary (see
  [Function alignment](#function-alignment)).
- On x86-64, temporaries that are not live at the same time share stack
  slots within a basic block.
- On x86-64 and AVR, branches are shortened to their smallest encoding.
  Thumb and RISC-V use their 16-bit branch forms only at `-O2`.

### `-O`, `-O1`

Optimize within basic blocks, without the global, loop and
interprocedural passes. `-O` is the same as `-O1`.

The IR optimizer runs the local passes listed in
[Passes with no switch](#passes-with-no-switch) and the
[`cfg-clean`](#-fcfg-clean--fno-cfg-clean) pass. Local variables stay in
memory (mem2reg is an `-O2` pass), but a value stored to a local and
read back in the same basic block is forwarded without the reload, unless
the local is `volatile`.

After optimization, a `static` function that no longer has a reachable
caller is not emitted. This is decided by reachability from the
functions visible outside the unit, constructors, destructors and
functions marked `__attribute__((used))`, so a `static` function called
only from another dead `static` function is dropped as well.

The code generators do the following at `-O1`:

| Target | Additional code generation at `-O1` |
|---|---|
| x86-64 | Skips reloading a value `rax` already holds; folds an address computation into the memory access, a shift into a scaled index, and a compare into the following branch; a temporary that is never referenced gets no stack slot. |
| AArch64 | A temporary that is never referenced gets no stack slot. |
| Thumb, RISC-V, AVR | Nothing. Code generation is the same as at `-O0`; only the IR differs. |

### `-O2`

Optimize for speed. Enables everything in `-O1`, and:

- every pass in the [pass table](#controlling-individual-passes), except
  `vectorize` on targets other than x86-64;
- function inlining, with the budgets in [Inlining](#inlining);
- attribute inference: a function found to read or write no memory, or
  only to read it, no longer forces the optimizer to forget loaded values
  at a call to it;
- constant propagation from read-only `static` variables (see
  [Passes with no switch](#passes-with-no-switch));
- splitting a value's live range around a loop that contains no call
  (part of `licm`);
- in the code generators: register allocation, tail calls, constant
  offsets folded into loads and stores, frameless functions, and the
  per-target choices in [Code generation by level](#code-generation-by-level).

### `-O3`

Accepted, and identical to `-O2`. No pass or code-generation decision in
EmbCC tests for a level above 2, and objects built with `-O3` are byte
for byte those built with `-O2`.

### `-Os`

Optimize for size. Enables everything in `-O2` except:

- `vectorize`, `unroll` and `switch-thread` are off, and so is live-range
  splitting around loops. Each of these adds code.
- The inlining budget for a callee that is copied is 6 IR instructions
  instead of 24, and a `static` function with two callers gets no larger
  budget (200 at `-O2`). A `static` function with a single caller is
  still inlined up to 2000 IR instructions, because moving it deletes
  the original (see [Inlining](#inlining)).
- A `switch` needs at least six cases and a value range of at most twice
  the number of cases to get a jump table (see
  [Switch lowering](#switch-lowering)).
- Functions are not padded to 16 bytes on x86-64 or AArch64.

Register allocation and tail calls are the same as at `-O2`.

### `-Oz`

Accepted, and identical to `-Os`.

### Spellings EmbCC does not accept

`-Og`, `-Ofast` and any level above 3 are refused:

```text
embcc: unknown optimization flag '-Og'
```

There is no "optimize for debugging" level. Use `-O0` with `-g`; see
[Debugging](debugging.md#optimized-code).

### Combining `-O` options

When more than one `-O` option is given, the last one sets the level,
with one exception: once `-Os` or `-Oz` has appeared, a later `-O0`,
`-O1`, `-O2` or `-O3` does not cancel the size request. `-Os -O2`
compiles as `-Os`, and `-Os -O0` runs the IR optimizer at its `-Os`
setting while the code generator works at level 0.
<!-- The driver sets opt_for_size and never clears it (main.c, the -O
     branch). This is probably a bug; reported to the lead. -->

The pass options described below are independent of their position on
the command line: `-fno-sccp -O2` and `-O2 -fno-sccp` are the same.

## What each level enables

| | `-O0` | `-O1` | `-O2` | `-Os` |
|---|---|---|---|---|
| Local passes ([no switch](#passes-with-no-switch)) | | yes | yes | yes |
| `cfg-clean` | | yes | yes | yes |
| Unreachable `static` functions dropped after optimization | | yes | yes | yes |
| `mem2reg`, `sroa`, `gcse`, `load-cse`, `pre`, `sccp`, `dse` | | | yes | yes |
| `licm` (rotation, invariant motion, strength reduction) | | | yes | yes |
| Live-range splitting around loops (part of `licm`) | | | yes | |
| `inline` | | | yes | yes, budget 6 |
| `div-magic`, `if-convert`, `tail-recursion`, `idiom` | | | yes | yes |
| `vectorize` | | | x86-64 only | |
| `unroll`, `switch-thread` | | | yes | |
| Attribute inference, read-only `static` propagation | | | yes | yes |
| Register allocation, tail calls (code generator) | | | yes | yes |
| 16-byte function alignment (x86-64, AArch64) | yes | yes | yes | |

## Controlling individual passes

### `-fNAME`, `-fno-NAME`

Turn the optimizer pass `NAME` on or off. `NAME` is one of the
seventeen names below. The level sets each pass's default, and an
explicit option overrides the level wherever it appears on the command
line.

```sh
embcc -O2 -fno-inline -c foo.c        # -O2 without inlining
embcc -O1 -fmem2reg -c foo.c          # -O1 plus one -O2 pass
```

A pass option takes effect only when the IR optimizer runs, which is at
`-O1` and above. At `-O0` it is accepted and does nothing.

`-fNAME` at `-O1` runs that pass, but not the other `-O2`-only work that
happens around it (attribute inference, read-only `static` propagation
and register allocation stay off).

A name that is not a pass, and not another option EmbCC knows, is
refused:

```text
embcc: error: unknown argument '-fno-unroll-loops'
```

followed by the usage summary. In particular, GCC's names for these
passes (`-funroll-loops`, `-fno-tree-vectorize`, `-finline-functions`)
are not EmbCC's; see
[GCC option names](#gcc-option-names).

Turning passes off one at a time is the quickest way to find which one
changes the behavior of a program:

```sh
for p in inline sroa licm unroll vectorize pre; do
    embcc -O2 -fno-$p prog.c -o prog && ./prog > out.$p
done
```

The table lists every pass. The entries that follow describe each one.

| Pass | Default | What it does |
|---|---|---|
| `mem2reg` | `-O2`, `-Os` | Promote scalar locals out of memory |
| `gcse` | `-O2`, `-Os` | Common subexpressions across blocks |
| `load-cse` | `-O2`, `-Os` | Redundant loads across blocks |
| `sccp` | `-O2`, `-Os` | Constant branches and unreachable blocks |
| `licm` | `-O2`, `-Os` | Loop rotation, invariant motion, induction-variable strength reduction |
| `vectorize` | `-O2`, x86-64 only | Lane-wise loops (SSE2) |
| `inline` | `-O2`, `-Os` | Function inlining |
| `dse` | `-O2`, `-Os` | Stores overwritten before they are read |
| `div-magic` | `-O2`, `-Os` | Division by a constant without a divide |
| `if-convert` | `-O2`, `-Os` | A two-way choice without a branch |
| `cfg-clean` | `-O1`, `-O2`, `-Os` | Jump threading and dead jumps |
| `tail-recursion` | `-O2`, `-Os` | A self tail call becomes a loop |
| `idiom` | `-O2`, `-Os` | Copy and clear loops become block operations |
| `sroa` | `-O2`, `-Os` | Split a local aggregate into scalars |
| `unroll` | `-O2` | Loop unrolling |
| `pre` | `-O2`, `-Os` | Partial redundancy elimination |
| `switch-thread` | `-O2` | State machines jump straight to the next case |

Two kinds of function get less than the full pipeline at any level:

- A function that uses computed `goto` (`goto *p`) is not given the
  passes that insert code on a control-flow edge: `mem2reg`, `licm`,
  `vectorize`, `if-convert`, `unroll`, `switch-thread`, `pre`,
  `tail-recursion`, `idiom` and the second `sroa` round. Its locals stay
  in memory.
- A C++ function with an exception region (a `try` block, or an object
  whose destructor must run during unwinding) gets only the block-local
  passes, `sroa`, `div-magic` and `cfg-clean`. Once no call in a region
  can throw, the region is removed and the function is optimized
  normally.

### `-fmem2reg`, `-fno-mem2reg`

Promote every scalar local variable whose address is never taken out of
its stack slot and into SSA values, which the later passes and the
register allocator work on. A variable live across a loop or down one arm
of an `if` becomes a value like any other.

A `volatile` local is never promoted. A local whose address is taken
stays in memory, unless `sroa` first shows that the address is used only
for loads and stores at constant offsets.

This pass is what makes most of `-O2` effective. With `-fno-mem2reg`,
locals stay in memory and the other global passes see little. Default:
on at `-O2` and `-Os`.

### `-fgcse`, `-fno-gcse`

Global common-subexpression elimination. An integer computation already
made in a block that dominates the current one is reused instead of
being computed again. Arithmetic, shifts and comparisons are reused, and
so is the address of a global variable or a string literal, which takes
two instructions on Thumb, RISC-V and AArch64 (except on AVR, where
keeping it live costs more than rebuilding it). A constant or a local's
address is cheaper to recompute than to keep live, and memory reads are
left to `load-cse`. Floating-point operations are never reused.
`EMBCC_NO_GCSE_ADDR=1` stops the reuse of addresses. Default: on at
`-O2` and `-Os`.

### `-fload-cse`, `-fno-load-cse`

Remove a load that repeats an earlier load of the same address on
every path to it, with no store, call, atomic operation, fence or inline
`asm` in between that could change the value. The second load becomes a
copy of the first. `volatile` loads are never removed. Default: on at
`-O2` and `-Os`.

### `-fsccp`, `-fno-sccp`

Resolve a conditional branch whose condition is a known constant into a
jump, and delete every block that is then unreachable. The constants
typically come from inlining a call with a constant argument, or from a
configuration macro. Default: on at `-O2` and `-Os`.

### `-flicm`, `-fno-licm`

The loop passes:

- **Rotation.** A loop is turned from top-tested into bottom-tested, so
  each iteration runs one conditional branch instead of a branch and a
  jump. The test is duplicated as a guard before the loop.
- **Loop-invariant code motion.** A computation whose operands do not
  change in the loop is moved in front of it. Only operations that
  cannot fault are moved, so a load through a pointer, a division and a
  remainder stay in the loop. A read of a local variable is moved only
  when nothing in the loop writes it and its address is never taken.
- **Induction-variable strength reduction.** An array access `a[i]` in a
  loop is turned into a pointer that advances by the element size each
  iteration.
- **Live-range splitting** (not at `-Os`). A value that is live across a
  call outside a loop, but not across any call inside it, gets a second
  name inside the loop, so the loop does not keep it in a callee-saved
  register.

Default: on at `-O2` and `-Os` (live-range splitting at `-O2` only).

### `-fvectorize`, `-fno-vectorize`

Rewrite a loop to process 16 bytes per iteration with SSE2 vector
instructions. Only the x86-64 code generator has vector instructions, so
the default is on at `-O2` for x86-64 and off everywhere else. Forcing it
on for another target with `-fvectorize` stops the compilation as soon as
a loop is vectorized, with an error such as:

```text
embcc: v.c:3: error: aarch64 codegen: unhandled IR op 56 in 'vi'
embcc: v.c:3: error: the ARMv7-M backend cannot lower this operation at 64 bits yet (function vi) [vload w=8 size=4]
```

A loop is vectorized when all of the following hold:

- it counts an induction variable up from 0 by 1, to a constant or to a
  value computed before the loop (a constant count must be a multiple of
  the lane count; a run-time count leaves a scalar loop for the
  remainder);
- every memory access is `array[i]` on a global array, all with the same
  element size of 4 or 8 bytes, so no two accesses can overlap;
- no access is `volatile`;
- every operation is an add, subtract, and, or, xor, a shift by a
  constant, or a multiply by a constant of the form 2^k or 2^k + 1;
- or the loop is an integer sum (`s += a[i]`), which is kept as four
  partial sums and added up after the loop.

A loop over arrays reached through a pointer is not vectorized.

Default: on at `-O2` for x86-64. Off at `-Os`.

<!-- BUG, reported to the lead: vec_op_char (opt.c) does not check
     i->flt, so `float a[N], b[N], c[N]; a[i] = b[i] + c[i]` with a
     constant N is vectorized with paddd (integer add) on x86-64 at -O2.
     Seen with embcc inspect ir v.c -O2 / llvm-objdump. The note below
     should be removed when that is fixed. -->

**Known problem:** on x86-64 at `-O2`, a loop of this shape over `float`
or `double` arrays is vectorized with integer lane operations and
computes wrong results. Compile such code with `-fno-vectorize` until
this is fixed. `-fremarks` reports each loop it vectorizes.

### `-finline`, `-fno-inline`

Replace a call to a small function defined in the same translation unit
with the function's body. See [Inlining](#inlining) for the budgets and
for what is never inlined. Default: on at `-O2` and `-Os`, with a smaller
budget at `-Os`.

### `-fdse`, `-fno-dse`

Dead-store elimination. Within a basic block, a store is removed when a
later store writes the same address at the same width and nothing in
between can read it:

```c
p->x = 1;   /* removed */
p->x = 2;
```

Only stores through the same address value are matched. A `volatile`
store is never removed. (A store to a local that is never read at all is
removed by dead-code elimination at `-O1`.) Default: on at `-O2` and
`-Os`.

### `-fdiv-magic`, `-fno-div-magic`

Replace a 32-bit division or remainder by a constant with a
multiplication by a "magic" constant and shifts, signed or unsigned.
64-bit divisions are left alone, because the 128-bit product they would
need is a library call on most targets. Unsigned division by a power of
two is already a shift at `-O1`. Default: on at `-O2` and `-Os`, because
the multiply sequence is also smaller than a divide on these targets.

### `-fif-convert`, `-fno-if-convert`

Turn a branch whose two arms each only copy an already computed value
into a select:

```c
x = c ? a : b;      /* no branch */
```

A select evaluates both arms, so only arms that are plain values are
converted; an arm with a load, a call or anything that can fault keeps
its branch. On x86-64 a select is a `cmov`, on AArch64 a `csel`; the other
code generators lower it in their own way. Default: on at `-O2` and
`-Os`.

### `-fcfg-clean`, `-fno-cfg-clean`

Simplify the control-flow graph:

- a jump to a jump goes straight to the final target;
- a branch whose two targets are the same becomes a jump;
- a jump to the next instruction is removed;
- the 0/1 value built for `&&` and `||` in a condition is not built,
  merged and tested again; each arm jumps to where it is going;
- unreachable code is removed.

Default: on at `-O1`, `-O2` and `-Os`.

### `-ftail-recursion`, `-fno-tail-recursion`

Turn `return f(...)`, where `f` is the function itself, into an
assignment of the new arguments and a jump to the top of the function.
The recursion becomes a loop that uses no stack and that the loop
passes can optimize. It is not done when the address of a parameter is
taken. Calls to other functions in tail position are handled by the code
generator (see [Code generation by level](#code-generation-by-level)).
Default: on at `-O2` and `-Os`.

### `-fidiom`, `-fno-idiom`

Recognize a loop with a constant trip count that does nothing but clear
an array or copy one array to another, and replace it with a single
block clear or block copy:

```c
for (i = 0; i < 64; i++) a[i] = 0;      /* one 256-byte clear */
for (i = 0; i < 64; i++) a[i] = b[i];   /* one 256-byte copy */
```

For a copy, both arrays must be distinct global objects. The block
operation is expanded inline by every code generator; no call to
`memset` or `memcpy` is introduced (x86-64 uses `rep movsq` and
`rep stosq` for blocks over 256 bytes). Default: on at `-O2` and `-Os`.

### `-fsroa`, `-fno-sroa`

Scalar replacement of aggregates. A local `struct`, `union` or array
whose address is used only for loads and stores at constant offsets is
split into one independent scalar per field accessed, which `mem2reg`
then promotes to registers. The aggregate must be at most 128 bytes and
split into at most 16 pieces.

It is not split when it is `volatile`, when any access is `volatile`,
when it is indexed by a variable, when its address is passed to a
function or stored, or when two accesses overlap with different widths
(a union used to reinterpret bytes). Default: on at `-O2` and `-Os`.

### `-funroll`, `-fno-unroll`

Copy the body of a counted loop several times with one test between
groups of copies. A loop is unrolled when it is bottom-tested (after
rotation), steps its induction variable by 1, compares it with a signed
`<` against a loop-invariant bound, has no other exit, and contains no
`alloca`, variable-length array, inline `asm` or computed `goto`.

| Body size (IR instructions) | Copies |
|---|---|
| 1 to 12 | 8 |
| 13 to 20 | 4 |
| more than 20 | not unrolled |

The number of copies is the largest power of two, up to 8, for which
copies times body is at most 96. The original loop is kept unchanged after the unrolled one and runs the
remaining iterations. Default: on at `-O2`; off at `-Os`.

### `-fpre`, `-fno-pre`

Partial redundancy elimination. When an expression is computed on some
paths to a point and computed again at that point, it is computed on the
paths that lacked it instead, and the later computation becomes a copy:

```c
if (c) sink(a * b);
return a * b;          /* a * b is computed once on each path */
```

The insertion never makes any path compute the expression more often
than before. Default: on at `-O2` and `-Os`.

### `-fswitch-thread`, `-fno-switch-thread`

In a loop around a `switch` on a state variable, where an arm sets the
state to a constant, jump from the end of that arm straight to the case
the next iteration will take, instead of going back through the switch.
The code between the arm and the switch (the loop's increment and exit
tests) is copied once for each such case. This is the shape of a lexer,
a protocol parser or a hand-written state machine. At most 48 blocks are
copied per switch and 400 IR instructions added per function. Default:
on at `-O2`; off at `-Os`.

## Passes with no switch

These run whenever the IR optimizer runs (`-O1` and above), and cannot
be turned off individually:

- **Constant folding** of integer operations at their width and
  signedness, of `float` and `double` arithmetic and comparisons on
  constants (see [Floating point](#floating-point)), and integer
  algebraic identities (`x + 0`, `x * 1`, `x * 0`, `x & 0`, ...).
- **Strength reduction**: multiplication by a power of two becomes a
  shift; unsigned division and remainder by a power of two become a
  shift and a mask. Signed division is never a plain shift, because it
  rounds toward zero.
- **Reassociation of constants**: `(x + 1) + 2` becomes `x + 3`.
  Integer only.
- **Local value numbering**: an integer computation, or a load with no
  store, call, fence or `asm` since the identical one, is reused within
  a basic block. `volatile` accesses and floating-point operations are
  never reused.
- **Quotient and remainder**: `a / b` and `a % b` with the same operands
  in one block divide once; the remainder is `a - q * b`.
- **Copy propagation** and **dead-code elimination**. A read of a local
  variable whose value is unused is removed, unless the variable is
  `volatile`. Loads through a pointer, divisions (which can trap), stores
  and branches are never treated as dead code, and neither are calls,
  except that at `-O2` an unused call to a function found to touch no
  memory and not to throw is removed.
- **Store-to-load forwarding** for local variables that are not
  `volatile` and whose address is not taken, within a basic block, and
  for a private `union` whose words are read back after a store.
- **Range checks**: `c >= 'a' && c <= 'z'`, and the negated form, become
  one subtraction and one unsigned compare.
- Instruction-selection preparation: constants become immediate
  operands, and constants and addresses are placed next to their use.

At `-O2` and `-Os` these also run:

- **Attribute inference.** Each function is analyzed for whether it
  writes memory its caller can see, or reads any. A call to a function
  that does neither does not invalidate loaded values. The attributes
  `pure` and `const` are not needed for this, and are accepted without
  effect.
- **Read-only `static` variables.** A `static` variable with file scope
  whose address is used only to load from it, in every function of the
  unit, is treated as a constant: each load of it becomes its initial
  value. A variable that is `volatile`, thread-local, weak, in a named
  section, pointed to by an initializer, or in a unit with top-level
  `asm` is excluded.

## Inlining

The inliner runs at `-O2` and `-Os` (or with `-finline` at `-O1`), before
the per-function passes. Sizes are counted in EmbIR instructions, as
`embcc inspect ir` prints them, before the callee is optimized.

| Limit | Value |
|---|---|
| Largest callee copied into a caller, `-O2` | 24 IR instructions |
| Largest callee copied into a caller, `-Os` | 6 IR instructions |
| Largest `static` callee with exactly one call site and its address never taken (it is moved, not copied) | 2000 IR instructions, at every level that inlines |
| Largest `static` callee with exactly two call sites and its address never taken, `-O2` | 200 IR instructions |
| A caller stops growing at | 800 IR instructions |
| Inlined calls per caller, at most | 64 |

Only direct calls to functions defined in the same translation unit are
inlined. The `inline` keyword has no influence on the decision.

`__attribute__((always_inline))` lifts the size limit and nothing else:
a call that cannot be inlined for one of the reasons below is still not
inlined. `__attribute__((noinline))` prevents inlining. Both are
honoured only where the inliner runs.

A call is never inlined when:

- it is indirect, or returns a structure;
- it is a call from a function to itself (a recursive function can
  still be inlined into its other callers, within the limits above);
- the callee is variadic or has a C++ exception region;
- the callee returns a structure, `_Complex`, `long double` or
  `__int128`, or uses `__int128`;
- a parameter is not a scalar of at most 8 bytes;
- the callee contains inline `asm`, `va_start`, `alloca` or a
  variable-length array, computed `goto`, or a call to a function that
  returns a structure;
- the caller has a C++ exception region or uses `__int128`.

`-fremarks` reports every decision with its reason, for example:

```text
big.c:2: remark: not-inlined 'helper': 45 instructions, budget 24 [inline/callee-too-large]
big.c:5: remark: inlined 'ai': 45 instructions into c, budget 24 [inline/always_inline]
big.c:7: remark: not-inlined 'ni' [inline/callee-is-noinline]
```

`embcc why not-inlined NAME FILE -O2` prints the same answer for one
function. See [Optimization remarks](diagnostics.md#optimization-remarks).

## Switch lowering

A `switch` is lowered when the IR is generated, so the rule applies at
every level, `-O0` included. A jump table is used when all of these
hold:

| Condition | `-O0` to `-O3` | `-Os`, `-Oz` |
|---|---|---|
| Number of cases | at least 4 | at least 6 |
| Span from lowest to highest case value | at most 4 × cases + 4 | at most 2 × cases |
| Span, absolute limit | 4096 | 4096 |
| Controlling expression | no wider than a pointer | no wider than a pointer |

AVR never uses a jump table. Otherwise the cases become a binary search
on the case values, ending in a chain of up to four equality tests.

## Code generation by level

The code generators read only two facts: whether the level is at least
1, and whether it is at least 2. `-Os` and `-Oz` are level 2 for them;
only function alignment checks for `-Os`.

At `-O2` and `-Os`, every code generator:

- **allocates registers** to values, including callee-saved registers,
  which are then saved and restored. On x86-64 and AArch64, a function
  with computed `goto` or a C++ exception region is compiled without
  register allocation;
- **folds a constant offset** into a load or store addressing mode;
- **makes tail calls**: a call immediately followed by a return of its
  value becomes a jump, under the conditions in the table below.

| Target | `-O2` / `-Os` specifics | Tail calls (`-O2`, `-Os`) |
|---|---|---|
| x86-64 | A leaf function with no stack frame omits `push rbp`; an ELF function that only saves registers uses pushes and no frame pointer; a shared epilogue for functions with several returns. Frameless and push-only functions are not made with `-g`. | Direct calls; no structure return; caller not variadic, no `alloca`, no computed `goto`, no exception region, no address of a local taken; all arguments in registers; not for the Windows convention. |
| AArch64 | A leaf function with no stack frame omits the `x29`/`x30` frame record (not with `-g`). | Direct, non-variadic calls; result at most 8 bytes, not a structure or floating point; function has a single return; no `alloca`, no exception region, no address of a local taken; all arguments in registers. Not with `-g`. |
| Thumb | Each function is generated with and without 64-bit register pairs and the shorter is kept (not with an FPU, not with `-g`); 16-bit branch and `cbz` forms. | As AArch64, result at most 4 bytes, not in a VFP register; only in a function that pushes nothing. Not with `-g`. |
| RISC-V | RV32: each function is generated with and without register pairs and the shorter is kept; compressed branches and jumps. | As AArch64, result at most XLEN. Not with `-g`. |
| AVR | Nine allocation strategies are tried and the shortest code is kept. Not with `-g`, and not in an interrupt handler. | Not in an interrupt handler, not with `-g`; result at most 8 bytes; no argument in a register the epilogue restores. |

On AVR, `-g` at `-O2` turns register allocation off entirely. See
[Debugging](debugging.md#how--g-changes-the-generated-code) for every way
`-g` changes code.

### Function alignment

| Target | `-O0` to `-O3` | `-Os`, `-Oz` |
|---|---|---|
| x86-64 | 16 bytes, padded with `nop` | none |
| AArch64 | 16 bytes, padded with `nop` | 4 bytes (instruction size) |
| Thumb | 2 bytes; 4 if the function contains inline `asm` | same |
| RISC-V | 4 bytes; 2 with the C extension | same |
| AVR | 2 bytes (instruction size) | same |

### Frame pointer

`-fomit-frame-pointer` and `-fno-omit-frame-pointer` are accepted and
have no effect. What each target does:

| Target | Frame pointer |
|---|---|
| x86-64 | `rbp` at `-O0` and `-O1`. At `-O2`, omitted in frameless leaf functions and push-only frames, unless `-g`. |
| AArch64 | `x29` frame record at `-O0` and `-O1`. At `-O2`, omitted in frameless leaf functions, unless `-g`. |
| Thumb | None. `r7` is set up only in a function with a variable-length array or `alloca`. |
| RISC-V | None. `s0` is set up only in a function with a variable-length array or `alloca`. |
| AVR | `Y` (`r28:r29`), only when the function has a stack frame, stack arguments, variable arguments, a structure return, or is an interrupt handler. |

## What the optimizer assumes

### Signed integer overflow

Signed integer arithmetic wraps in two's complement, at every level.
EmbCC behaves as if `-fwrapv` were always given: no optimization assumes
that signed overflow cannot happen. For example, `x + 1 > x` is not
folded to 1, and `(x * 2) / 2` is not folded to `x`.

`-fwrapv` is accepted and changes nothing. `-fno-wrapv`, `-ftrapv`,
`-fstrict-overflow` and `-fno-strict-overflow` are not accepted
(`unknown argument`). To find signed overflow at run time, use
[`-fsanitize=signed-integer-overflow`](#-fsanitizechecks).

Unsigned arithmetic wraps, as C requires. Out-of-range conversions from
floating point to integer are not folded at compile time.

Arithmetic wraps at the width of its type inside an expression as well as
when the result is stored. On AVR, where `int` is 16 bits, an `int` or
`unsigned int` result that leaves its 16-bit range wraps before the next
operation reads it: `long f(int x) { return x + 1; }` returns -32768 for
32767, and `(0xffffu + 1) / 2` is 0.

### Aliasing

EmbCC does no type-based alias analysis. A store through an `int *` is
assumed to be able to change a `float` that is later read, exactly as
with `-fno-strict-aliasing`.

It does rely on C's object model: a pointer computed from one object
(`&a[i]`, `p->field`, `base + offset`) is assumed to point into that
object and not into another. Two accesses based on different global
objects, or on different local variables, are assumed not to overlap,
and a pointer of unknown origin is assumed not to reach a local variable
whose address was never taken. Code that reaches one object by pointer
arithmetic from another is outside this assumption.

`restrict` is accepted and not used by the optimizer.
`__attribute__((may_alias))` is accepted and has no effect, because
nothing it would disable is done. `-fstrict-aliasing` and
`-fno-strict-aliasing` are accepted and have no effect.

### Floating point

From `-O1` up, the optimizer evaluates `float` and `double` arithmetic
whose operands are constants, and otherwise leaves a floating-point
computation as written:

- `+`, `-`, `*`, `/`, negation and the comparisons on constant operands
  are evaluated in the operation's own format with one rounding, to
  nearest, which is what the machine computes under the default rounding
  mode: `251 / 255.0f` and `0.1 + 0.2` become constants. Where `double`
  is binary32, as on AVR, a `double` operation is evaluated in binary32.
  A floating-point division by zero folds to the infinity the machine
  would produce. An operation is left to run time when an operand or the
  result is a NaN, because which NaN an invalid operation produces
  differs between machines. `long double` arithmetic is never evaluated
  at compile time;
- floating-point arithmetic is not reassociated or simplified (`x + 0.0`
  and `x * 1.0` stay), and two identical floating-point operations are
  not merged;
- a multiply and an add are never contracted into a fused multiply-add;
  EmbCC emits no fused multiply-add instruction on any target
  (`__builtin_fma` is not a builtin; `fma` is the library function);
- negation flips the sign bit, which is exact for zero and NaN.

The conversion of a constant (for example `(double)1000000`) is also
evaluated, with round-to-nearest. A conversion is left to run time when
the value is not finite, when it is out of range for an integer result,
or when it would carry a NaN between `float` and `double`.

The evaluation assumes the default rounding mode and floating-point
environment, so a program that changes the rounding mode with `fesetround`
does not see it applied to arithmetic on constants.

`-ffast-math`, `-fno-fast-math` and `-ffp-contract=STYLE` are not
accepted (`unknown argument`). `#pragma STDC FP_CONTRACT` and
`#pragma STDC FENV_ACCESS` are accepted without a diagnostic and have no
effect.

### `volatile`

Every access to a `volatile` object is performed as written, at every
level: it is not removed, merged, moved out of a loop, vectorized or
replaced by a value already known. This holds for an access through a
pointer, for a variable with static storage duration, and for a local
variable:

- A `volatile` local variable is kept in its stack slot by every code
  generator; it is never promoted to a register or given one by the
  register allocator. `volatile int v = 5; return v + v;` reads `v` twice,
  and `(void)v;` reads it once. A `volatile` local changed between
  `setjmp` and `longjmp` keeps its new value, as C11 7.13.2.1 requires.
- A `volatile` aggregate is not split.
- A `volatile` bit-field is read and written through its storage unit,
  and every such access is volatile. Reading the field loads the unit
  once; assigning to it loads the unit once and stores it once. The value
  of the assignment expression is the assigned value converted to the
  field's width, not a second read of the unit.

### Library calls

The optimizer does not introduce calls to library functions. Block
copies and clears (from structure assignment or from the
[`idiom`](#-fidiom--fno-idiom) pass) are expanded inline. EmbCC
recognizes no library function name as a builtin, so `-fno-builtin` and
`-ffreestanding` are accepted and change nothing here. The code
generators do call the compiler runtime for operations the target lacks
(for example software floating point, or 64-bit division on 32-bit and
8-bit targets); see [Libraries](libraries.md).

## Run-time checks: `-fsanitize`

EmbCC's sanitizer inserts checks that **trap**: a failed check executes
the target's trap instruction, with no runtime library and no message.
Under a debugger the program stops at the failing operation; without one
it stops instead of continuing with a wrong value.

| Target | Trap instruction |
|---|---|
| x86-64 | `ud2` |
| AArch64 | `udf` |
| Thumb | `udf #0` |
| RISC-V | `unimp` |
| AVR | a branch to itself (AVR has no trapping instruction) |

### `-fsanitize=CHECKS`

Enable the checks named in the comma-separated list `CHECKS`. Off by
default.

| Check | Traps when |
|---|---|
| `signed-integer-overflow` | a signed `+`, `-`, `*` or unary `-` (including compound assignment, `++` and `--`) overflows |
| `integer-divide-by-zero` | the divisor of `/` or `%` is zero, or a signed division is `MIN / -1` |
| `shift`, `shift-exponent` | a shift count is negative, or not less than the width of the promoted left operand |
| `undefined` | all of the above |

Any other name is refused, by name:

```text
embcc: <embcc>: error: -fsanitize=address is not supported: EmbCC's sanitizer inserts checks that TRAP, and this one needs a runtime library to report through. The ones it has are undefined, signed-integer-overflow, integer-divide-by-zero and shift
```

This includes `-fsanitize=all`, `null`, `alignment`, `bounds`,
`shift-base` and `float-divide-by-zero`.

The checks are ordinary IR, so the optimizer removes the ones it can
prove never fail: at `-O2` a division by a non-zero constant carries no
check, and a shift by a constant in range carries none. At `-O0` and
`-O1` every check stays.

### `-fno-sanitize=CHECKS`

Remove the named checks from the set enabled so far. The same names are
accepted.

### `-fsanitize-trap`, `-fsanitize-trap=CHECKS`, `-fsanitize-undefined-trap-on-error`

Accepted for compatibility; trapping is the only mode. Note that
`-fsanitize-trap=CHECKS` also **enables** the named checks, unlike in
GCC and Clang, where it only selects how checks enabled by
`-fsanitize=` report. `-fsanitize-trap` and
`-fsanitize-undefined-trap-on-error` without a list enable nothing.

Other options that start with `-fsanitize`, such as
`-fsanitize-recover=all`, are refused:

```text
embcc: error: -fsanitize-recover=all is not supported; EmbCC would emit ordinary code and the flag's promise would not hold
```

## Seeing what the optimizer did

### `embcc inspect ir FILE [OPTIONS]`

Print the EmbIR of every function as it stands after optimization at the
level given in `OPTIONS`. At `-O0` this is the IR before any pass, so
comparing the two shows exactly what the optimizer changed:

```sh
embcc inspect ir prog.c -O0 > before.ir
embcc inspect ir prog.c -O2 > after.ir
diff before.ir after.ir
```

Pass options apply, so `embcc inspect ir prog.c -O2 -fno-licm` shows the
effect of one pass. Each instruction ends with a comment giving the
source line, and usually the column, it came from. For example:

```text
func @g nparams=2 nvars=4 vregs=29 labels=3 {
  %7 = cmp.4s gt %0, #3	; 4:9
  brz.4s %7 -> L0	; 4:5
  %25 = mul.4s %0, %0	; 1:31
  ...
```

Here `mul` came from line 1, the body of an inlined function. The IR is
described in [EmbIR](../internals/ir.md).

### `embcc inspect cfg FILE [OPTIONS]`

Print each function's control-flow graph as the passes see it: blocks,
predecessors and successors, immediate dominators, and back edges
(loops).

### `embcc inspect callgraph FILE [OPTIONS]`

Print which function calls which, after optimization. At `-O2` a callee
that was inlined everywhere no longer appears.

### `-fremarks`, `-fremarks=json`, `embcc why`

Report each optimization decision (inlined or not and why, variables
promoted to registers, loops rotated, hoisted, vectorized, unrolled,
functions dropped) on standard error. See
[Optimization remarks](diagnostics.md#optimization-remarks).

### `-S`

Write the generated code as an assembly file. See
[Invoking EmbCC](invoking.md).

## GCC option names

| GCC or Clang option | EmbCC |
|---|---|
| `-Og` | Not accepted. Use `-O0 -g`. |
| `-Ofast` | Not accepted. |
| `-O3` | Accepted, same as `-O2`. |
| `-Oz` | Accepted, same as `-Os`. |
| `-fno-inline`, `-finline` | Same meaning. |
| `-finline-functions`, `-fno-inline-functions` | Not accepted. Use `-finline` / `-fno-inline`. |
| `-funroll-loops`, `-fno-unroll-loops` | Not accepted. Use `-funroll` / `-fno-unroll`. |
| `-ftree-vectorize`, `-fno-tree-vectorize` | Not accepted. Use `-fvectorize` / `-fno-vectorize`. |
| `-fgcse`, `-fno-gcse` | Same spelling, EmbCC's pass. |
| `-fwrapv` | Accepted; always the behavior. |
| `-fno-wrapv`, `-ftrapv` | Not accepted. |
| `-fstrict-aliasing`, `-fno-strict-aliasing` | Accepted, no effect; there is no type-based aliasing. |
| `-fomit-frame-pointer`, `-fno-omit-frame-pointer` | Accepted, no effect. See [Frame pointer](#frame-pointer). |
| `-ffunction-sections`, `-fdata-sections` | Accepted, not implemented: the functions of a unit share one `.text` section and its data one data section. |
| `-ffast-math`, `-ffp-contract=...` | Not accepted. |
| `-flto` | Refused: `embcc: error: -flto is not supported; EmbCC would emit ordinary code and the flag's promise would not hold` |
| `__attribute__((optimize(...)))`, `hot`, `cold`, `flatten` | Accepted, no effect. The level applies to the whole compilation. |
