# embsvd — a device's registers from its SVD file

`embsvd` reads a microcontroller's CMSIS-SVD file, the XML description
every Cortex-M vendor publishes of a device's peripherals, registers, bit
fields and interrupts. From it, it writes the files a bare-metal project
starts with: the device header, a startup file and a linker script. It
also answers questions about the registers. This page is the command
reference.

## Synopsis

```text
embsvd DEVICE.svd [--header FILE] [--no-cmsis]
                  [--nvic-prio-bits N] [--fpu-present 0|1]
                  [--startup FILE]
                  [--ld FILE --flash ORIGIN:LENGTH --ram ORIGIN:LENGTH]
embsvd DEVICE.svd --list
embsvd DEVICE.svd --show PERIPHERAL
```

## A project from an SVD

```sh
embsvd STM32F405.svd --nvic-prio-bits 4 --fpu-present 1 \
       --header STM32F405.h --startup startup.c \
       --ld STM32F405.ld --flash 0x08000000:1M --ram 0x20000000:128K
CF="--target=thumbv7em-none-eabi -O2 -ICMSIS/Core/Include -fgnuc-version=4.2.1"
embcc $CF -c startup.c && embcc $CF -c main.c
embcc --target=thumbv7em-none-eabi -T STM32F405.ld startup.o main.o -o fw.elf
```

`main.c` includes `STM32F405.h` and drives the device through it:

```c
#include "STM32F405.h"

void USART1_IRQHandler(void) { /* ... */ }    /* takes the startup's weak one */

int main(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN_Msk;
    USART1->CR1 = USART6_CR1_UE_Msk | USART6_CR1_TE_Msk;
    NVIC_EnableIRQ(USART1_IRQn);
    SysTick_Config(16000);
    /* ... */
}
```

## The header (`--header FILE`)

The header has the shape of the headers CMSIS's `svdconv` writes, and of
the vendors' own:

- **`IRQn_Type`.** The core's exceptions (`SysTick_IRQn = -1`, ...), then
  each interrupt of the SVD as `NAME_IRQn`, by number. ARMv6-M and
  ARMv8-M Baseline cores (`CM0`, `CM0PLUS`, `CM1`, `CM23`) get only the
  exceptions they have.
- **The core's configuration.** `__CM4_REV` (from the SVD's `r1p0`-style
  revision), `__MPU_PRESENT`, `__FPU_PRESENT`, `__NVIC_PRIO_BITS` and
  `__Vendor_SysTickConfig`. Then the header includes CMSIS's core header for
  the SVD's cpu (`core_cm4.h` for `CM4`), which provides `NVIC`, `SCB`,
  `SysTick`, and the intrinsics. With the core header included, the SVD's
  own copies of the core's peripherals (everything from 0xE0000000 up to
  the vendor area at 0xE0042000) are left out, as `svdconv` leaves them
  out, and a comment lists them.
- **One struct per register layout.** A peripheral's registers are laid
  out by their offsets, with `uint8_t RESERVEDn[...]` filling the gaps.
  Registers declared as each other's `alternateRegister` share an
  anonymous union. A register is `__IO`, `__I` (read-only) or `__O`
  (write-only), and is `uint8_t` to `uint64_t` by its size. The struct is
  named after the peripheral that declares the registers
  (`USART6_Type`). A peripheral `derivedFrom` another uses its type, so
  `USART1`, `USART2` and `USART3` are all `USART6_Type` in ST's file.
- **Where each one is.** `NAME_BASE` and `#define NAME ((TYPE *)NAME_BASE)`.
- **Every field.** `LAYOUT_REGISTER_FIELD_Pos` and `..._Msk`, where
  `LAYOUT` is the peripheral that declares the registers
  (`USART6_CR1_UE_Msk`).

`--no-cmsis` writes the header without CMSIS. The core's peripherals then
stay, and `__IO`, `__I` and `__O` are defined in the header.

An SVD can be wrong about the core. ST's `STM32F405.svd` 1.2 says 3
priority bits and no FPU, but the part has 4 and an FPU, as ST's own
`stm32f405xx.h` says. `--nvic-prio-bits N` and `--fpu-present 0|1`
override what the file says.

## The startup (`--startup FILE`)

The startup is a C file:

- **The vector table.** It is placed in `.isr_vector`: the initial stack
  pointer (`_estack`), `Reset_Handler`, the core's exceptions, and each
  interrupt in its slot (`16 + NAME_IRQn`). Unused slots are 0.
- **Handlers.** Every handler is a weak alias of `Default_Handler`, which
  spins. A function of the same name in the program, such as
  `USART1_IRQHandler` or `SysTick_Handler`, replaces it.
- **`Reset_Handler`.** It copies `.data` from `_sidata` to
  `_sdata`..`_edata`, zeroes `_sbss`..`_ebss`, and calls `SystemInit` if
  the program defines one (a weak empty one is there otherwise). It then
  runs the constructors from `__init_array_start` to `__init_array_end`,
  and calls `main`.

## The linker script (`--ld FILE`)

An SVD describes peripherals, not memories, so the flash and RAM regions
are given on the command line: `--flash ORIGIN:LENGTH --ram
ORIGIN:LENGTH`, in C notation with an optional `K`, `M` or `G`
(`0x08000000:1M`). The script has STM32CubeMX's shape:

- `.isr_vector` first in flash, kept;
- `.text`, `.rodata`, `.ARM.exidx` and the constructor arrays in flash;
- `.data` in RAM, stored in flash (`_sidata`);
- `.bss` in RAM;
- `_estack` at the top of RAM.

It defines every symbol the generated startup reads.

## Asking (`--list`, `--show`)

`--list` prints the peripherals: the base address, and either the number
of registers or which peripheral it is `like` (derived from).

```text
STM32F405: CM4, 91 peripherals, 83 interrupts
  USART1       0x40011000  like USART6  Universal synchronous asynchronous receiver transmitter
```

`--show NAME` prints one peripheral's registers with their addresses,
sizes, access, reset values and fields:

```text
USART1 at 0x40011000: Universal synchronous asynchronous receiver transmitter
  0x40011000  +0x000  SR             32 bits  rw  reset 0xc00000  Status register
      [9]     CTS
      [7]     TXE
  0x40011008  +0x008  BRR            32 bits  rw  reset 0x0  Baud rate register
      [15:4] DIV_Mantissa
      [3:0]  DIV_Fraction
```

## What it reads

- **Numbers.** Decimal, `0x` hex, and `#` binary, with `k`/`M`/`G`.
- **Fields.** Given by `bitOffset`/`bitWidth`, `lsb`/`msb` or
  `bitRange`.
- **Inherited values.** `size`, `access` and `resetValue` come from the
  device or the peripheral when a register does not give them.
- **Peripherals `derivedFrom` another,** through any chain of them.
- **Register arrays.** `dim` arrays named `NAME%s` become `NAME0`,
  `NAME1`, ... (or the `dimIndex` names).

These are refused by name, with the file's line, because a header whose
offsets are approximately right compiles and then drives the wrong
register:

- `<cluster>`;
- arrays written `NAME[%s]`;
- registers or fields `derivedFrom` another;
- a peripheral that is `derivedFrom` another and also has registers of
  its own;
- two registers at one offset when neither is the other's
  `alternateRegister`;
- a register that overlaps the one before it;
- a field outside its register.

## Exit status

`embsvd` exits with 0 on success, 1 when the file cannot be read or is
refused, and 2 for a usage error.

## Tests

`tests/golden/svd-stm32f405.sh` checks embsvd against ST's own files:

- From ST's `STM32F405.svd`, a table of 149 register addresses in 31
  peripherals is compiled against the generated header and against ST's
  hand-written `stm32f4xx.h`, and the two must be byte-identical.
- A program built only from the generated header, startup and linker
  script, plus CMSIS-Core, **runs** on QEMU's netduinoplus2 (an
  STM32F405). It checks `.data`, `.bss`, a constructor, SysTick, and a
  pended USART1 interrupt reaching its handler through the generated
  vector table.

The SVD, CMSIS_5 and cmsis-device-f4 are looked for under `~/EmbRef`.
The test skips when they are missing.

## See also

[Bare-metal programming](../embedded.md#cmsis-and-vendor-files),
[`embld`](embld.md) (linker scripts).
