# embrt — the worst-case stack, proved

`embrt` computes how much stack each entry point and interrupt handler of
a firmware can need. It works from what the compiler and the objects say,
not from a run on a board, which only shows the paths that particular run
took. Where no bound exists, because of recursion, a frame that grows at
run time, or a function nobody describes, it names the reason instead of
guessing. This page is the command reference.

## Synopsis

```text
embrt OBJ.o... [--entry NAME]... [--isr NAME]... [--isr-frame BYTES]
               [--nested] [--max-stack SIZE] [--json]
```

## What it reads

Compile every file with three extra flags:

```sh
embcc --target=thumbv7em-none-eabi -O2 \
      -ffunction-sections -fstack-usage -fcallgraph-info=su -c main.c
```

For each object `OBJ.o`, `embrt` reads:

| File | From | What it gives |
|---|---|---|
| `OBJ.su` | `-fstack-usage` | every emitted function's frame, in GCC's format |
| `OBJ.ci` | `-fcallgraph-info=su` | the calls left after optimisation, and which go through a pointer, in GCC's format |
| `OBJ.o` | the object itself | its call relocations |

The relocations matter. Every call to a function in another section is
one, including the calls a backend makes to a run-time helper: a
soft-float add, a 64-bit divide, an `__aeabi_*` routine. No call graph
built from the source can show those calls. `-ffunction-sections` puts
each function in its own section, so even a call within one file leaves a
relocation.

GCC writes the same two files with the same flags, so objects from GCC
and EmbCC can be analysed together. Give `embrt` the run-time library's
objects too, compiled the same way, so the helpers' frames are known.

## What it reports

```text
$ embrt app.o softfp.o aeabi.o --isr SysTick_Handler --isr-frame 32
entry main: 280 bytes at most
       main                               16 bytes (calls through a pointer)
    -> mix                                48 bytes
    -> __adddf3                           16 bytes
    -> addsub                            136 bytes
    -> unpack                             64 bytes
interrupt SysTick_Handler: 40 bytes at most
       SysTick_Handler                    40 bytes

a call through a pointer is taken to reach any of the 2 functions whose address the program takes

worst case with interrupts: 352 bytes (280 for the entries, 72 for the handlers, 32 per handler for the hardware's frame)
```

- **Entries.** Each entry (`--entry`, `main` by default) gets its worst
  case: its frame plus the deepest of its callees', all the way down. The
  path below it reaches that worst case.
- **Indirect calls.** A call through a pointer is assumed to reach any
  function whose address the program takes, from a relocation that is
  not a call. That is sound, and the report says so. A function table's
  entries are counted; a function whose address is never taken is not.
- **Interrupts.** `--isr NAME` adds a handler on top of the entries'
  worst case:
  - without `--nested`, the deepest handler (only one interrupts at a
    time);
  - with `--nested`, every handler at once, each preempting the last.

  `--isr-frame BYTES` is what the hardware pushes per interrupt: 32 on a
  Cortex-M without an FPU, 104 with lazy FPU stacking.
- **Unbounded.** A path with no bound is reported with its reason, and the
  exit status is 1:

| Reason | Cause |
|---|---|
| `recursion` | a cycle in the calls, named by the function it closes through |
| `its frame grows at run time` | a variable-length array or `alloca`, which `-fstack-usage` marks `dynamic` |
| `no object defines it` | a callee in no object given, such as a library compiled without `-fstack-usage` |
| `no frame size` | an object without its `.su` file |

`--max-stack SIZE` makes the exit status 1 when the total (with
interrupts) exceeds `SIZE`. Use it to check a linker script's stack size
in a build. `--json` writes each entry's bound, path and reason, and the
total.

## Exit status

| Status | Meaning |
|---|---|
| 0 | every entry is bounded, and within `--max-stack` |
| 1 | an entry has no bound, an object has no `.su`, or the total exceeds `--max-stack` |
| 2 | an object cannot be read, or an option is malformed |

## How it is checked

`tests/golden/embrt.sh` checks EmbRT in three ways.

**Against real runs.** It builds every `tests/exec` program that links
without libc for the Cortex-M3, with lib/rt's helpers. It runs each on
QEMU's lm3s6965evb under a startup that fills the free stack with a
pattern, then reports how much of it `main` overwrote. Every measured
use must be at or under EmbRT's bound: 168 programs pass, some exactly
at their bound. The first version of this test found that EmbCC's
`-fstack-usage` left a variadic function's 16-byte register save area out
of its frame, which made a bound 16 bytes short. That is fixed, on both
ARMv7-M and ARMv6-M.

**The compiler's files.** It checks `-fcallgraph-info`'s nodes and edges,
including indirect calls and a call left behind by inlining, and the
`dynamic` qualifier on a VLA function's frame.

**Each reason and calculation.** It checks every unbounded reason, the
interrupt totals (deepest and nested), the separation of two same-named
`static` functions in different files, and `--max-stack` at its boundary.
