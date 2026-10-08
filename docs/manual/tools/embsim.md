# embsim — run a firmware image without a board

`embsim` runs a linked ELF image the way the part does: a Cortex-M
(ARMv6-M, ARMv7-M), a RISC-V core (RV32 and RV64, IMAFDC) or an AVR
(the ATmega328P). It is ISO C with no dependencies, so it builds on any
machine, including one without QEMU. It counts the instructions it executes and estimates the
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

embcc --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -O2 -c main.c
embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 boot.o io.o main.o -o fw.elf
embsim fw.elf --board virt --stats

embcc --target=avr -Os -c main.c
embld -e __vectors -Ttext 0 -Tdata 0x100 boot.o io.o main.o librt.a -o fw.elf
embsim fw.elf --board uno --stats
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
| `virt` | RISC-V, RV32 or RV64 with IMAFDC | 128 MiB RAM at `0x80000000` | NS16550A at `0x10000000` |
| `uno` | ATmega328P (AVR5) | 32 KiB flash, 2 KiB SRAM at data `0x100` | USART0 at data `0xC0` |

- `--cpu` runs another core on the board: `cortex-m0`, `cortex-m0plus`,
  `cortex-m3`, `cortex-m4` or `cortex-m7` on the Cortex-M boards; `rv32`
  or `rv64` on `virt`, which otherwise takes the image's width (an
  ELFCLASS64 image is RV64); `atmega328p` on `uno`.
- `--ram-size` changes the size of the SRAM at `0x20000000`, or of
  virt's RAM (the harness's QEMU runs give it `-m 8`, so
  `--ram-size 8M` is the same machine).
- virt also has QEMU's reset ROM at `0x1000`, the SiFive test device at
  `0x100000` that ends a run, and the CLINT at `0x2000000`; the PLIC's
  space reads as zero and ignores writes.
- On `uno` the AVR's data space is at `0x800000` on EmbSim's bus, as gdb
  numbers it: the registers at `0x800000`, the I/O registers from
  `0x800020`, the SRAM from `0x800100` (`--ram-size` resizes it), and
  flash at 0. Timer/Counter1 is there too, and the other I/O registers
  keep what is written.
- A segment of the image outside the board's memory gets memory of its
  own, so a link script for a similar part still runs.
- Flash on the lm3s6965 and the micro:bit ignores stores, as flash does.
  On the MPS2 boards it is SSRAM, which takes them.

## What it models: the Cortex-M

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

## What it models: the RISC-V core

- **The instruction sets.** RV32I and RV64I, M, A (LR/SC and every
  AMO), F and D, C (every compressed form at each width), Zicsr and
  Zifencei. An instruction outside them -- a reserved encoding, an
  extension EmbCC does not emit (B, V, Zfh), S-mode's SRET and
  SFENCE.VMA -- is an illegal-instruction trap.
- **Floating point.** IEEE arithmetic done in integers, so a result is
  the same on any host: the five rounding modes and the dynamic one,
  the accrued flags in fflags with tininess detected after rounding,
  the canonical NaN, NaN-boxing of singles in the 64-bit registers, and
  FMIN and FMAX as minimumNumber and maximumNumber. The FPU is off at
  reset (mstatus.FS 0), and an F or D instruction then traps, as on
  QEMU; the harness's boot turns it on.
- **Reset.** The core starts at `0x1000`, in QEMU's reset ROM, which
  loads a0 (the hart, 0), a1 (the device tree's address) and a2 (the
  firmware information) and jumps to the start of RAM. The ROM is the
  same, word for word, as QEMU's for the image, so the two run the same
  instructions from the first; the device tree itself is not there.
- **Privilege.** Machine mode, and user mode for an mret to it (machine
  CSRs, mret and an ecall's cause then behave as in U-mode). misa says
  A, C, D, F, I, M and U.
- **Traps.** mtvec, direct and vectored; mepc, mcause, mtval (the
  instruction for an illegal one, the address for an access, the pc
  for ebreak); mstatus's MIE, MPIE and MPP. A trap whose vector cannot
  be fetched is a lockup, which ends the run.
- **Memory.** The bus is 32 bits wide: an RV64 access above 4 GiB, or
  anywhere nothing is, is an access fault. Ordinary loads and stores
  may be misaligned, as on QEMU; LR, SC and the AMOs may not (a
  misaligned-address trap). SC succeeds when its address and the value
  LR read are still there, which is QEMU's rule.
- **Interrupts.** The CLINT's software (msip) and timer (mtimecmp)
  interrupts, through mip and mie. WFI waits for one: it skips ahead to
  mtimecmp when the timer is enabled in mie, and ends the run when
  nothing can wake it.
- **The counters.** mcycle and cycle read the estimated cycles, minstret
  and instret the instructions; time and the CLINT's mtime count with
  mcycle, one tick per cycle (the core is clocked at the device tree's
  10 MHz timebase). All three can be written. On QEMU these follow the
  host's clock instead, so a program that prints them differs there.
- **CSRs.** The machine-mode ones (mstatus, misa, mie, mip, mtvec,
  mscratch, mepc, mcause, mtval, mcounteren, mcountinhibit, menvcfg,
  medeleg, mideleg, the IDs), the PMP registers (kept, not enforced),
  the counters and the event counters (zero), and fflags, frm and fcsr.
  Another CSR, or a write to a read-only one, is an illegal
  instruction.

## What it models: the AVR

- **The instruction set.** The ATmega328P's: the classic AVR core with
  the multiplies (MUL, MULS, MULSU, FMUL, FMULS, FMULSU), MOVW, LPM
  Rd,Z and LPM Rd,Z+, and JMP and CALL; a 16-bit program counter, so a
  call pushes two bytes (big-endian on the stack, as the part does).
  SREG's H, S, V, N, Z and C are the instruction set manual's for every
  instruction, SBC, SBCI and CPC leaving Z set only when it was. An
  instruction the part does not have -- ELPM, EIJMP, EICALL, the
  XMEGA's -- and SPM, which is not modelled, end the run as a lockup
  that names it.
- **Memory.** The register file, SP and SREG in the data space (LD from
  address 30 reads r30); X, Y and Z with pre-decrement, post-increment
  and displacement; program memory read by LPM. The program counter
  wraps at 32 KiB, as the part's does.
- **Reset.** The core starts at 0, the reset vector, with SP at RAMEND
  (0x08FF) and SREG 0 (QEMU's starts with I set; the harness clears
  SREG before it matters).
- **Interrupts.** The vector table at 0, two words a vector; the lowest
  vector requested is taken when I is set, with the return address
  pushed, I cleared and the source's flag cleared; after SEI and after
  RETI one more instruction runs first. USART0's data-register-empty and
  transmit-complete, and Timer/Counter1's capture, compare A and B and
  overflow, are the sources.
- **SLEEP**, when SMCR.SE is set, waits for an interrupt: it skips ahead
  to the device that will request one, and ends the run when nothing
  can (or I is clear). Every sleep mode is treated as Idle. BREAK is a
  NOP, as on a part whose on-chip debugging is off; WDR is a NOP.
- **USART0** sends what is written to UDR0 when the transmitter is on
  (UCSR0B.TXEN0); UDRE0 is always set, and TXC0 is set at once.
- **Timer/Counter1** counts the core's cycles through its prescaler,
  which runs free as the part's does, in every waveform mode: normal,
  CTC with OCR1A or ICR1 as TOP, fast PWM and the dual-slope modes,
  setting TOV1, OCF1A, OCF1B and ICF1 where the datasheet says; a flag
  is cleared by writing it a one, or by entering its vector; the 16-bit
  registers go through TEMP.
- **Cycles are exact.** Each instruction takes the datasheet's cycles
  ("Instruction Set Summary"): a taken branch one more, a skip one more
  over a one-word instruction and two over a two-word one, an
  interrupt's entry four, and four more when it wakes SLEEP. The part
  has no wait states, so this is the count the silicon takes.

## Counting

Each instruction gets the cost in `tools/bench/cost.h`, the table
tools/bench uses in QEMU. On the Cortex-M that covers loads, multiples,
divides, VFP division and square root, and 2 cycles more for a taken
branch. On RISC-V (a plain in-order pipeline, at either width): 1, a
load 2, a divide or remainder 16, fdiv and fsqrt 16, and 2 more for a
taken branch or jump.

- `--stats` prints the count and the estimate when the run ends.
- `--count FILE` writes them as two lines, the format of the bench's
  QEMU plugin, so either can feed `tools/bench`.

On the Cortex-M and RISC-V the estimate is a model, not a
cycle-accurate core: there are no wait states and no pipeline stalls. It
charges the things a compiler chooses between. On the AVR it is the
datasheet's count (above). On the AVR `--count`'s file has a third
line: the instructions a skip passed over, which are not run and are
not in the count (QEMU's plugin counts them; see below).

`--trace FILE` writes the address and halfwords of every instruction
executed (`-` for stderr); on RISC-V, the instruction as one word, or
as one halfword for a compressed one; on the AVR, its one or two
words, at the byte address.

## Output, and the end of a run

The board's UART writes to stdout. The UART must be enabled as on the
part: CMSDK's CTRL, the nRF51's ENABLE and STARTTX, the AVR's TXEN0.
The NS16550A sends what is written to THR, and its LSR always says the
transmitter is empty.

On the Cortex-M, semihosting (`bkpt 0xab`) is on unless
`--no-semihosting` is given. It handles:
- console output: SYS_WRITEC, SYS_WRITE0 and SYS_WRITE;
- the exit calls: SYS_EXIT and SYS_EXIT_EXTENDED;
- the small calls a C library makes at start-up.

| The run ends when | Exit status |
|---|---|
| the image calls SYS_EXIT_EXTENDED | the code it gives |
| the image calls SYS_EXIT | 0 for ADP_Stopped_ApplicationExit, else 1 |
| the image stores 0x5555 to virt's test device | 0 |
| the image stores 0x3333 to the test device | the upper halfword stored |
| the image requests a reset (AIRCR.SYSRESETREQ) | 0 |
| a branch to itself, or a WFI, that no exception or interrupt can interrupt | 0 |
| an AVR SLEEP that nothing can wake (the AVR harness ends in such a loop) | 0 |
| the output contains `--until`'s string | 0 |
| the core locks up (a RISC-V trap whose vector cannot be fetched; an AVR instruction the part does not have) | 3 |
| `--max-insns` instructions have run | 4 |
| the image cannot be loaded, or an option is wrong | 2 |

A run that ends at a lockup or at `--max-insns` says why on stderr.

## Debugging: `--gdb`

`--gdb PORT` serves the GDB remote protocol on `localhost:PORT` while the
image runs. `--gdb-wait` holds the core at reset until a debugger
connects, as QEMU's `-S` does. Without it the image runs, and a debugger
that connects stops it where it is; an image that ends first ends EmbSim
as without `--gdb`, but one that waits in a WFI or a loop nothing can
interrupt waits there for the debugger. `HOST:PORT` listens on another
address, and QEMU's `tcp::PORT` is accepted too.

```sh
embsim fw.elf --board mps2-an386 --gdb 1234 --gdb-wait &
gdb fw.elf -ex 'target remote localhost:1234'
lldb fw.elf -o 'gdb-remote localhost:1234'
embdbg fw.elf remote 1234
```

The server answers as QEMU's stub does, so a debugger sees the same
machine on either, and a script written for one runs on the other:

- **Registers.** On a Cortex-M: r0 to r12, sp, lr, pc and xpsr; d0 to
  d15 and fpscr on a core with an FPU (gdb shows s0 to s31 from them);
  and msp, psp, primask, control, and on ARMv7-M basepri and faultmask.
  The target description gdb reads has the FPU only when the core has
  one. On RISC-V: x0 to x31 and pc, f0 to f31 at 64 bits, priv, and the
  CSRs EmbSim has (fflags, frm and fcsr among them), each numbered as
  QEMU's stub numbers it (a CSR is 66 plus its number). QEMU's
  description lists the supervisor's and hypervisor's CSRs too, which
  EmbSim does not have, so `info all-registers` differs there. On the
  AVR: r0 to r31, SREG, SP and the PC (a byte address), and memory as
  gdb's AVR target numbers it -- flash at 0, the data space at
  `0x800000`.
- **Breakpoints**, software and hardware alike. The server keeps them;
  it never writes into the image, so they work in flash.
- **Watchpoints**: `watch`, `rwatch` and `awatch`. As on QEMU, the
  target stops before the access, and gdb steps it (on the AVR, after
  it, as gdb's AVR target expects; QEMU's AVR stub has no working
  watchpoints to referee them against).
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

On the Cortex-M:
- The MPU (MemManage faults).
- ARMv8-M (TrustZone, the Cortex-M23/M33).
- FPv5's VSEL, VMAXNM, VMINNM and VRINT.
- The half-precision and fixed-point VCVT forms.
- The ARMv7E-M parallel add and subtract instructions.
- Peripherals beyond the UART, SysTick, the NVIC and DWT's cycle counter.

Each instruction among these is a UsageFault, so a run that needs one
stops at a named fault rather than computing something wrong.

On the AVR:
- Timer/Counter0 and 2, SPI, TWI, the ADC, the analog comparator, EEPROM,
  the watchdog, the external and pin-change interrupts, the USART's
  receiver, and the power reduction register. Their registers keep what
  is written (on QEMU they read as zero), and their interrupts never
  come.
- The output-compare pins and input capture from a pin.
- Self-programming (SPM) and the boot section; the other AVR parts (the
  ATmega2560's 3-byte program counter, ELPM and RAMPZ).
- The sleep modes beyond Idle.

On RISC-V:
- Supervisor mode and virtual memory, and the hypervisor.
- Physical memory protection: the PMP registers are kept, not
  enforced.
- The PLIC, and so external interrupts; virt's RTC, virtio and PCIe.
- RISC-V semihosting.
- mcountinhibit is kept but does not stop the counters.
- Extensions beyond IMAFDC, Zicsr and Zifencei (an illegal instruction
  each, as above).

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

**RISC-V.** `tests/golden/embsim-riscv.sh` does the same on virt with
qemu-system-riscv32 and -riscv64: the exec corpus on RV32 with ilp32
(-O0, -O2, -Os) and ilp32d (-O0, -O2), and on RV64 with lp64 (-O0,
-O2) and lp64d (-O0, -O2, -Os), 2425 programs, each with the output,
the test device's status, the instruction count and the cycles QEMU
gives. QEMU stops its guest some way after the test device asks it to,
so the plugin is told where the driver's last function starts and
counts to there. `tests/golden/embsim/rv-isa.c` prints the edges: M's
division by zero and overflow, the high multiplies, the compressed
forms, misaligned accesses, LR/SC and the AMOs, traps and their CSRs,
and every F and D operation in every rounding mode over a table of
values with its flags. And the ends of a run: the test device, the
lockup, the idle loop and WFI, the timer interrupt waking WFI.
`embsim-gdb.sh` runs its gdb session on RV32 and RV64 against QEMU's
virt stub too.

**AVR.** `tests/golden/embsim-avr.sh` does the same on QEMU's uno: the
exec corpus at -O0, -O2 and -Os, every program that builds for the AVR
without libc (some 580: a few more or fewer as QEMU finishes the longest
in its time), with the output and the instruction count QEMU gives. QEMU's AVR runs a block again from a data access it must redo,
so the plugin counts its instructions one at a time, skipping a repeat
of the same instruction (which only a jump to itself does honestly);
and it counts an instruction a skip passes over, which EmbSim counts
apart. QEMU has no cycle model, so `tests/golden/embsim/avr-cycles.S`
holds the cycles to the datasheet: each of its lines says what it
takes, and EmbSim's count must add up to it. `avr-isa.S` and
`avr-isa.c` print the edges -- SREG after every arithmetic instruction
over a table of operands, the multiplies, the 16-bit forms, the skips,
the branches, the addressing modes, LPM, Timer/Counter1's interrupt --
which must be QEMU's. And `embsim-gdb.sh` runs its gdb session on the
AVR against QEMU's uno stub.
