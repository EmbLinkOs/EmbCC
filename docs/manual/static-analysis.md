# Static analysis

EmbCC checks four things about a program without running it:

- [Coding standards](#coding-standards): the MISRA C:2023 and CERT C rules
  that a compiler can decide, each one a warning.
- [The analyzer](#the-analyzer): what interrupt handlers share unsafely
  with the rest of the program, and memory that is freed twice, used after
  it is freed, leaked, or dereferenced before it is checked.
- [Stack bounds](#stack-and-timing-bounds): each entry point's worst-case
  stack, over the whole linked program, interrupts included.
- [Timing bounds](#stack-and-timing-bounds): each entry point's worst-case
  cycle count, from the code that was actually emitted.

None of them changes the generated code.

## Coding standards

```sh
embcc --misra -c motor.c            # MISRA C:2023
embcc --cert -c motor.c             # CERT C
embcc --misra --cert -c motor.c     # both
```

```text
embcc: mi.c:4:9: warning: MISRA C:2023 Rule 14.4 (required): a controlling expression that is not essentially Boolean [--warn=misra-14.4]
      if (n)
          ^
embcc: mi.c:5:16: warning: MISRA C:2023 Rule 10.4 (required): the operands of an arithmetic or comparison operator in different essential type categories (signed and unsigned) [--warn=misra-10.4]
          return x * 3;
                 ^
```

Each rule is a warning named after it: `--warn=misra-14.4`,
`--warn=cert-exp30`. They behave like any other warning:

```sh
embcc --misra --no-warn=misra-15.5 --error=misra-17.7 -c motor.c
```

```c
#pragma GCC diagnostic ignored "-Wmisra-11.4"   /* a register map */
#define UART0 ((struct uart *)0x4000C000u)
```

`--misra=2012` takes the MISRA C:2012 numbering, which is the same for the
rules EmbCC checks. `--no-misra` and `--no-cert` turn a whole standard off
again. The [warning reference](warnings.md) lists every rule with its
category (required, advisory, mandatory) and what it diagnoses: 66 MISRA
rules and 14 CERT rules.

### What a compiler can decide

A compiler can check a rule when the rule is about the translation unit
in front of it: a construct, a type, a conversion, a control-flow shape,
a call of a named function. These are the rules checked. A rule that
needs the whole program (8.6, one definition), a judgement (Directive
4.6's "typedefs that indicate size", Rule 2.1's "unreachable" in general),
or a document (the deviation records) is not, and no tool can check those
from one file. A MISRA compliance claim still needs the rest of the
process. `--misra` covers its first part.

MISRA's *essential type* model is followed: the type of an expression
before the implicit conversions C inserts, with comparisons and logical
operators Boolean, and an integer constant's category taken from its
sign and its width from the fewest bytes that hold its value. Rules 7.1
to 7.3 read the constant's spelling, so `0x10U` and `16u` are told apart
from `16`.

## The analyzer

```sh
embcc --analyze -c isr.c            # GCC's spelling: -fanalyzer
```

### Interrupt handlers

A bare-metal program's second thread of control is its interrupt
handlers, and the analyzer looks for what they share with the rest of
the code:

```c
unsigned ticks;
void SysTick_Handler(void) { ticks++; }
int main(void) {
    for (;;) {
        ticks += 2;
        if (ticks > 1000) return 0;
    }
}
```

```text
embcc: isr.c:5:9: warning: read-modify-write of 'ticks', which interrupt handler 'SysTick_Handler' also writes, outside a critical section: an interrupt between the read and the write loses its update [--warn=isr-race-rmw]
          ticks += 2;
          ^~~~~
embcc: isr.c:6:13: warning: 'ticks' is written by interrupt handler 'SysTick_Handler' and read here, and is not volatile: the compiler may keep it in a register [--warn=isr-shared-nonvolatile]
          if (ticks > 1000) return 0;
              ^~~~~
```

A function is a handler when it has an `interrupt` or `signal` attribute,
is named as CMSIS names them (`*_IRQHandler`, `*_Handler`), is AVR's
`__vector_N`, or is pointed at by a vector table (a global in a section
named for vectors). Everything a handler calls is handler code too.
Interrupts are off between `__disable_irq()` and `__enable_irq()`,
`cli()` and `sei()`, an RTOS's critical-section calls, a PRIMASK write,
`cpsid`/`cpsie` in inline assembly, and inside AVR's `ATOMIC_BLOCK`.

| Warning | Finds |
|---|---|
| `isr-shared-nonvolatile` | An object a handler writes and other code reads (or the other way round), not `volatile` |
| `isr-race-rmw` | `x++`, `x \|= m` or `x = x + 1` of an object a handler also writes, outside a critical section |
| `isr-torn-access` | An access wider than the machine reads or writes in one instruction, of an object a handler also uses, outside a critical section. A 32-bit counter on AVR, a 64-bit one on Cortex-M |
| `isr-unsafe-call` | A handler, or something it calls, calling what allocates, locks, blocks or does standard I/O |
| `isr-reentrancy` | A function that a handler and the code it interrupts both call, and that keeps state in a static object |
| `isr-critical-unbalanced` | A function that returns with interrupts still disabled on some path |

### Memory

Within each function, each local pointer is followed along the
statements: allocated (checked for null or not), freed, or escaped
(passed on, stored or returned). The arms of an `if` are followed
separately and then joined, and a loop's body is joined with what came
before it. A warning is only given for something some path really does.

| Warning | Finds |
|---|---|
| `analyzer-double-free` | Memory freed twice |
| `analyzer-use-after-free` | Memory used after it was freed |
| `analyzer-malloc-leak` | Memory the function allocated, with no pointer to it left when the function returns |
| `analyzer-null-dereference` | A null pointer dereferenced |
| `analyzer-possible-null-dereference` | The result of an allocation dereferenced before it was checked for null |

The names are GCC's `-Wanalyzer-` warnings, under `--warn=`.

## Stack and timing bounds

```sh
embcc -O1 --stack-info -c *.c                 # each object records its functions
embcc -O1 --stack-info *.o -o fw.elf --link-script=board.ld
embcc --stack-report fw.elf
embcc --stack-report fw.elf --stack-limit=2048 # exit 1 above 2 KiB
```

For this program on a Cortex-M4:

```c
__attribute__((noinline)) int sum(const int *a, int n) { int s = 0; for (int i = 0; i < 16; i++) s += a[i]; return s; }
__attribute__((noinline)) int fill(int *a) { for (int i = 0; i < 16; i++) a[i] = i; return sum(a, 16); }
volatile int g;
void TIM2_IRQHandler(void) { g++; }
int main(void) { int buf[16]; return fill(buf); }
int spin(int n) { int k = 0; while (n > 0) { n -= g; k++; } return k; }
```

```text
root                                frame      stack         cycles  worst stack path
TIM2_IRQHandler                         0          0              9  TIM2_IRQHandler  (interrupt)
main                                   72         80            295  main > fill > sum
spin                                    0          0              ?  spin
                                                                       unknown: a loop with no bound

worst case: 112 bytes = 80 (the program) + 0 (the deepest interrupt handler) + 32 bytes (Cortex-M's eight-word frame; 26 words, 104 bytes, with the FPU's context) pushed by the hardware; nesting interrupts add a handler's for each level
```

`main` needs 80 bytes: its own 72 (the array and the saved registers),
`fill`'s 8, and nothing for `sum`, a leaf that pushes nothing. Its 295
cycles are its own plus each callee's times the number of times its call
runs; the two loops are counted, 16 trips each. `spin`'s loop runs until
a volatile says so, so it has no bound, and the report says that rather
than guessing. `spin` is a root because nothing calls it.

`--stack-info` puts a record of each function in its object, in a debug
section (`.debug_embcc_stack`) that every linker keeps and no loader
loads. A Mach-O object has it in the section `__stack` of a segment
`__EMBCC`, which ld64 keeps in the image, and a COFF object in
`.debug_embcc_stack`. The record holds the function's frame, the calls it makes (the
direct ones, the run-time helpers the backend called, and each indirect
call's signature), and its own worst-case cycles. `--stack-report`
reads the records back from the linked image, or from objects, and for
each root works out:

- **the worst stack**: its frame plus the deepest chain of callees, with
  the path;
- **the worst cycles**: its own, plus each call's callee times the number
  of times the call can run.

A root is `main`, the reset handler, every interrupt handler, and
anything nothing calls. An indirect call is matched with every function
whose address is taken and whose parameters have the same classes. The
interrupt line adds the worst handler and the frame the hardware pushes,
which is the bound when handlers do not nest.

A number is unknown, and the report says why, when the call graph has a
recursion, when an indirect call matches nothing, when a callee was built
without `--stack-info`, when a frame grows at run time (a VLA or
`alloca`), or, for cycles, when a loop has no bound.

### Where the cycles come from

Each block costs what its machine instructions cost under the cycle model
that EmbSim and the benchmarks use: Cortex-M4, RV32 and RV64, the
ATmega328P, and a five-stage MIPS32 pipeline. Elsewhere one cycle per
instruction is used, as an estimate. A taken branch adds its refill. The
bound is the longest path through the function, with each loop counted
as its trip count times its longest iteration. A loop's trip count is
found when the loop counts: an induction variable started from a
constant, or from a value fixed before the loop (a pointer the loop
walks), stepped by a constant, and compared with a constant, or with that
value plus a constant, to leave.
A loop that cannot be bounded this way makes the function's cycles
unknown, and the report names its line.

This is a static bound on the code, not a measurement. It assumes no wait
states, no cache misses and no interrupts during the path, so on a part
with flash wait states or a cache, add those. It is a safe upper bound
for the model, and a good one for the zero-wait-state parts EmbCC's
embedded targets mostly are.

The [`embrt`](tools/embrt.md) tool gives the same stack bound from GCC's
formats, the `-fstack-usage` and `-fcallgraph-info` files, with options
for nesting interrupts and a JSON report. `--stack-report` needs only the
image, and adds the cycles.
