# embsim — run a firmware image without a board

`embsim` runs a linked ELF image, or an Intel HEX one, the way the part
does: a Cortex-M (ARMv6-M, ARMv7-M), a RISC-V core (RV32 and RV64,
IMAFDC), an AVR (the ATmega328P), a MIPS32 Release 2 core (QEMU malta's
24Kc, a PIC32MZ's) or a PIC24F (the PIC24FJ1024GB610). It is ISO C
with no dependencies, so it builds on any machine, including one
without QEMU. It counts the instructions it executes and estimates the
cycles they take, and gdb, lldb or embdbg can debug the image while it
runs. Given the part's CMSIS-SVD file, it puts every peripheral register
the file describes on the bus, and traces a driver's accesses to them by
name. This page is the command reference; how EmbSim is built inside,
and how to add a core, a peripheral or a board, is
[EmbSim's internals](../../internals/embsim.md).

## Synopsis

```text
embsim IMAGE.elf|IMAGE.hex [--board NAME] [--cpu NAME] [--ram-size SIZE]
                 [--until STRING] [--max-insns N] [--stats]
                 [--count FILE] [--trace FILE] [--no-semihosting]
                 [--gdb [HOST:]PORT [--gdb-wait]]
                 [--svd FILE.svd] [--trace-periph[=NAME,...]]
                 [--coverage FILE [--coverage-format=text|lcov]]
                 [--profile[=FILE] [--profile-format=report|collapsed|collapsed-insns]]
                 [--stack-report[=FILE]] [--stack-limit ADDR|SYMBOL]
                 [--stack-su FILE.su]... [--stack-embrt FILE.json] [FILE.su...]
                 [--fault-report[=FILE]] [--input FILE|-] [--record FILE]
                 [--dfp DIR] [--set NAME=VALUE]...
embsim [IMAGE.elf] --replay FILE [--count FILE] [--stats] [...]
embsim --svd FILE.svd --svd-map
```

```sh
embcc --target=thumbv7em-none-eabihf --opt=speed -c main.c
embld -e reset -Ttext 0 -Tdata 0x20000000 boot.o main.o -o fw.elf
embsim fw.elf --board mps2-an386 --stats

embcc --target=riscv64-unknown-elf --isa=rv64gc --abi=lp64d --opt=speed -c main.c
embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 boot.o io.o main.o -o fw.elf
embsim fw.elf --board virt --stats

embcc --target=avr --opt=size -c main.c
embld -e __vectors -Ttext 0 -Tdata 0x100 boot.o io.o main.o librt.a -o fw.elf
embsim fw.elf --board uno --stats

embsvd STM32F405.svd --header STM32F405.h --startup startup.c \
    --ld f405.ld --flash 0x08000000:1M --ram 0x20000000:128K
embcc --target=thumbv7em-none-eabi --opt=speed -c startup.c main.c
embcc --target=thumbv7em-none-eabi --link-script=f405.ld startup.o main.o -o fw.elf
embsim fw.elf --svd STM32F405.svd --trace-periph=USART2,RCC

embcc --target=mipsel-none-elf --mcu=32MZ2048EFH100 --dfp=$DFP *.c *.S -o fw.elf
embsim fw.elf --board explorer16-32 --dfp $DFP --set temperature=30

embsim thermo.hex --board explorer16-32-pic24 --dfp $DFP24 --set S3=pressed
```

The image is an ELF file (its PT_LOAD segments), or an Intel HEX file
(MPLAB's production `.hex`, an objcopy `-O ihex`), whose records are
placed at their addresses on EmbSim's bus.

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
| `stm32f405` | Cortex-M4 with FPv4-SP | 1 MiB flash at `0x08000000` (and at 0), 128 KiB SRAM at `0x20000000`, 64 KiB CCM at `0x10000000` | the USARTs of its SVD ([below](#the-stm32f405)) |
| `virt` | RISC-V, RV32 or RV64 with IMAFDC | 128 MiB RAM at `0x80000000` | NS16550A at `0x10000000` |
| `uno` | ATmega328P (AVR5) | 32 KiB flash, 2 KiB SRAM at data `0x100` | USART0 at data `0xC0` |
| `malta` | MIPS32r2 (24Kc), little-endian | 64 MiB RAM at 0 (`0x80000000` in kseg0) | NS16550A at `0x1F000900`, registers 8 bytes apart |
| `pic32mz2048efh100` | PIC32MZ EF (MIPS32r2, EIC mode, shadow sets) | 512 KiB RAM at 0, 2 MiB flash at `0x1D000000`, boot flash at `0x1FC00000` | UART1-6, of the part's ATDF ([below](#pic32-parts-the-explorer-1632-and-the-curiosity)) |
| `explorer16-32` | the same part, on Microchip's Explorer 16/32 | as `pic32mz2048efh100` | the same, and the board's LEDs, buttons, sensors and LCD |
| `curiosity-pic32mz-ef-2` | PIC32MZ2048EFM144, on Microchip's Curiosity PIC32MZ EF 2.0 | as `pic32mz2048efh100` | the same, and the board's LEDs and switches |
| `pic24fj1024gb610` | PIC24FJ1024GB610 (the PIC24F core, with EDS) | 1 MiB of program flash, 32 KiB of RAM at data `0x0800` | UART1-6, of the part's EDC ([below](#pic24-parts-and-the-explorer-1632s-pic24-pim)) |
| `explorer16-32-pic24` | the same part, on the Explorer 16/32 with its PIC24FJ1024GB610 PIM | as `pic24fj1024gb610` | the same, and the board's LEDs, buttons, sensors, LCD and EEPROM |

- `--cpu` runs another core on the board: `cortex-m0`, `cortex-m0plus`,
  `cortex-m3`, `cortex-m4` or `cortex-m7` on the Cortex-M boards; `rv32`
  or `rv64` on `virt`, which otherwise takes the image's width (an
  ELFCLASS64 image is RV64); `atmega328p` on `uno`; `24kc` on `malta`;
  `pic32mz` on the PIC32 boards; `pic24fj1024gb610` on the PIC24 ones.
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
- On `malta` the core starts at `0xBFC00000`, in a boot ROM that jumps
  to the image's entry, as QEMU's `-kernel` does; SOFTRES (`0x1F000500`)
  written with `0x42` resets the board, which ends the run, as QEMU's
  `-no-reboot` does. A MIPS image's segments are loaded at the physical
  address of their kseg0 or kseg1 address.
- On the PIC24 boards EmbSim's bus holds both of the part's spaces:
  program memory at twice its address (each 24-bit word the four bytes
  an XC16 image and its HEX file give it: its three and a phantom 0),
  the configuration memory at twice `0x800000`, and the data space at
  `0x2000000` plus its Extended Data Space address. An ELF segment goes
  to twice its load address; a HEX record to its address as written.
- A segment of the image outside the board's memory gets memory of its
  own, so a link script for a similar part still runs.
- Flash on the lm3s6965, the micro:bit and the STM32F405 ignores
  stores, as flash does. On the MPS2 boards it is SSRAM, which takes
  them.

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
  and DWT, the peripheral space reads as zero and ignores writes --
  unless an SVD file describes it (`--svd`, below).

## Peripherals from an SVD file: `--svd`

`--svd FILE.svd` reads the part's CMSIS-SVD file -- the one its vendor
publishes, and embsvd writes the device header from -- and puts each of
its peripherals' registers on the bus at the address the file gives.
A register then behaves as the file says its fields do:

| The file says | A read | A write |
|---|---|---|
| `resetValue` | the value until written, and again after a reset | |
| `read-write` (or nothing) | the value | sets the field |
| `read-only` | the value | ignored |
| `write-only` | 0 | sets the field (a model may use it) |
| `writeOnce` | 0 | the first write after reset sets it; the rest are ignored |
| `read-writeOnce` | the value | the first write after reset sets it |
| `modifiedWriteValues` `oneToClear`, `oneToSet`, `oneToToggle` | | each 1 written clears, sets or toggles its bit; a 0 leaves it |
| `zeroToClear`, `zeroToSet`, `zeroToToggle` | | the same for each 0 written |
| `clear`, `set` | | any write clears or sets the whole field |
| `readAction` `clear`, `set` | the value, and then the field is cleared or set | |
| bits no field covers | their reset value | ignored |

- A byte or halfword access reads or writes those bytes alone; a word
  access that spans two narrow registers reaches both.
- Clusters, register arrays (`dim`, `dimIncrement`, `dimIndex`) and
  `derivedFrom` -- of a register, a cluster, a field or a peripheral --
  are placed as embsvd places them in its header: EmbSim reads the file
  with embsvd's reader.
- **An access where no register is is a bus fault** -- a hole in a
  peripheral, or the space between two -- and stderr says where, since a
  HardFault handler rarely does:

  ```text
  embsim: bus fault: a word read at 0x40004440 (pc 0x08000398): USART2 has no register at +0x040
  embsim: bus fault: a word write at 0x40008000 (pc 0x080003b2): no peripheral of STM32F405 is there
  ```

  The space covered is the file's peripherals', from the lowest register
  to the highest; outside it, the board's map answers as without
  `--svd`. On the part, a hole inside a peripheral's block usually reads
  as 0 instead: the fault is EmbSim's rule, to catch a wrong offset.
- Two registers at one address -- an `alternateRegister`, or Nordic's
  peripherals that share their space (SPI0, SPIM0, TWI0...) -- are one
  register, named by the first in the file.
- The core's own peripherals (NVIC, SCB, SysTick, the MPU: anything at
  `0xE0000000` and up) are the core's, not the file's.
- A debugger's reads (`x` in gdb) have no side effect: no `readAction`,
  no trace.
- A peripheral with no model is that alone: its registers keep what is
  written, as the file allows, and nothing else happens. The models of
  [the STM32F405](#the-stm32f405) are what make a peripheral do
  something.

`--svd` works on any Cortex-M board: the file's peripherals go before
the board's space that reads as zero, and the board's own UART still
answers where it is. A file whose name is a board's (`STM32F405.svd`)
picks that board unless `--board` says otherwise.

`--svd-map` lists the registers EmbSim made from the file, without
running anything, one a line: the peripheral, the address, the size in
bits, the register's name and its path, the reset value and the access;
and, for a register not on the bus, why (`-- is P.R`: another register
at its address; `-- the core's`).

### Tracing a driver: `--trace-periph`

`--trace-periph` prints, on stderr, each access to the peripherals' registers by
the SVD's names; `--trace-periph=NAME,...` only those named, as
`PERIPHERAL` or `PERIPHERAL.REGISTER`, with `*` for any characters
(`--trace-periph=USART*,GPIOA.ODR`). Each line has the instruction's
address and what happened:

```text
periph 080002fc W USART2.CR1.UE 0->1
periph 080002fc W USART2.CR1.TE 0->1
periph 08000276 R USART2.SR 0x000000c0
periph 08000246 W GPIOA.BSRR.BS5=1
periph 08000246 H GPIOA.ODR.ODR5 0->1
periph 08000312 W RCC.CR.HSEON 0->1
periph 08000312 H RCC.CR.HSERDY 0->1
periph 00000a1c W UARTE0.ENABLE.ENABLE 0x0->0x8 (Disabled->Enabled)
periph 08000446 W USART3.BRR 0x00000683 ignored: its clock is off (RCC.APB1ENR.USART3EN)
```

- `W`: the program wrote; a line for each field that changed, or for a
  write-only field, what was written to it (`BS5=1`). A write that
  changed nothing says so (`(no change)`, `ignored: read-only`).
- `R`: the program read; the register's value.
- `H`: the hardware changed a field -- a model (a ready bit, ODR from
  BSRR, a timer's flag), or a `readAction` (`(by the read)`).
- A field's value is 0 or 1, or hex when it is wider, with its
  enumerated value's name when the file gives one.

### The STM32F405

The `stm32f405` board is QEMU's netduinoplus2: 1 MiB of flash at
`0x08000000`, seen at 0 too (where the core reads its vector table),
128 KiB of SRAM, 64 KiB of CCM RAM, a Cortex-M4 with FPv4-SP and 4 NVIC
priority bits. Its peripherals are ST's `STM32F405.svd` (in the
cmsis-svd-data project), which EmbSim finds with `--svd FILE` or in the
directories of `EMBSIM_SVD_PATH` (separated by `:`):

```sh
EMBSIM_SVD_PATH=$HOME/svd embsim fw.elf --board stm32f405
```

Over its registers, four models, found by the SVD's names (`RCC`,
`GPIO*`, `USART*` and `UART*`, `TIM*`):

- **RCC.** A ready bit follows its enable: HSIRDY, HSERDY, PLLRDY and
  PLLI2SRDY in CR, LSERDY in BDCR, LSIRDY in CSR are set the moment the
  oscillator or PLL is turned on, and cleared when it is off. CFGR's SWS
  follows SW. **The clock gates:** a peripheral whose enable bit
  (`USART2EN` in `APB1ENR`...) is clear reads as 0 and ignores writes,
  as the part's do, and the first such access is a warning:

  ```text
  embsim: warning: a write at pc 0x08000446: USART3.BRR is ignored while its clock is off (RCC.APB1ENR.USART3EN is 0)
  ```

- **GPIO.** BSRR sets and resets ODR's bits (set wins when both are
  written). IDR is what each pin reads from its configuration, since
  nothing outside drives the pins: an output reads what ODR drives (an
  open-drain one at 1 reads its pull), an input or alternate-function
  pin reads 1 when pulled up, else 0, an analog pin 0. Out of reset
  GPIOA's IDR is `0xa000`: PA13 and PA15, the debug port's, pulled up.
- **USART1-6, UART4-5.** SR resets to `0xc0` (TXE and TC; ST's file says
  `0x00c00000`); a write to DR with CR1's UE and TE set sends the byte
  to stdout at once, so TXE stays set and TC sets; TC, RXNE, LBD and CTS
  are cleared by writing 0; the TXE, TC and RXNE interrupts are raised
  through the NVIC when enabled. Reception is not modelled (the board's
  console for `--input` is the core's boards' UARTs, above).
- **TIM1-14**, their time base: CNT counts up while CEN is set, one
  count per PSC + 1 clocks of the timer's clock; past ARR it returns to
  0 and that is an update event: UIF, and the interrupt with UIE (the
  file's, `TIM1_UP_TIM10` for TIM1). PSC takes effect at the update
  event, ARR too with ARPE; EGR.UG reinitializes the counter and makes
  one (setting UIF unless URS); UDIS and OPM. The timer's clock is the
  core's, or half of it with APB1 at /4 (the APB prescaler in RCC's
  CFGR: x2 when it is not 1). A timer whose clock is gated off does
  not count.
- SysTick is the core's, as on every Cortex-M board.

QEMU's netduinoplus2 is the referee where QEMU models the peripheral,
and the reference manual (RM0090) where it does not:

| | QEMU 11.1 | EmbSim, as RM0090 |
|---|---|---|
| RCC, GPIO | not modelled: they read 0 | modelled, above |
| a peripheral whose clock is off | works anyway | reads 0, ignores writes |
| USART SR written with ones | they are set (`0x3ff`) | rc_w0 flags and read-only ones keep their value |
| TIM EGR.UG | UIF stays 0 | UIF set (URS 0) |
| TIM CNT with CEN clear | keeps counting | holds |
| TIM update interrupt | its alarm is armed by writes to CNT, ARR, PSC or EGR, not CEN; none came in the configurations tried | every update with UIE |
| a hole in a peripheral's block (USART2 + 0x40) | reads 0 | a bus fault (EmbSim's rule) |
| USART SR, BRR and CR1, TC cleared, the TXE interrupt; TIM2's CR1, PSC and ARR, UIF cleared, CNT counting; a reserved address's bus fault | as EmbSim | as QEMU |

Out of reset, the timer and the core see the same clock; EmbSim does not
model the clocks' frequencies themselves -- its time is the core's
cycles, whatever the PLL is set to.

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

## PIC32 parts, the Explorer 16/32 and the Curiosity

QEMU has no PIC32 board; EmbSim's are where EmbCC's PIC32 output runs
(`embcc --mcu=32MZ2048EFH100`, [MIPS32](../embedded.md#mips32)). A PIC32 board is
built from the part's device pack -- Microchip's
`Microchip.PIC32MZ-EF_DFP`, which MPLAB X installs and
packs.download.microchip.com serves -- found with `--dfp DIR` (the
pack's directory, which holds `atdf/PIC32MZ2048EFH100.atdf`), `--svd
FILE.atdf`, or a directory holding the file in `EMBSIM_SVD_PATH`:

```sh
embsim fw.elf --board explorer16-32 --dfp ~/.mchp_packs/Microchip/PIC32MZ-EF_DFP/1.4.168
```

**The registers.** The pack's ATDF file (the "AVR tools device file"
Microchip writes for its PIC32, SAM and AVR parts alike) describes every
register of the part; EmbSim reads it as it reads an SVD file, and each
register behaves as [above](#peripherals-from-an-svd-file---svd): 71
peripherals and 1608 registers on the PIC32MZ2048EFH100, 4230 with their
CLR, SET and INV registers. Two things are a PIC32's:

- Each register the pack's processor module gives CLR, SET and INV
  registers (`xc32/<part>/p<part>.S`, beside `atdf/`) has them at +4,
  +8 and +0xC: a write clears, sets or inverts its bits in the register,
  as one write of the result, so a model sees `T2CONSET = 0x8000` as
  T2CON written with ON set. They read as 0. Every SFR and each of these
  is at the address the pack's device header gives it, but USBCRCON,
  where Microchip's two files disagree (EmbSim follows the ATDF).
- An address in the SFR space where no register is reads as 0 and
  ignores writes, as the part's do: a startup writes OFFn's whole table,
  over the vectors the part does not implement.

Where one ATDF register group names every instance's registers (Timer2
to Timer9 in one, UART1 to UART6 in one), each instance is a peripheral
of its own: `TMR3`, `UART2`. `--svd-map` lists them, `--trace-periph`
traces them by these names.

**The core** is the `pic32mz` model of the [MIPS32
core](#what-it-models-the-mips32-core), a PIC32MZ EF's: PRId 0x0001A700
(the M5150), Config1 with a 16-entry TLB, 16 KiB and 4 KiB 4-way caches
of 16-byte lines (`cache` does nothing), Config3 with EIC and vectored
interrupts, DSP and DSPr2, MIPS32 and microMIPS (MIPS32 at reset); seven
shadow register sets (SRSCtl.HSS 7); Status.MX and CU1 writable. The
interrupts come from the EVIC in EIC mode. Its FPU is modelled: XC32's
hard-float (FP64) images run as EmbCC's soft-float ones do -- the
Thermometre's XC32 build shows the same temperature on the LCD as its
EmbCC build.

**The models** over the registers:

- **EVIC.** An interrupt is requested while its flag (IFSx), its enable
  (IECx) and a priority (IPCx: IP 1-7) are set; the highest priority
  is presented, then the highest subpriority, then the lowest number.
  The core takes it when its priority is above Status.IPL: at EBASE +
  OFFn of its vector in multi-vector mode (INTCON.MVEC), else EBASE +
  0x200, in the shadow register set PRISS gives its priority; INTSTAT
  says which. The core's timer (Count = Compare) and its two software
  interrupts (Cause.IP0, IP1) set CTIF, CS0IF and CS1IF as they rise.
- **SYSKEY and the oscillator.** Writing 0xAA996655 then 0x556699AA to
  SYSKEY unlocks OSCCON, SPLLCON and the PBxDIV registers; a write to
  one while locked is ignored, as on the part, and the first is a
  warning. OSCCON starts with the clock the configuration words choose
  (DEVCFG1.FNOSC); setting OSWEN switches at once. PB1-PB5 and PB8
  divide by 2 out of reset, PB7 by 1; CLKSTAT says every clock is
  ready.
- **Timer1 to Timer9.** Counting while ON, at peripheral bus 3's clock
  (the core's cycles over PB3DIV) over the prescaler; at PRx, back to 0
  and the interrupt flag. Timer2 to Timer9 pair into 32-bit timers
  (T32), with the odd timer's interrupt. PRx is 0xFFFF out of reset.
- **Ports A-K.** All inputs and every analog-capable pin analog out of
  reset (TRISx, ANSELx: the pins the pack's device header names, as the
  data sheet's reset values have them; the ATDF lists all sixteen).
  PORTx reads the pins -- an output what LATx drives, an input what the
  board drives on it or its pull-up (CNPUx), an analog pin 0 -- and a
  write to PORTx writes LATx.
- **UART1-6.** A byte written to UxTXREG with ON and UTXEN goes to
  stdout at once (TRMT stays set); the transmit interrupt is persistent
  while there is room; the run's input (`--input`) arrives at the first
  UART receiving.
- **PMP**, in its master modes: a write to PMDIN is a write cycle to the
  board (the address on the PMA lines PMAEN enables, the data), a read
  of PMDIN gives the previous read cycle's data and starts the next;
  BUSY for the wait states (WAITB, WAITM, WAITE, at PB2's clock), then
  the interrupt with IRQM 01, and PMADDR stepped with INCM.
- **ADCHS**, enough to convert an input and read it: turning the ADC on
  makes its reference ready (BGVRRDY), ANENx makes ADCx ready (WKRDYx);
  the global software trigger (GSWTRG) converts each input whose
  trigger source it is, and the scan list with STRGSRC 1; RQCNVRT
  converts ADINSEL's. The result is the board's voltage against 3.3 V
  at the resolution SELRES gives, in ADCDATAn with ARDYn (reading it
  clears ARDYn) and, enabled, its interrupt. ADC0-4 convert their
  alternate inputs AN45-AN49 with ADCTRGMODE.SHxALT 01.

**The `explorer16-32` board** is Microchip's Explorer 16/32 (DM240001-3)
with the PIC32MZ EF plug-in module (MA320019). Its pins are the PIM's, as
its information sheet maps the board's 100-pin socket to the part:

| | Pin | |
|---|---|---|
| LEDs D3-D10 | RA0, RA1, RA2, RA3, RF13, RF12, RA6, RA7 | lit when the pin drives high |
| buttons S4, S5, S6 | RF8, RA7 (with D10), RD13 | low when pressed (the board's pull-ups); S3 is not wired on this PIM |
| TC1047A temperature sensor | AN4 (RB4) | 500 mV + 10 mV per degree C |
| potentiometer R6 | AN45 (RB5) | ADC0's alternate input |
| 16x2 LCD, HD44780 | PMD0-7, RS on PMA0, E on PMWR, R/W on PMRD | the PMP in master mode |
| USB-UART | the UART the program uses | every UART's output is stdout |

The run reports on stderr each LED that goes on or off, and the LCD's
two lines each time the program clears the display or sends its cursor
home -- the frame it had drawn -- and when the run ends:

```text
explorer16-32: LED D5 on (RA2) at 502.378 ms
explorer16-32: LCD |Temp:24.95 C    | at 1004.377 ms
explorer16-32:     |                |
```

The times are the core's cycles at 200 MHz. The LCD is an HD44780: its
instruction set (clear, home, entry mode, display on/off, cursor and
display shift, function set with 8- or 4-bit interface and one or two
lines, the CGRAM and DDRAM addresses), its 80-byte display RAM with line
2 at 0x40, never busy.

`--set NAME=VALUE` gives the board's inputs: `temperature=DEGREES` (25
by default), `pot=VOLTS` or `pot=PERCENT%` (1.65 V by default), and
`S4`, `S5` or `S6` `=pressed` or `=released`.

**The `curiosity-pic32mz-ef-2` board** is Microchip's Curiosity PIC32MZ
EF 2.0 (DM320209), with a PIC32MZ2048EFM144 (`embcc
--mcu=32MZ2048EFM144`, the same pack). Its pins are those of Harmony 3's
board support package for it:

| | Pin | |
|---|---|---|
| LED1, LED2, LED3 | RJ7, RK7, RJ3 | lit when the pin drives low |
| RGB LED: red, green, blue | RB7, RB8, RB9 | lit when the pin drives low |
| switches SW1-SW4 | RJ4, RJ5, RJ6, RC14 | low when pressed; the board has no pull-ups, so a released switch reads its pin's own (CNPUx), else 0 |
| USB-UART | the UART the program uses | every UART's output is stdout |

An LED lit low is off while its pin is an input or an open drain let
go. `--set SW1=pressed` (to `SW4`, `=pressed` or `=released`) gives the
switches:

```text
$ embsim fw.elf --board curiosity-pic32mz-ef-2 --dfp $DFP --set SW1=pressed
curiosity-pic32mz-ef-2: LED LED1 on (RJ7) at 0.052 ms
curiosity-pic32mz-ef-2: LED RGB green on (RB8) at 0.052 ms
```

**Not modelled on the PIC32:** microMIPS (`-mmicromips` code), the
DSP ASE beyond its registers, the MCU ASE's IRET, the peripherals other
than these (their registers keep what is written), change-notification
interrupts, the external interrupt pins, conversion and baud-rate
timing, DMA, the watchdog and deadman timer, the prefetch cache, PMD's
module disables, sleep and idle.

## What it models: the MIPS32 core

- **The instruction set.** MIPS32 Release 2, little-endian: every
  integer instruction, the branches and jumps with their delay slots
  (the likely branches skip theirs when not taken), mul, madd, msub and
  the HI/LO multiplies and divides, clz and clo, ext and ins, seb, seh,
  wsbh, rotr and rotrv, the traps, ll and sc, lwl, lwr, swl and swr,
  rdhwr; cache, pref, sync and synci do nothing. A divide by zero, or
  of INT_MIN by -1, leaves LO the dividend and HI 0, as QEMU's does (the
  architecture leaves them unpredictable). A floating-point instruction
  is a Coprocessor Unusable exception on the 24Kc, which has no FPU; an
  encoding no MIPS32r2 core has is a Reserved Instruction exception.
- **The FPU** (`pic32mz`, which `--cpu pic32mz` puts on malta too):
  coprocessor 1 with 64-bit registers (Status.FR) or paired 32-bit ones,
  single and double precision and the word and long-word conversions,
  in IEEE 754-2008 mode (FCSR.NAN2008, ABS2008): loads and stores, the
  indexed ones too, the moves, add to sqrt, recip and rsqrt, abs and neg
  (the sign alone), the unfused multiply-adds, cvt, round, trunc, ceil
  and floor (a NaN gives 0, an overflow the largest integer of its
  sign), c.cond into FCC0-FCC7, bc1t and bc1f, FCSR's Cause, Flags and
  Enables and the floating-point exception. The arithmetic is IEEE
  754's in integers, the RISC-V core's: a NaN an operation makes is the
  default one (QEMU propagates an operand's payload; both are the
  architecture's). Paired singles and flush-to-zero are not modelled.
- **Coprocessor 0.** Status, Cause, EPC, ErrorEPC, BadVAddr, EBase,
  IntCtl, SRSCtl, SRSMap, Count and Compare, Config to Config3, PRId,
  HWREna, UserLocal, LLAddr, the TLB's registers and the debug ones;
  di, ei, eret, deret, wait, rdpgpr and wrpgpr.
- **Exceptions** are the privileged architecture's: EPC and Cause.BD
  for an instruction in a delay slot, Status.EXL and ERL, the boot
  vectors at `0xBFC00200` while Status.BEV is set and EBase's after,
  the TLB refill vector, BadVAddr, Cause.CE for a coprocessor that is
  not usable.
- **Memory.** kseg0 and kseg1 reach the bus's low 512 MiB unmapped;
  kuseg, kseg2 and kseg3 go through a 16-entry TLB (tlbr, tlbwi, tlbwr,
  tlbp), and kuseg is unmapped while Status.ERL is set. Loads and
  stores must be aligned (but for lwl, lwr, swl, swr): an address error
  otherwise. An access to nothing is a bus error exception. sc succeeds
  when ll's address still holds the value ll read (QEMU's rule), and
  eret clears the link.
- **Interrupts.** Compatibility mode (Cause.IP against Status.IM, the
  timer on IP7) and vectored mode (IntCtl.VS), the hardware lines
  IP2-IP7, and the two software interrupts; on a PIC32, EIC mode: the
  EVIC's level against Status.IPL (8 bits: Status bits 18 and 16-10),
  captured in Cause.RIPL as the interrupt is taken, its handler at
  EBASE plus the EVIC's offset, its shadow register set.
- **Shadow register sets** (`pic32mz`): an exception or interrupt saves
  SRSCtl.CSS in PSS and switches to ESS (an exception), the EVIC's set
  (EIC mode) or SRSMap's (vectored mode); eret switches back; rdpgpr
  and wrpgpr reach the previous set.
- **The DSP ASE** (`pic32mz`), what an RTOS saves and restores: HI and
  LO of $ac1-$ac3 (mfhi, mflo, mthi, mtlo, the multiplies), rddsp and
  wrdsp, lwx, lhx, lbux and bposge32, each a DSP State Disabled
  exception while Status.MX is clear; the rest of the ASE ends the run
  naming the instruction. Count counts every other
  cycle; Compare's match sets Cause.TI. wait sleeps until an interrupt:
  it skips ahead to the device that will raise one, and ends the run
  when nothing can.
- **sdbbp** enters debug mode, which needs a probe: with none, the run
  ends as a lockup at the breakpoint (startups put one in a default
  handler, for a debugger to stop at).

## PIC24 parts, and the Explorer 16/32's PIC24 PIM

QEMU has no PIC24 either. A PIC24 board is built from the part's device
pack -- `Microchip.PIC24F-GA-GB_DFP`, from MPLAB X or
packs.download.microchip.com -- found with `--dfp DIR` (the pack's
directory, which holds `edc/PIC24FJ1024GB610.PIC`), `--svd FILE.PIC`,
or `EMBSIM_SVD_PATH`. An image is XC16's: its ELF file, or the HEX file
MPLAB X writes beside it.

```sh
embsim dist/default/production/thermo.X.production.hex \
    --board explorer16-32-pic24 --dfp ~/.mchp_packs/Microchip/PIC24F-GA-GB_DFP/1.9.336
```

**The registers.** The pack's EDC file (MPLAB's description of a 16-bit
part) gives every special function register: 664 on the
PIC24FJ1024GB610, each at the address the pack's linker script
(`xc16/support/PIC24F/gld/p24FJ1024GB610.gld`) gives it, with its value
at power-on and the fields of the data sheet's names. Each behaves as
[above](#peripherals-from-an-svd-file---svd); the unimplemented bits
read 0, and the SFR space where no register is reads 0 and ignores
writes, as the part's does. The file names no peripherals: a run of
registers of one module is one, by its first register's name -- `INT`
(the interrupt controller), `OSC`, `TMR` (Timer1-5), `UART` (UART1-6),
`SPI` (SPI1-3), `GPIO` (ports A-G), `ADC1`, `PMP`, `PPS`, `RTCC`, and so
on, as `--svd-map` lists them. The core's registers (W0-W15, SPLIM, PCL,
PCH, DSRPAG, DSWPAG, RCOUNT, SR, CORCON, DISICNT, TBLPAG) are the core's.

**The models:**

- **The interrupt controller.** An interrupt is requested while its
  flag (IFSx), its enable (IECx) and a priority (IPCx: 1-7) are set and
  INTCON2.GIE is; the highest priority is presented, then the lowest
  number, and the core takes it when it is above its own. INTTREG says
  which; INTCON1 gets the traps' status bits; NSTDIS stops nesting.
- **The oscillator.** OSCCON starts on the configuration words' FNOSC;
  its bytes take a write only right after their unlock sequence (0x46,
  0x57 to the low byte; 0x78, 0x9A to the high one), as XC16's
  `__builtin_write_OSCCONL` and `_OSCCONH` write them, and a write
  without one is ignored with a warning. OSWEN switches at once. The
  core runs at half the oscillator: the 8 MHz FRC, the FRC divided by
  CLKDIV.RCDIV (OSCFDIV), or 32 MHz from the 96 MHz PLL over
  CLKDIV.CPDIV -- the time the board reports.
- **The Peripheral Pin Select** registers ignore writes while
  OSCCON.IOLOCK is set.
- **Timer1 to Timer5**, at the instruction clock over TCKPS (1, 8, 64,
  256), their flag at the match with PRx; Timer2/3 and Timer4/5 pair
  into 32-bit timers (T32), TMR3HLD and TMR5HLD latched by a read of
  the low half.
- **Ports A-G**, as the PIC32's (ANSx and IOCPUx for ANSELx and CNPUx).
- **UART1-6.** A byte written to UxTXREG with UARTEN and UTXEN goes to
  stdout at once; the transmit interrupt is flagged as the buffer has
  room again; the run's input arrives at the first UART on.
- **The EPMP**, in master mode: its chip selects' regions of the
  Extended Data Space (PMCSxBS), reached through the EDS window, are bus
  cycles to the board: the word offset of the address on the PMA lines
  PMCON4 enables, the low byte on an 8-bit port (one cycle a word, as
  Microchip's own Explorer 16/32 LCD driver depends on), 16 bits on a
  16-bit one.
- **ADC1**: SAMP or ASAM start a sample, the program's clearing of SAMP
  (SSRC 0000) or SAMC TADs (SSRC 0111) the conversion, 12 TADs (14 with
  MODE12); CH0SA's input, or with CSCNA the next of AD1CSSL/AD1CSSH's;
  the result against AVDD (3.3 V) in FORM's format, into the FIFO or the
  input's own buffer (BUFREGEN), and the ADC1 interrupt after SMPI + 1.
  AN28 is the band gap (1.2 V), AN29 AVSS, AN30 AVDD.
- **SPI1-3**, as masters: a byte written to SPIxBUFL is exchanged at
  once with the slave the board has on the unit's pins, and read back
  from SPIxBUFL (SPIRBE, SPIRBF, SPIROV).

**The `explorer16-32-pic24` board** is the Explorer 16/32 with the
PIC24FJ1024GB610 PIM (MA240023). The board's schematic names the socket
pin of each part, and the PIM connects the part's pins to the socket
one to one, but for the exceptions its information sheet (DS50002423)
lists; its jumpers are in their USB position:

| | Pin | |
|---|---|---|
| LEDs D3-D10 | RA0-RA7 | lit when the pin drives high |
| buttons S3, S4, S5, S6 | RD6, RD13, RA7 (with D10), RD7 | low when pressed (the board's pull-ups) |
| TC1047A temperature sensor | AN4 (RB4) | 500 mV + 10 mV per degree C |
| potentiometer R6 | AN5 (RB5) | |
| 16x2 LCD, HD44780 | PMD0-7, RS on PMA0, E on PMWR, R/W on PMRD | the EPMP |
| 25LC256 serial EEPROM | SCK RG6 (RP21), SI RG8 (RP19), SO RG7 (RP26), CS RG0 | the SPI unit the PPS puts on those pins |
| USB-UART | the UART the program uses | every UART's output is stdout |

The EEPROM is a 25LC256: 32 KiB, erased (0xFF) when the run starts;
READ, WRITE (a 64-byte page, written as CS rises, then 5 ms busy),
WREN, WRDI, RDSR, WRSR and its block protection. The run reports the
LEDs and the LCD as the PIC32 Explorer does; `--set` gives
`temperature=DEGREES`, `pot=VOLTS` or `pot=PERCENT%`, and `S3` to `S6`
`=pressed` or `=released`.

**Not modelled on the PIC24:** the other peripherals' behaviour (their
registers keep what is written: RTCC, DMA, I2C, the CCPs, input capture
and output compare, CRC, CLC, CTMU, USB, the comparators, the NVM
controller and so self-programming), the change-notification
interrupts, the external interrupt pins, the alternate vector table,
the watchdog and deadman timer, Sleep and Idle beyond waiting for an
interrupt, the EPMP's slave modes and its wait states' time, the SPI's
and the UART's bit timing.

## What it models: the PIC24F core

- **The instruction set** of the PIC24F, by Microchip's 16-bit MCU and
  DSC Programmer's Reference Manual (DS70000157): every instruction the
  manual marks as the PIC24F's, byte and word forms, with the
  addressing modes (register direct, indirect with pre/post increment
  and decrement, register offset, signed 10-bit offset, the file
  registers' 13 and 16-bit addresses) and SR's flags as each
  description gives them (Z only cleared by ADDC, SUBB, SUBBR, CPB).
  BSW.Z writes Z's complement, as the manual's examples have it. The
  dsPIC's DSP instructions and the PIC24E's are not the core's: an
  opcode it does not have makes the part reset (RCON.IOPUWR), and the
  run ends there as a lockup naming it.
- **REPEAT**: the target runs RCOUNT + 1 times, SR.RA set meanwhile,
  and may be interrupted between any two runs. DIV.S, DIV.U and their
  .D forms give the quotient, remainder and flags on the first of their
  18 runs (REPEAT #17), so an interrupt in the middle leaves W0 and W1
  as the program will see them; a divide by zero is a math error trap.
- **The data space.** The core's registers at 0x0000-0x0055; a W
  register used as a pointer before anything is written to it makes the
  part reset (RCON.IOPUWR): the run ends there. The SFRs and RAM below
  0x8000; above, the EDS window: DSRPAG (reads) and DSWPAG (writes)
  pages 1-0x1FF are the EDS (RAM above 0x8000, then the EPMP's space),
  0x200-0x2FF program memory's low words and 0x300-0x3FF its high bytes
  (program space visibility: XC16's `const` data); page 0 there is an
  address error, as is a word access at an odd address. W15 as a
  pointer below 0x0800 or above SPLIM is a stack error.
- **Table reads** of program and configuration memory (TBLPAG); table
  writes go to the flash's latches, which are not modelled.
- **Exceptions.** Traps (address, stack and math errors, the hard and
  soft ones) and interrupts through the vector table at 0x000004: the
  PC and SR's low byte with IPL3 pushed, the priority raised (IPL3 for
  a trap), the vector's address; RETFIE undoes it. DISI holds
  priorities 1-6 off for its count. A trap prints what raised it on
  stderr; one at or below the CPU's priority makes the part reset, and
  the run ends there.
- **PWRSAV** waits for an interrupt, skipping ahead to the device that
  will raise one; with none, the run ends. RESET ends it (exit 0).

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

On the MIPS32 core (a microAptiv-class pipeline): 1, a load 2, a
multiply into HI and LO or `mul` 2, a divide 18, and nothing more for a
taken branch -- the delay slot hides it, and its instruction counts as
any other.

On the PIC24F the count is the manual's, in instruction cycles (half
the oscillator's): one an instruction, two for a taken branch, GOTO,
CALL, RCALL, MOV.D, PUSH.D, POP.D and the table instructions, three
for RETURN, RETLW and RETFIE, one more for a skip over a word and two
over a two-word instruction, one more for a read through the EDS
window, five for an exception's entry.

On the Cortex-M, RISC-V and MIPS the estimate is a model, not a
cycle-accurate core: there are no wait states and no pipeline stalls. It
charges the things a compiler chooses between. On the AVR it is the
datasheet's count (above). On the AVR `--count`'s file has a third
line: the instructions a skip passed over, which are not run and are
not in the count (QEMU's plugin counts them; see below).

`--trace FILE` writes the address and halfwords of every instruction
executed (`-` for stderr); on RISC-V, the instruction as one word, or
as one halfword for a compressed one; on the AVR, its one or two
words, at the byte address; on MIPS, the instruction as one word, at
its virtual address; on the PIC24, its one or two 24-bit words, at its
program address.

## Output, and the end of a run

The board's UART writes to stdout. The UART must be enabled as on the
part: CMSDK's CTRL, the nRF51's ENABLE and STARTTX, the AVR's TXEN0.
The NS16550A sends what is written to THR, and its LSR always says the
transmitter is empty. Its receiver has nothing, unless the run has an
input (`--input`, [below](#record-and-replay---input---record---replay)).

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
| the image writes 0x42 to malta's SOFTRES | 0 |
| a PIC24's RESET instruction | 0 |
| the image requests a reset (AIRCR.SYSRESETREQ) | 0 |
| a branch to itself, or a WFI, MIPS wait or PIC24 PWRSAV, that no exception or interrupt can interrupt | 0 |
| an AVR SLEEP that nothing can wake (the AVR harness ends in such a loop) | 0 |
| the output contains `--until`'s string | 0 |
| the core locks up (a RISC-V trap or MIPS exception whose vector cannot be fetched; an AVR instruction the part does not have; a MIPS sdbbp with no debugger; a PIC24 opcode the core does not have, a W register used as a pointer before it is written, or a trap at the CPU's priority -- each a reset on the part; an instruction EmbSim does not model) | 3 |
| the stack overflows, with `--stack-report` or `--stack-limit` | 3 |
| `--max-insns` instructions have run | 4 |
| `--replay`: the run is not the recording's | 5 |
| the image cannot be loaded, or an option is wrong | 2 |

A run that ends at a lockup or at `--max-insns` says why on stderr.

## Testing and analysis

These are the measurements a firmware's CI wants of a run, and what a
debugging session wants after one. Each is an option; a run without
them is exactly the run it always was, and with them the program's
output, its exit status and its counts do not change.

They read the image's symbol table, and its DWARF when it was built
with `-g`: the line table for source lines. EmbCC, GCC and clang images
alike (DWARF 2 to 5).

### Coverage: `--coverage`

`--coverage FILE` counts each instruction the run executes, and at the
end writes which source lines and functions ran, and how often (`-` for
stderr):

```sh
embcc --target=thumbv7m-none-eabi --opt=none -g -c main.c
embsim fw.elf --coverage fw.cov
embsim fw.elf --coverage fw.info --coverage-format=lcov
genhtml fw.info -o coverage-html
```

The report is gcov's layout, file by file: a summary, each function and
the times it was entered, and the source with each line's count
(`#####`: code that never ran; `-`: no code).

```text
tests/golden/embsim-an/cov.c: 14 of 17 lines (82.4%), 2 of 3 functions (66.7%)
  function never_called (line 17): 0
  function classify (line 22): 5
  function main (line 32): 1
    #####:   17:static int never_called(int x)
        -:   18:{
    #####:   19:    return x * 3;
        -:   20:}
        -:   21:
        5:   22:static int classify(int v)
        -:   23:{
        5:   24:    int r = 1;
        5:   25:    if (v < 0)
        1:   26:        r = -1;
        4:   27:    else if (v == 0)
        1:   28:        r = 0;
        5:   29:    return r;
...
        6:   35:    for (int i = 0; i < 5; i++)
        5:   36:        sum += classify(i - 1);
        1:   37:    if (sum > 100)
    #####:   38:        sum = never_called(sum);

total: 14 of 17 lines (82.4%), 5 of 7 functions (71.4%)
```

- A line has code when the line table gives it addresses in the image's
  executable segments. Its count is that of its most-executed
  instruction: the times it ran, for straight-line code. A `for` line
  counts its test (6 for 5 turns). A line that holds code shared by
  several statements counts them all: at -O0 a function's epilogue is
  usually on its last `return`, which then counts every return.
- A function's count is its first instruction's: the times it was
  entered. A function inlined everywhere counts 0, and its lines count
  where they were inlined.
- The source is read from the path the line table gives (relative to
  the directory EmbSim runs in); when it cannot be, the lines with code
  are listed alone.
- Without `-g` there is no line table: the report has the functions
  alone, from the symbol table, with a warning.
- Functions with no line information (a startup file built without
  `-g`, the C library) are listed apart, and count in the total.
- `--coverage-format=lcov` writes lcov's tracefile instead (`SF`, `FN`,
  `FNDA`, `FNF`, `FNH`, `DA`, `LF`, `LH`), which genhtml, Codecov,
  Coveralls and most CI services read.

### Profiling: `--profile`

`--profile` writes, when the run ends, the instructions and estimated
cycles each function took, on stderr; `--profile=FILE` to a file.

```text
embsim profile: fw.elf, 2809 instructions, 4164 cycles (est.)
  self insns  self cycles   self   incl insns  incl cycles   incl     calls  function
        1320         1920  46.1%         1338         1941  46.6%        30  spin
         995         1330  31.9%         2809         4164 100.0%         0  reset
         224          410   9.8%         1814         2834  68.1%         1  main
         129          231   5.5%          174          311   7.5%         2  putn
          29           77   1.8%           29           77   1.8%         5  rec
          30           42   1.0%           30           42   1.0%         6  leaf
          18           21   0.5%           18           21   0.5%         3  tick
...
        2809         4164 100.0%                                             (total)
```

- **self** is what ran in the function itself; **incl** adds what its
  callees ran, and its interrupt handlers' when an interrupt came while
  it was on the stack. A recursive function's inclusive counts each
  instruction once. **calls** is how often a call, or an exception's
  entry, reached it.
- **The counts add up exactly to `--stats`' totals.** Each step of the
  run goes to one function, whatever it costs: a taken branch's extra
  cycles, a WFI's or a SLEEP's wait (to the function that waited), an
  AVR interrupt's four entry cycles (to the code it interrupted).
- Code no symbol covers (QEMU's reset ROM on virt) is `[unknown]`.

EmbSim follows the calls and returns as the core executes them, which
needs no frame pointer and no debug information:

| Core | Calls | Returns |
|---|---|---|
| Cortex-M | `bl`, `blx` | `bx`, `pop {..., pc}`, `ldm` with the pc, `ldr pc`, `mov pc` |
| RISC-V | `jal` and `jalr` that link (`ra` or `t0`), `c.jal`, `c.jalr` | `jalr` and `c.jr` that do not |
| AVR | `call`, `rcall`, `icall` | `ret` |
| MIPS | `jal`, `jalr` that links, `bal` and the other linking branches | `jr`, `jalr $zero` |

A MIPS call or return transfers after its delay slot: the slot's
instruction counts to the caller, and the frame begins (or ends) where
the slot hands over. A return is one when it lands where a call on the stack returns to:
that call's frame and the ones above it end, so a tail call (a branch to
another function, which then returns for both) leaves the stack right,
and the function it branched to is shown as called from the one that
branched. An exception's entry is a frame too, and its return (an
EXC_RETURN, `mret`, `reti`, `eret`) ends it wherever the handler returns to.
Where the vector table holds a jump (the AVR's, RISC-V's vectored mode,
a MIPS vector's `j` and its delay slot) the handler is the jump's
target, and the jump counts as the handler's.

`--profile-format=collapsed` writes the call paths instead, one a line
with its cycles (`collapsed-insns`: its instructions), the format
flamegraph.pl, speedscope and inferno read:

```text
reset;main;a 36
reset;main;a;leaf 21
reset;main;rec;rec;rec 16
reset;main;spin 1920
reset;main;spin;tick 21
```

```sh
embsim fw.elf --profile=fw.folded --profile-format=collapsed
flamegraph.pl fw.folded > fw.svg
```

### The stacks: `--stack-report` and `--stack-limit`

`--stack-report` writes, when the run ends, how deep each stack went and
which functions took it there (on stderr; `--stack-report=FILE` to a
file). Give it the `.su` files `--stack-usage` wrote (`--stack-su FILE`,
or simply the files: an argument ending in `.su` is one), and embrt's
`--json` report (`--stack-embrt FILE`), and their numbers go beside the
run's:

```sh
embcc --target=thumbv7m-none-eabi --opt=basic --stack-usage --call-graph=su -c *.c
embrt *.o --entry main --isr SysTick_Handler --json > bound.json
embsim fw.elf --stack-report --stack-embrt bound.json *.su
```

```text
embsim stack: fw.elf
main stack (MSP): top 0x20010000, deepest 0x2000ffa0: 96 bytes used of 65216 (down to 0x20000140, _end)
process stack (PSP): not used
by function, deepest first (bytes; frame and incl from the function's entry):
  function                    deepest   depth   frame     .su    incl   embrt
  writec                   0x2000ffa0      96      16      16      16       -
  rec                      0x2000ffa8      88       8       8      40       -
  putn                     0x2000ffb0      80      32      32      48      48
  tick                     0x2000ffb0      80       0       0       0       0
  main                     0x2000ffd0      48      24      24      72    none
the run stayed within the static numbers
```

- **The stacks.** The main stack's high-water mark, and on a Cortex-M
  the process stack's (when a thread ran on the PSP; each is read as
  the core banks it). The top is the stack pointer's highest value (on
  a Cortex-M, the reset value from the vector table too). The region
  runs down to the stack's limit: `--stack-limit`, else the link's
  `__stack_limit` (`__StackLimit`, `_sstack`), else the end of `.data`
  and `.bss` (`_end`, `__bss_end`, `_ebss`, `end`), else the bottom of
  the RAM the stack is in.
- **deepest**, **depth**: the lowest sp while the function was running,
  and how far that is below the top.
- **frame**: the most the function moved sp below its entry -- the
  number `--stack-usage` gives, beside it as **.su**. On the AVR the
  call's two-byte return address is the callee's, as avr-gcc's and
  EmbCC's `.su` count it.
- **incl**: the most the stack went below the function's entry while it
  was on the stack, its callees' frames included and the interrupts that
  came meanwhile not -- the number embrt bounds for an entry point,
  beside it as **embrt** (`none` where embrt finds no bound, such as
  recursion).
- A frame or a depth above its static number is marked `ABOVE THE
  STATIC BOUND`: the static analysis missed something, and the last line
  says so.
- On RISC-V and the AVR there is one stack pointer: a program that
  switches stacks (an RTOS's tasks) is measured over all of them.

**An overflow ends the run.** With `--stack-report` or `--stack-limit
ADDR` (or a symbol's name), the main stack going below its limit -- past
`--stack-limit` or `__stack_limit`, or, with neither, into `.bss` and
`.data` -- stops the run at the instruction that moved sp there, as a
part with a stack guard would fault, with status 3:

```text
embsim: stack overflow: sp 0x2000f7f8 is below the stack limit 0x2000f800 (__stack_limit), at deep (ovf.c:9); 354 instructions, 573 cycles (est.)
embsim: stack overflow: sp 0x1ffffff0 is below the end of .data and .bss 0x20000004 (_end), at deep+0x2 (ovf.c:9); 10046 instructions, 16432 cycles (est.)
```

An exception's entry that stacks past the limit is reported as one (`in
an exception's entry, at` the instruction it interrupted).

### Faults: `--fault-report`

`--fault-report` decodes each fault the moment the core takes it, on
stderr (`--fault-report=FILE` to a file): a Cortex-M's HardFault,
MemManage, BusFault and UsageFault, a RISC-V exception (an ECALL and
an EBREAK, which a program makes on purpose, are not faults), and a
MIPS exception other than an interrupt, a syscall and a break: Cause's
code in words, BadVAddr for an address or TLB exception, the
coprocessor of a Coprocessor Unusable, the registers of the set the
exception interrupted:

```text
embsim: fault: DSP ASE state disabled: a DSP instruction with Status.MX clear (Cause.ExcCode 26) at 0x9d000b5c, in main+0x6c (pic32-fw.c:123)
  instruction 0x9d000b5c: 01200811  mthi t1, $ac1
```

```text
embsim: fault: HardFault (exception 3) at 0x00000160, in crash (fault.c:31)
  HFSR  0x40000000  FORCED: a configurable fault escalated to HardFault (its handler disabled, or not able to preempt)
  CFSR  0x00008200
        PRECISERR: a precise data bus error at 0x30000000
  BFAR  0x30000000 (BFARVALID)
  instruction 0x00000160: 6800       ldr r0, [r0, #0]
  stacked frame at 0x2000ff90 (MSP):
    r0  0x30000000  r1  0x20000004  r2  0xe000e010  r3   0x00000000
    r12 0x00000000  lr  0x00000173  pc  0x00000160  xpsr 0x0100000f
    lr: level2+0xc (fault.c:37)
  r4  0x00000007  r5  0x00000000  r6  0x00000000  r7   0x00000000
  r8  0x00000000  r9  0x00000000  r10 0x00000000  r11  0x00000000
  backtrace (.debug_frame):
    #0  0x00000160 in crash (fault.c:31)
    #1  0x00000172 in level2+0xc (fault.c:37)
    #2  0x000001c4 in tick+0x8 (fault.c:65)
        <- an exception's entry (EXC_RETURN 0xfffffff9): the code it interrupted
    #3  0x000001da in spin+0xa (fault.c:71)
    #4  0x00000234 in main+0x52 (fault.c:110)
    #5  0x000000a2 in reset+0x92
        (no call frame information for 0x000000a2: the unwinding stops here)
  handler: none -- the vector is 0x00000000, without the Thumb bit: the core cannot run it (it will fault again)
embsim: the core locked up at 0x00000000, in 0x00000000: the fault above had no handler
```

- **The status registers in words**: each CFSR bit set (MemManage's,
  BusFault's and UsageFault's), HFSR's FORCED and VECTTBL, and BFAR and
  MMFAR when their valid bits say they hold the address. On RISC-V,
  mcause by name, and mtval as what it is for that cause (the address,
  or the instruction's bits).
- **The stacked frame** -- r0-r3, r12, lr, pc and xPSR, read from the
  stack the EXC_RETURN in lr names -- and the other registers; on
  RISC-V, the 32 registers.
- **The faulting instruction**, disassembled (Thumb, RISC-V; a form the
  disassembler does not know is its encoding), with its symbol and line.
- **The backtrace**, by the image's `.debug_frame` (EmbCC writes it for
  Thumb and RISC-V with `-g`) from the registers at the fault. On a
  Cortex-M it goes through an exception's frame, where the CFI's return
  address is an EXC_RETURN, into the code the exception interrupted.
  Where no CFI covers the faulting pc, it is the calls EmbSim saw the
  core make (the call stack `--profile` follows), named by the symbol
  table.
- **The handler**: the one the vector table or mtvec gives, or that
  there is none -- a vector without the Thumb bit or nowhere near a
  function, an mtvec where nothing can be fetched -- in which case the
  core locks up next, and the run ends with status 3 and one more line.
- An AVR has no faults; with `--fault-report` its lockup (an instruction
  the part does not have) gives the pc and the calls that led there.

It is an option, not the default when a run stops on an unhandled
fault, for two reasons. A run's stderr without new options is the same
as before, byte for byte, which scripts and the golden tests read. And
most firmware does not stop on its faults: its HardFault handler is a
`b .`, so EmbSim ends the run as an idle loop (status 0), or a handler
logs and resets. The report is taken at the fault's entry, so it is
there in both cases, and for every fault a program handles too (a test
that provokes faults gets one report each), which as a default would be
noise.

### Record and replay: `--input`, `--record`, `--replay`

`--input FILE` connects FILE (`-`: stdin) to the board's UART receiver:
the PL011 of the lm3s6965evb, the CMSDK UART of the MPS2 boards, virt's
NS16550A and the AVR's USART0.

| UART | A byte arriving | Its interrupt |
|---|---|---|
| PL011 | waits in DR; FR's RXFE clears; RIS's RXRIS sets | with IMSC's RXIM: IRQ 5; reading DR or ICR clears it |
| CMSDK | with CTRL's RX enable: waits in DATA; STATE's RX full sets | with CTRL's RX interrupt enable: INTSTATUS bit 1, IRQ 0 |
| NS16550A | waits in RBR; LSR's DR sets | none (virt's PLIC is not modelled): poll LSR |
| USART0 | with RXEN0: waits in UDR0; UCSR0A's RXC0 sets | with RXCIE0: vector 18 |

A byte arrives when the host has one: the receiver is offered the next
every 4096 steps while it is empty, and when the core would sleep with
nothing else to wake it, the run waits for the host instead of ending.
Typing into stdin makes an interactive console. Where the host's bytes
fall in the run depends on the host -- how fast the pipe delivers, when
a person types -- and a program that counts its own time sees different
counts each run:

```text
$ (printf ab; sleep 0.3; printf cd; sleep 0.3; printf q) | embsim rr.elf --input -
got a at tick 261
got b at tick 591
got c at tick 12463
got d at tick 12785
got q at tick 24650
```

That is what `--record FILE` is for. It writes everything that came from
outside the machine, each with the step it came at, and the run's end:

- the bytes the UART received (`rx`), and the core it woke when it had
  to wait for one (`wake`);
- semihosting's SYS_READC, a byte of the host's stdin (`readc`);
- a debugger's register and memory writes (`reg`, `mem`, gdb's `load`
  and flash erases among them), its `monitor reset` and `reload`, the
  core it wakes from a sleep nothing else would end, and its `kill`;
- every exception and interrupt the core entered, with its step and
  instruction count (`exc`), which a replay checks: a run of the same one
  at a constant stride -- a timer's tick waking an idle loop while the
  run waits for the host -- is one line, so the record grows with what
  the program does, not with how long a person takes to type;
- the machine: the image (and a hash of it), the board, the core, the
  RAM size, the SVD file, the device pack (`--dfp`), semihosting,
  `--max-insns`, `--until`, and the board's inputs (`--set`), which the
  replay takes from it;
- the end: the steps, the instructions and cycles, how the run ended
  and its status, a hash of the registers and memory, and the output's
  length and hash.

There is no time to record: every counter and timer EmbSim has runs on
the core's estimated cycles, never on the host's clock. A step is a
call of the core's step that did something; one a watchpoint stops
before its access does nothing, and a run without the debugger never
takes it.

```text
@1232 1231 exc 15 x 261 +11 +10
@4096 3835 rx 61
@4099 3837 exc 21
@4575 4312 exc 15 x 329 +11 +10
...
@1189 1189 mem 20000084 29000000
@1189 1189 reg 0 64000000
end 270912 246621 242911779 1 0 669cb360efc5d2fd 34 022fc4ec611bf39b
```

`--replay FILE` runs the recording again: the same machine (an option
that shapes it may be given only as the recording has it; the image is
the recording's unless named, and must hash the same), the recorded
inputs at the recorded steps, no host input and no debugger. Its output,
its `--count` and `--stats`, its exit status and its final state are the
recorded run's; it says so on stderr, or where the two went apart, with
status 5:

```text
embsim: replay: the run is the recording's (270912 steps, 246621 instructions, 17 events)
embsim: replay diverged at step 4097 (3836 instructions): rx: the recording is at 3835 instructions here
embsim: replay diverged: the run ended at
  steps, instructions, cycles, state, status, the state's hash, the output's bytes and hash:
  this run's:      164410 149801 146072424 1 0 c9f06dca0cedf591 99 a408e13e96d3cf61
  the recording's: 164410 149801 146072424 1 0 c9f06dca0cedf591 99 30e7262b9dcebd41
```

A recorded debugging session -- breakpoints, `set var`, a register
changed, a watchpoint -- replays without the debugger, so a run that
went wrong once under gdb can be run again, and `--trace`d, `--profile`d
or debugged again, exactly as it went.

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
- Peripherals beyond the UART, SysTick, the NVIC and DWT's cycle
  counter, except as an SVD file's registers (`--svd`) and the
  STM32F405's models.
- On the STM32F405: DMA, the ADC, SPI, I2C, EXTI and the rest are their
  registers alone; RCC's clock frequencies, its interrupts and its reset
  registers; GPIO's LCKR and pin inputs from outside; the USARTs'
  receivers and baud-rate timing; the timers' capture/compare,
  down-counting, center-aligned modes, repetition counter, slave modes
  and DMA requests.

Each instruction among these is a UsageFault, so a run that needs one
stops at a named fault rather than computing something wrong.

On the AVR:
- Timer/Counter0 and 2, SPI, TWI, the ADC, the analog comparator, EEPROM,
  the watchdog, the external and pin-change interrupts, and the power
  reduction register (the USART's receiver is, with `--input`). Their
  registers keep what
  is written (on QEMU they read as zero), and their interrupts never
  come.
- The output-compare pins and input capture from a pin.
- Self-programming (SPM) and the boot section; the other AVR parts (the
  ATmega2560's 3-byte program counter, ELPM and RAMPZ).
- The sleep modes beyond Idle.

On MIPS:
- microMIPS, MIPS16e, the MCU ASE and the DSP ASE's arithmetic (each
  ends the run as a lockup naming the instruction); paired singles.
- Big-endian images (mips-none-elf): EmbSim's MIPS core is
  little-endian.
- The caches (`cache` is a no-op), the watch registers (kept, not
  enforced), the performance counters, EJTAG's debug mode (sdbbp ends
  the run).
- `--stack-report` takes the one stack pointer as the program's single
  stack: an RTOS's task stacks, in its heap below `_end`, read as an
  overflow (give `--stack-limit` an address below them). The GDB
  server gives a MIPS core no target
  description and no memory map, as QEMU's stub does: gdb describes the
  core itself (registers 0-72: the GPRs, Status, LO, HI, BadVAddr,
  Cause, PC, the FPU's as 0), and would compare a map's kseg0 addresses
  with its own sign-extended ones.

On the PIC24F:
- The dsPIC's DSP engine and the PIC24E's and dsPIC33's instructions
  (MOVPAG, CPBEQ and the rest; each ends the run as a lockup); the
  flash's programming (TBLWT and the NVM controller); the debug
  executive and BREAK.
- `--coverage`, `--profile`, `--stack-report` and `--fault-report`
  do not follow the PIC24 core yet; no gdb has a PIC24 target, so the
  GDB server's registers are EmbSim's own numbering (W0-W15, SPLIM, the
  PC, SR, CORCON, DSRPAG, DSWPAG, RCOUNT, TBLPAG).
- Program memory an image leaves out reads 0 (NOP), not erased flash.

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

**SVD files.** `tests/golden/embsim-svd.sh` runs a program over a test
SVD with every access type, modifiedWriteValues and readAction, arrays,
clusters and derivedFrom, and compares each value with the CMSIS-SVD
specification's, the trace and the map with their records; and an
STM32F405 firmware, built from embsvd's header, startup and linker
script, that drives RCC, GPIO, USART1-3 and TIM2, on QEMU's
netduinoplus2 and on EmbSim: its values must be QEMU's where QEMU models
the register, and RM0090's (a record) where it does not or differs.
`tests/golden/embsim-svd-all.sh` maps every SVD file in `$EMBREF/svd`
(ST's and Nordic's) and holds each register's address and size to a
second reading of the file in Python.

**Analysis.** `tests/golden/embsim-coverage.sh` builds
`tests/golden/embsim-an/cov.c`, whose every line with code is marked
with the times it runs, at -O0 -g on the M3, RV32 and the AVR, and with
clang (DWARF 5); the report and the lcov tracefile must give exactly the
marks, line for line, the run must be the same with and without
`--coverage`, and an image without `-g` must give its functions.
`tests/golden/embsim-profile.sh` profiles `embsim-an/prof.c`, whose
calls are known (a loop's, recursion five deep, a call through a pointer,
a timer interrupt), on the M3, RV32 with and without compressed
instructions, and the AVR: the counts must add up to `--stats`' in the
report and in both collapsed forms, each function's self instructions
must be what the `--trace` of the run gives it by llvm-nm's symbols, and
the calls, the paths and the inclusive counts must be prof.c's; and
`exc.c`'s handlers must be entered as often as it counts.
`tests/golden/embsim-stack.sh` measures prof.c's stacks: each frame must
be the `.su` file's, the depth the sum of the deepest path's frames, and
the inclusive depths of the functions embrt bounds embrt's bounds; and it
overflows `ovf.c`'s recursion past `__stack_limit`, `--stack-limit` and
into `.bss`, where the run must stop at the push that crossed.
`tests/golden/embsim-fault.sh` takes `fault.c`'s faults on the M3 and
RV32 -- a BusFault handled, a divide by zero forced to a HardFault with
no handler, a fault in an interrupt handler, a load access fault and an
illegal instruction -- and holds each report to the fault: the status
registers' words, the faulting instruction's mnemonic against
llvm-objdump's, the backtrace against the source lines marked in
fault.c, the handler; and the run must be the same without the report.
`tests/golden/embsim-replay.sh` feeds `rr.c` bytes through a pipe with
pauses, on the PL011 (by interrupt, with SysTick and without), the
CMSDK UART, the NS16550A and the AVR's USART0, and through SYS_READC;
records each run, replays it with other bytes on stdin, and requires the
same output, `--count`, `--stats` and status; replays a gdb session's
writes and watchpoint without gdb; and refuses a record with a byte
changed, a byte a step late, another image or another board.

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

**MIPS.** `tests/golden/embsim-mips.sh` runs the exec corpus at -O0,
-O2 and -Os on QEMU's malta and on EmbSim's (the output and the exit
status), `mips-isa.c`'s instruction edges and `mips-fpu.c`'s FPU edges
against QEMU's 24Kc and P5600, and the ends of a run.

**PIC32.** No QEMU has a PIC32. `tests/golden/embsim-pic32.sh` holds the
register file the PIC32MZ2048EFH100's ATDF gives to the pack's own
header (every SFR and its CLR, SET and INV at their addresses), and an
EmbCC program on each PIC32 board to its record: the DSP ASE after
EmbCC's startup, the EVIC, shadow sets, the timers, the ports, the ADC,
the LCD on the PMP, SYSKEY on the Explorer; ANSELx, the switches and the
LEDs on the Curiosity.

**PIC24.** No QEMU has a PIC24, and no PIC24 assembler is at hand:
`tests/golden/embsim-pic24.sh` holds the EDC's register file to the
pack's linker script (every SFR at its address), and the instruction
set to the examples of the programmer's reference manual: 227 of them,
each case setting the example's Before state and printing its After
state (`pic24-isa.s`, assembled by `pic24-asm.awk`, a few instructions'
assembler, into an Intel HEX image), which must be what the manual
prints -- the examples that contradict their own instruction's
description are left out, and named. `pic24-board.s` drives the
peripherals and the Explorer 16/32's PIC24 PIM: Timer1's interrupt, the
LEDs, the buttons, the sensors through ADC1, the LCD through the EPMP,
the EEPROM through SPI1 and the Peripheral Pin Select.
