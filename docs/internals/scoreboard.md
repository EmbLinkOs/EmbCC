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
| 2. Size | Code bytes | `lib/libc` at `-Os` for every target clang also has. Function sizes from `llvm-nm -S`, summed over the functions both compilers define. clang gets `-fno-inline-functions`, so a function is the same function in both. Math (fdlibm) is reported apart, because floating-point helpers dominate it. |
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
| thumbv7em | 1.316 | 1.282 |
| thumbv6m | 1.360 | 1.413 |
| RV32 | 1.358 | 1.283 |
| RV64 | 1.336 | 1.226 |
| x86-64 | 1.708 | 1.484 |
| AArch64 | 1.509 | 1.362 |
| MIPS32 | 1.312 | 1.296 |
| PowerPC | 1.224 | 1.195 |
| SPARC | 2.168 | 1.616 |
| LoongArch64 | 1.193 | 1.188 |

**Compile speed:**

| Corpus | Before | After the compile-speed fixes |
|---|---|---|
| `lib/libc` | 0.26 | 0.22 |
| `src/` | 2.49 | 1.16 |

The fixes are two `src/opt/opt.c` changes. Strength reduction rebuilt a
whole-function table per instruction. And every IR instruction carried 32
call-argument descriptors inline: 2.8 KB each, now 224 bytes.

**Limitations:** 99 backend refusals and 85 front-end and driver refusals.

## Reading it

- **A size ratio is per function.** One function's code can grow because
  of a choice another target's output makes look obvious, so the per-target
  tables are where to look next (`docs/internals/testing.md`, code size).
- **Cycles are estimates.** They come from tools/bench's cost table (see
  `tools/bench/icount.c`), not from silicon. A kernel can move by
  placement alone under QEMU, so compare the instruction counts too.
- **A refusal counted in section 4 is honest.** EmbCC refuses what it
  cannot do correctly rather than emitting something that might work. The
  count falls only when a construct starts compiling correctly.
