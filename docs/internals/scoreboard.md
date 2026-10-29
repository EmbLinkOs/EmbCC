# Scoreboard

`tools/scoreboard.sh` measures where EmbCC stands against clang. It is
rerun every round of work, so that "competitive" is a number and the next
round goes where the gap is largest.

```sh
make embcc
sh tools/scoreboard.sh            # everything, minutes of QEMU
sh tools/scoreboard.sh --quick    # sections 2-4 only
sh tools/scoreboard.sh out.md     # write the page to a file
```

Every ratio is EmbCC / clang, so below 1.00 EmbCC is ahead. clang is the
reference because it is installed for every target EmbCC has; there is no
GCC cross-compiler here for most of them.

## What it measures

| Section | What | How |
|---|---|---|
| 1. Speed | Estimated cycles | `tools/bench/run.sh` on the Cortex-M4 and RV32 boards at `-O2` and `-Os`, geomean over its kernels |
| 2. Size | Code bytes | `lib/libc` at `-Os` for every target clang also has, AVR included. Function sizes from `llvm-nm -S`, summed two ways: per object (every function of each file both compilers build) and per function (the functions both define). clang gets `-fno-inline-functions`. Math (fdlibm, msun) is reported apart, because floating-point helpers dominate it. Steer by the per-object ratio: see [Reading it](#reading-it). |
| 3. Compile speed | Wall clock | `lib/libc`, and EmbCC's own `src/`, one file at a time for `x86_64-elf` at `-O2`. Both compilers read `lib/libc/include`. Without those headers clang fails most of `src/` at once and looks fast. |
| 4. Limitations | Refusals | Distinct refusal messages in `src/`: the constructs EmbCC names instead of compiling. The goal is zero. |

## Baseline, 2026-10-08

On integ-batch2 (#114), against clang 23.1.0.

**Speed:**

| Board | `-O2` |
|---|---|
| Cortex-M4 | 1.211 |
| RV32 | 1.177 |

EmbCC is already ahead on sort and text (and crc on RV32). It is furthest
behind on the state machine (1.66 to 1.75) and the fixed-point FIR (1.49
to 1.75).

**Size**, all functions and without math:

| Target | all | without math |
|---|---|---|
| thumbv7em | 1.313 | 1.276 |
| thumbv6m | 1.353 | 1.407 |
| RV32 | 1.351 | 1.263 |
| RV64 | 1.335 | 1.226 |
| x86-64 | 1.702 | 1.467 |
| AArch64 | 1.510 | 1.360 |
| MIPS32 | 1.304 | 1.280 |
| PowerPC | 1.221 | 1.190 |
| SPARC (both soft float) | 1.962 | 1.589 |
| LoongArch64 | 1.191 | 1.185 |

**Compile speed:**

| Corpus | Before | After the compile-speed fixes |
|---|---|---|
| `lib/libc` | 0.26 | 0.22 |
| `src/` | 2.49 | 1.16 |

The fixes are two `src/opt/opt.c` changes. Strength reduction rebuilt a
whole-function table per instruction. And every IR instruction carried 32
call-argument descriptors inline: 2.8 KB each, now 224 bytes.

**Limitations:** 99 backend refusals and 85 front-end and driver refusals.

## 2026-10-24

On close-gaps (after #162's library work), against clang 23.1.0.

**Speed** (`-O2` / `-Os`): Cortex-M4 1.102 / 1.103, RV32 1.058 / 1.104.
Furthest behind: the linked-list walk on the M4 (1.39), fixed point
(1.34 on both), the state machine (1.30 and 1.21), the matrix (1.20).
Ahead on sort (M4 0.85) and crc (RV32 0.75).

**Size**, per object (all / without math): Cortex-M4 1.135 / 1.081,
Cortex-M0 1.167 / 1.137, RV32 1.278 / 1.123, RV64 1.279 / 1.120,
x86-64 1.695 / 1.370, AArch64 1.492 / 1.261, MIPS32 1.266 / 1.153,
PowerPC 1.220 / 1.110, SPARC 1.244 / 1.108, LoongArch64 1.145 / 1.100,
AVR 1.527 / 1.385.

**Compile speed:** `lib/libc` 0.18, `src/` 1.05.

**Limitations:** 110 backend refusals and 43 front-end and driver
refusals.

## Reading it

- **Steer size by the per-object ratio.** EmbCC inlines a static function
  that has one caller even where clang, given `-fno-inline-functions`,
  keeps it. The per-function join then pairs the caller's whole body with
  clang's small wrapper and leaves clang's static out of the sum: when
  printf's engine became one such static, the Cortex-M4 read 1.40 per
  function and 1.13 per object, with no code grown. Per object, both
  compilers' bytes for the same file are counted, whoever inlined what.
- **Look at the per-target tables next.** One function's code can grow
  because of a choice another target's output makes look obvious
  (`docs/internals/testing.md`, code size).
- **Cycles are estimates.** They come from tools/bench's cost table (see
  `tools/bench/icount.c`), not from silicon. A kernel can move by
  placement alone under QEMU, so compare the instruction counts too.
- **A refusal counted in section 4 is honest.** EmbCC refuses what it
  cannot do correctly rather than emitting something that might work. The
  count falls only when a construct starts compiling correctly.
