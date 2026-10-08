# embsim — run a Cortex-M image without a board

`embsim` runs a linked ELF image the way a Cortex-M part does. It is ISO
C with no dependencies, so it builds on any machine, including one
without QEMU. It counts the instructions it executes and estimates the
cycles they take, and gdb, lldb or embdbg can debug the image while it
runs. This page is the command reference; how EmbSim is built inside,
and how to add a core, a peripheral or a board, is
[EmbSim's internals](../../internals/embsim.md).

## Synopsis

```text
embsim IMAGE.elf [--board NAME] [--cpu NAME] [--ram-size SIZE]
                 [--until STRING] [--max-insns N] [--stats]
                 [--count FILE] [--trace FILE] [--no-semihosting]
                 [--gdb [HOST:]PORT [--gdb-wait]]
```

```sh
embcc --target=thumbv7em-none-eabihf -O2 -c main.c
embld -e reset -Ttext 0 -Tdata 0x20000000 boot.o main.o -o fw.elf
embsim fw.elf --board mps2-an386 --stats
```

## The boards

Each board has the memory and UART of QEMU's model of it, so an image
built for QEMU runs unchanged.

| Board | Core | Memory | UART |
|---|---|---|---|
| `lm3s6965evb` (default) | Cortex-M3 | 256 KiB flash at 0, 64 KiB SRAM at `0x20000000` | PL011 at `0x4000C000` |
| `mps2-an385` | Cortex-M3 | 4 MiB at 0, 4 MiB at `0x20000000`, 16 MiB at `0x60000000` | CMSDK at `0x40004000` |
| `mps2-an386` | Cortex-M4 with FPv4-SP | as `mps2-an385` | CMSDK |
| `mps2-an500` | Cortex-M7 with FPv5 (double) | as `mps2-an385` | CMSDK |
| `microbit` | Cortex-M0 | 256 KiB flash at 0, 16 KiB SRAM at `0x20000000` | nRF51 at `0x40002000` |

- `--cpu` runs another core on the board: `cortex-m0`, `cortex-m0plus`,
  `cortex-m3`, `cortex-m4` or `cortex-m7`.
- `--ram-size` changes the size of the SRAM at `0x20000000`.
- A segment of the image outside the board's memory gets memory of its
  own, so a link script for a similar part still runs.
- Flash on the lm3s6965 and the micro:bit ignores stores, as flash does.
  On the MPS2 boards it is SSRAM, which takes them.

## What it models

- **The instruction sets.**
  - ARMv6-M.
  - ARMv7-M, with the DSP multiplies and saturation of ARMv7E-M.
  - The floating-point unit: FPv4-SP, and FPv5's double precision.
    Arithmetic follows the architecture's NaN rules and FPSCR's DN and
    FZ bits, not the host's.
  - An instruction the core does not have is a UsageFault (a HardFault
    on ARMv6-M), as on the part.
- **Reset.** The core fetches the stack pointer and the reset vector from
  the vector table at address 0. The ELF entry point is not used.
- **Exceptions.**
  - Entry stacks the frame, the FPU's extended frame when CONTROL.FPCA is
    set, and keeps the 8-byte alignment that xPSR bit 9 records.
  - EXC_RETURN chooses the stack and the frame to return with.
  - Priorities, with AIRCR's grouping, PRIMASK, BASEPRI and FAULTMASK.
  - SVC, PendSV, SysTick, and the NVIC's interrupts. Pend one through
    ISPR, STIR or ICSR.
  - Faults: undefined instructions, invalid state, unaligned accesses
    (and CCR.UNALIGN_TRP), divide by zero (CCR.DIV_0_TRP), bus errors,
    and a disabled coprocessor (CPACR). Each is recorded in CFSR, HFSR
    and BFAR.
  - A fault whose handler is disabled or cannot preempt escalates to
    HardFault. One that HardFault cannot take locks the core up.
- **Thread mode.** Code can run on the PSP, and unprivileged.
- **Exclusives.** LDREX and STREX keep a local monitor.
- **Bit-banding.** The bit-band aliases of SRAM and of the peripherals,
  on the boards that have them.
- **Time.** SysTick and DWT's CYCCNT advance by the estimated cycles,
  so a timed run gives the same answer every time.
- **Other peripherals.** Apart from the UART, the system control space
  and DWT, the peripheral space reads as zero and ignores writes.

## Counting

Each instruction gets the cost in `tools/bench/cost.h`, the table
tools/bench uses in QEMU. That covers loads, multiples, divides, VFP
division and square root, and 2 cycles more for a taken branch.

- `--stats` prints the count and the estimate when the run ends.
- `--count FILE` writes them as two lines, the format of the bench's
  QEMU plugin, so either can feed `tools/bench`.

The estimate is a model, not a cycle-accurate core: there are no wait
states and no pipeline stalls. It charges the things a compiler chooses
between.

`--trace FILE` writes the address and halfwords of every instruction
executed (`-` for stderr).

## Output, and the end of a run

The board's UART writes to stdout. The UART must be enabled as on the
part: CMSDK's CTRL, and the nRF51's ENABLE and STARTTX.

Semihosting (`bkpt 0xab`) is on unless `--no-semihosting` is given. It
handles:
- console output: SYS_WRITEC, SYS_WRITE0 and SYS_WRITE;
- the exit calls: SYS_EXIT and SYS_EXIT_EXTENDED;
- the small calls a C library makes at start-up.

| The run ends when | Exit status |
|---|---|
| the image calls SYS_EXIT_EXTENDED | the code it gives |
| the image calls SYS_EXIT | 0 for ADP_Stopped_ApplicationExit, else 1 |
| the image requests a reset (AIRCR.SYSRESETREQ) | 0 |
| a branch to itself, or a WFI, that no exception can interrupt | 0 |
| the output contains `--until`'s string | 0 |
| the core locks up | 3 |
| `--max-insns` instructions have run | 4 |
| the image cannot be loaded, or an option is wrong | 2 |

A run that ends at a lockup or at `--max-insns` says why on stderr.

## Debugging: `--gdb`

`--gdb PORT` serves the GDB remote protocol on `localhost:PORT` while the
image runs. `--gdb-wait` holds the core at reset until a debugger
connects, as QEMU's `-S` does; without it the image runs, and a debugger
that connects stops it where it is. `HOST:PORT` listens on another
address, and QEMU's `tcp::PORT` is accepted too.

```sh
embsim fw.elf --board mps2-an386 --gdb 1234 --gdb-wait &
gdb fw.elf -ex 'target remote localhost:1234'
lldb fw.elf -o 'gdb-remote localhost:1234'
embdbg fw.elf remote 1234
```

The server answers as QEMU's stub does, so a debugger sees the same
machine on either, and a script written for one runs on the other:

- **Registers.** r0 to r12, sp, lr, pc and xpsr; d0 to d15 and fpscr on
  a core with an FPU (gdb shows s0 to s31 from them); and msp, psp,
  primask, control, and on ARMv7-M basepri and faultmask. The target
  description gdb reads has the FPU only when the core has one.
- **Breakpoints**, software and hardware alike. The server keeps them;
  it never writes into the image, so they work in flash.
- **Watchpoints**: `watch`, `rwatch` and `awatch`. As on QEMU, the
  target stops before the access, and gdb steps it.
- **Stepping**: `stepi` runs one instruction. An exception that is
  pending is taken first, so a step can stop at a handler's first
  instruction.
- **Interrupting** (Ctrl-C, the protocol's 0x03). The server looks for
  it every 16384 instructions.
- **Memory** reads and writes, and `load`. The server gives gdb a memory
  map from the board: flash, RAM, and the device space. gdb programs
  the flash with it, and uses hardware breakpoints there (it says so
  once: "automatically using hardware breakpoints").
- `kill` ends EmbSim with status 0. `detach` leaves the image running to
  its end, as a board does when the probe lets go; EmbSim then exits as
  without a debugger.

A run under a debugger ends differently:

| What happens | What the debugger sees |
|---|---|
| the image exits (semihosting, a reset request, `--until`, `--max-insns`) | the program exited, with the status of the table above |
| the core locks up | a stop with SIGSEGV, at the lockup |
| a WFI or a loop that nothing can interrupt | nothing: the core waits, as a part does, until the debugger interrupts it |

Time is the core's estimated cycles, with or without a debugger: a run
stopped at a breakpoint and continued has the counts of one that never
stopped. A WFI the debugger interrupts completes when it is resumed, as
on a part (halting wakes the core; QEMU's stays asleep).

### `monitor` commands

| Command | Action |
|---|---|
| `monitor reset` | reset the core and the devices, as the reset pin does. Memory keeps what is in it, so an image gdb loaded stays, and the counts start again |
| `monitor reload` | load IMAGE.elf into memory again, then reset |
| `monitor stats` | the instructions run and the estimated cycles |
| `monitor insns`, `monitor cycles` | each alone, as a number |
| `monitor help` | the list |

`monitor insns` at a breakpoint is the number `--max-insns` would stop
a run without a debugger at, one instruction before it.

## Not modelled yet

- The MPU (MemManage faults).
- ARMv8-M (TrustZone, the Cortex-M23/M33).
- FPv5's VSEL, VMAXNM, VMINNM and VRINT.
- The half-precision and fixed-point VCVT forms.
- The ARMv7E-M parallel add and subtract instructions.
- Peripherals beyond the UART, SysTick, the NVIC and DWT's cycle counter.

Each instruction among these is a UsageFault, so a run that needs one
stops at a named fault rather than computing something wrong.

## How it is checked

`tests/golden/embsim.sh` uses QEMU as the referee.

**The exec corpus.** Every `tests/exec` program is built with lib/libc
and lib/rt and run on QEMU and on EmbSim. The output and the exit status
must be the same. It runs on five cores:
- the M3 at -O0;
- the M4 with soft float at -Os;
- the M4 with FPv4-SP at -O2;
- the M7 with FPv5 at -O2;
- the M0 at -O2.

On the first four, the instruction count and the estimated cycles must
be the same as well. QEMU's count comes from tools/bench's plugin. It is
high by a constant, because QEMU counts the boot's first blocks twice,
so the test requires every program to show exactly the constant that a
`main` that only returns shows.

**Exceptions.** `tests/golden/embsim/exc.c` exercises SVC, PendSV,
nested interrupts, PRIMASK and BASEPRI, a handled and an escalated
UsageFault, SysTick waking WFI, a thread on the PSP, and the FPU's
extended frame. Its output must be QEMU's, and the record in
`tests/golden/embsim/exc-*.txt`.

**The ends of a run.** The exit status, the lockup, the idle loop,
`--until` and `--max-insns`.

**The GDB server.** `tests/golden/embsim-gdb.sh` runs the same gdb
session, on the same image, on QEMU's stub and on EmbSim's server, on
the M3 and on the M4 with its FPU, and the transcripts must be the same:
breakpoints, steps, registers, memory, a variable changed, watchpoints,
and the run to its exit. It does the same with an interrupted run, with
lldb, with embdbg and with gdb's `load`, and checks the `monitor`
counts against a run without a debugger.

Writing that test found one QEMU bug. On the mps2-an386, QEMU resumes a
divide that trapped with the instructions before it in its translation
block undone, so the test does not print that quotient.
