# embtrace — function-level tracing on the target

EmbTrace records every function entry and exit of a program built with
`-finstrument-functions`, on the target, in a ring in RAM. The program
writes the ring out as text through a console or a UART, and `embtrace`
on the host turns it into call counts, times, a call tree and a
Chrome/Perfetto trace. This page is the reference for both halves.

## Synopsis

```text
embtrace IMAGE.elf LOG [--tree] [--depth N] [--chrome OUT.json]
```

- `IMAGE.elf` names the addresses, from its symbol table.
- `LOG` is any text that holds a dump, such as a QEMU console log or a
  terminal capture. The last dump in it is read.

## On the target

Build the code to trace with `-finstrument-functions`. The two hooks it
calls are defined weak in `lib/rt` (`lib/rt/embtrace.c`), so the
program links with no more work. Then:
1. say where the dump goes;
2. optionally, give the trace a clock;
3. call `embtrace_dump()`.

```c
#include <embtrace.h>

EMBTRACE_NOI void embtrace_putc(int c) { uart_putc(c); }
EMBTRACE_NOI unsigned embtrace_clock(void) { return embtrace_dwt_cycles(); }

int main(void)
{
    embtrace_dwt_start();          /* Cortex-M3 and up: count cycles */
    run_the_thing();
    embtrace_dump();
}
```

```sh
embcc --target=thumbv7em-none-eabi -O2 -finstrument-functions -c main.c
```

`embtrace_dump()` can be called again at any point. `embtrace_reset()`
forgets the events so far, and `embtrace_enable(0)` and
`embtrace_enable(1)` stop and resume recording.

**Replacing the defaults.** Each of these is weak in `lib/rt`. A program
that defines one replaces it:

| Name | Default | What a replacement gives |
|---|---|---|
| `void embtrace_putc(int c)` | Writes nowhere | The dump's way out |
| `unsigned embtrace_clock(void)` | 0 | A time stamp. `<embtrace.h>` has `embtrace_dwt_start` and `embtrace_dwt_cycles` for Cortex-M3 and up, and `embtrace_mcycle` for RISC-V in machine mode |
| `const unsigned embtrace_clock_hz` | 0 | The clock's rate. It is written to the dump |
| `embtrace_events[N]` and `const unsigned embtrace_capacity = N` | 128 events (32 on AVR) | A ring of another size, or one in a section that survives a reset. Define both together |

**Not instrumented.** The replacements must not be instrumented: compile
them without `-finstrument-functions`, or mark them `EMBTRACE_NOI`
(`no_instrument_function`). The attribute counts on any declaration.

**Event size.** Each event is the function's address, its call site (the
return address), the clock and the kind. That is 16 bytes on a 32-bit
target and 24 on a 64-bit one.

**Safe in handlers and across cores.** Recording reserves the event's slot
with one atomic add on a 32-bit counter, and a reserved slot is its
writer's alone. So the hooks are safe in interrupt handlers and on
several cores without masking anything. Recording is turned off while
the dump runs.

### The dump

```text
EMBTRACE 1 <pointer bytes> <capacity> <recorded> <clock Hz>
<kind> <function> <call site> <time>      one line per event, oldest first, hex
EMBTRACE END
```

Kind 1 is an entry, 2 an exit. Other lines between the markers are
ignored, so a console that interleaves its own output does no harm.

## On the host

By default `embtrace` prints:
- a header line: how many events were read, how many were lost to the
  ring, and the deepest nesting;
- one line per function: calls, total time with its callees, self time,
  and the longest single call, largest total first.

**Times** are in the clock's units, which are cycles for the ready-made
clocks. With no clock, every time is 0, and `embtrace` counts events
instead and says so. The counts and the tree are exact either way.

**Options:**
- `--tree` prints the calls aggregated by path, in the order of the first
  call, to `--depth N` levels.
- `--chrome OUT.json` writes Chrome's JSON trace events, a begin and an
  end per call, which Perfetto (ui.perfetto.dev) and `chrome://tracing`
  open. With a rate in the dump, the times are microseconds.

```text
$ embtrace fw.elf console.log --tree --depth 3
365 events (365 recorded, 0 lost to the ring), no clock (times are counts of events); deepest 11
1 calls still open
177 of 177 call sites in their caller, 5 in a function not traced
main  x0
  fib  x1  353
    fib  x2  350
  helper  x5  5
```

**How the events are replayed:**
- **Stack.** They are replayed against a stack. An interrupt's calls
  nest inside whatever they interrupted, and replay like any other call.
- **Lost entries.** The ring keeps only the newest events, so the oldest
  frames' entries may be gone. An exit with no entry on the stack is
  counted and set aside.
- **Open frames.** A frame still open at the end, such as the function
  that called `embtrace_dump()`, is reported as open.
- **Call sites.** Each entry's call site, the hooks' second argument, is
  checked against the function the stack says called it. A site in a
  function the trace never names was in an uninstrumented caller, and is
  counted apart.

## Exit status

`embtrace` exits with:
- `0` when it has read a dump.
- `2` when:
  - the image or the log cannot be read;
  - the log holds no dump;
  - the last dump is cut off before `EMBTRACE END`.

## Checked by

`tests/golden/embtrace.sh` runs on two QEMU boards.

**Cortex-M3 (`lm3s6965evb`)**, with a ring big enough for the whole run:
- `fib(10)` is 177 calls, nested 11 deep, with the exact event-count
  times;
- a helper called from an uninstrumented function is attributed to the
  traced function above it;
- `no_instrument_function` keeps its function out, also on a definition
  after a plain prototype, and so do both exclusion lists;
- `main` is still open at the dump;
- every call site is in its caller;
- the Chrome trace has a begin per entry and an end per exit.

**RV32 (`virt`)**, with the default 128-event ring and `mcycle`:
- 941 events recorded, 128 kept and 813 lost;
- the times are real cycles;
- every call site is in its caller.

**Mutants:** 13, in the compiler, the runtime and the tool, each caught:
- an exit hook before the return's value;
- the hooks swapped;
- the function address passed as the call site;
- either exclusion list or the attribute ignored;
- the attribute not merged across declarations;
- a ring that does not wrap;
- a dump newest-first;
- the kinds swapped;
- the replay popping the wrong frame;
- self time wrong;
- the Thumb bit left on the symbols.
