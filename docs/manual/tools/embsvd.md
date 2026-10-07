# embsvd — a device's registers from its SVD file

`embsvd` reads a microcontroller's CMSIS-SVD file, the XML description
every Cortex-M vendor publishes of a device's peripherals, registers, bit
fields and interrupts. From it, it writes the files a bare-metal project
starts with: the device header, a startup file and a linker script. It
also answers questions about the registers, and describes the whole
device as JSON for other tools. This page is the command reference.

## Synopsis

```text
embsvd DEVICE.svd [--header FILE] [--no-cmsis]
                  [--nvic-prio-bits N] [--fpu-present 0|1]
                  [--startup FILE]
                  [--ld FILE --flash ORIGIN:LENGTH --ram ORIGIN:LENGTH]
                  [--json FILE [--flash ORIGIN:LENGTH] [--ram ORIGIN:LENGTH]]
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
  anonymous union, as do a read-only and a write-only register at one
  offset (svdconv makes the same union). A register is `__IO`, `__I`
  (read-only) or `__O` (write-only), and is `uint8_t` to `uint64_t` by
  its size. The struct is named after the peripheral that declares the
  registers (`USART6_Type`), or its `headerStructName`. A peripheral
  `derivedFrom` another uses its type, so `USART1`, `USART2` and
  `USART3` are all `USART6_Type` in ST's file.
- **One struct per cluster**, before the struct it is in, innermost first
  (see [Clusters and arrays](#clusters-and-arrays)).
- **Where each one is.** `NAME_BASE` and `#define NAME ((TYPE *)NAME_BASE)`.
  The SVD's `headerDefinitionsPrefix` goes in front of both, and of a
  peripheral's struct, but not of a cluster's: Nordic's `NRF_` gives
  `NRF_UARTE00_S`, `NRF_UARTE_Type` and `UARTE_PSEL_Type`.
- **Every field.** `STRUCT_REGISTER_FIELD_Pos` and `..._Msk`, where
  `STRUCT` is the struct that holds the register, less `_Type`
  (`USART6_CR1_UE_Msk`, `UARTE_PSEL_TXD_CONNECT_Msk`).

`--no-cmsis` writes the header without CMSIS. The core's peripherals then
stay, and `__IO`, `__I` and `__O` are defined in the header.

An SVD can be wrong about the core. ST's `STM32F405.svd` 1.2 says 3
priority bits and no FPU, but the part has 4 and an FPU, as ST's own
`stm32f405xx.h` says. `--nvic-prio-bits N` and `--fpu-present 0|1`
override what the file says.

## Clusters and arrays

A `<cluster>` is a group of registers at an `addressOffset` from the
peripheral or cluster it is in, and clusters nest to any depth. Each
becomes a struct, named, as svdconv names it, by its `headerStructName`,
else its `dimName`, else the enclosing struct's name and its own
(`NETIF_PORT_Type` for cluster `PORT[%s]` in peripheral `NET` whose
`headerStructName` is `NETIF`). The enclosing struct holds it as a
member at its offset.

An array -- a register, cluster, field or peripheral with `dim` and
`dimIncrement` -- is laid out by its name:

| SVD name | Elements | In the header |
|---|---|---|
| `DATA[%s]`, `dimIncrement` the register's size | packed | `__IO uint32_t DATA[4];` |
| `CH[%s]`, a cluster smaller than `dimIncrement` | spaced | the cluster's struct is padded to `dimIncrement`: `__IO DMA_CH_Type CH[4];` |
| `PRIO[%s]`, a register smaller than `dimIncrement` | spaced | `PRIO0`, `PRIO1`, ... at their offsets, `RESERVED` between |
| `GPIO%s_CTRL` | any | `GPIOA_CTRL`, `GPIOB_CTRL`, ... at their offsets |

`%s` takes its names from `dimIndex`: a list (`A,B,C`), a range of
numbers (`8-15`) or of letters (`A-D`), or `0` to `dim - 1` without one.
A field array (`CH%s_IE`) is one field per element, `dimIncrement` bits
apart. A peripheral array (`TIMER%s`) is one peripheral per element,
`dimIncrement` bytes apart, all with the first one's struct.

A register, cluster, field or `enumeratedValues` can be `derivedFrom`
another: in the same scope by its name (`CTRL`), or anywhere by its full
path from the peripheral (`NET.PORT.CFG`, `DMA.CTRL.MODE`). It is the
other one with what the derived element says written over it. An array's
`dim` comes along only when the new name has a `%s` for it, so
`<register derivedFrom="DATA">` named `DATAX` is one register. A field
that gives only a new `bitOffset` keeps the other's width. A cluster
derived from another, with no registers of its own, uses the other's
struct.

Every struct is checked against C as it is laid out: a member must be at
a multiple of its own alignment, as C would place it, and the elements of
an array must not overlap.

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

## The hardware as JSON (`--json FILE`)

`--json` writes what the SVD says about the device for tools rather than
for a compiler -- a debugger's register view, an RTOS's hardware
description -- with every array and cluster expanded and every register at
its absolute address. `--flash` and `--ram` add the memories. The
document is one object; its `"schema"` is `1`, and stays `1` as long as
no key below changes its meaning or goes away (keys may be added).

```json
{
  "schema": 1,
  "generator": "embsvd",
  "source": "device.svd",
  "device": {"name": "...", "vendor": "...", "version": "...", "description": "...",
             "addressUnitBits": 8, "width": 32, "headerDefinitionsPrefix": "NRF_"},
  "cpu": {"name": "CM33", "revision": "r0p4", "endian": "little",
          "mpuPresent": true, "fpuPresent": true, "fpuDP": false,
          "nvicPrioBits": 3, "vendorSystickConfig": false,
          "deviceNumInterrupts": 270},
  "memories": [{"name": "FLASH", "origin": 0, "length": 1572864, "access": "rx"},
               {"name": "RAM", "origin": 536870912, "length": 262144, "access": "rwx"}],
  "interrupts": [{"name": "SERIAL00", "value": 74, "description": null}],
  "peripherals": [
    {"name": "UARTE00_S", "description": "...", "groupName": null,
     "baseAddress": 1342480384, "derivedFrom": "UARTE00_NS",
     "typeName": "NRF_UARTE_Type",
     "addressBlocks": [{"offset": 0, "address": 1342480384, "size": 4096,
                        "usage": "registers"}],
     "interrupts": [{"name": "SERIAL00", "value": 74, "description": null}],
     "registers": [
       {"name": "TXD", "path": "PSEL.TXD", "index": [],
        "address": 1342481924, "offset": 1540, "size": 32,
        "access": "read-write", "resetValue": 4294967295,
        "resetMask": 4294967295, "alternate": null, "description": null,
        "fields": [
          {"name": "CONNECT", "bitOffset": 31, "bitWidth": 1,
           "access": "read-write", "description": null,
           "enumeratedValues": [
             {"name": "Connected", "value": 0, "isDefault": false,
              "usage": "read-write", "description": null}]}]}]}]
}
```

- **`device`**: the SVD's `name`, `vendor`, `version` and `description`
  (`null` when the file has none), its `addressUnitBits` and `width`, and
  its `headerDefinitionsPrefix` or `null`.
- **`cpu`**: `null` when the SVD has no `<cpu>`. Otherwise its `name`,
  `revision` and `endian` (strings or `null`), `mpuPresent`,
  `fpuPresent`, `fpuDP` and `vendorSystickConfig` (booleans, `false`
  when not said), `nvicPrioBits`, and `deviceNumInterrupts` (or `null`).
  `--nvic-prio-bits` and `--fpu-present` correct them here too.
- **`memories`**: from `--flash` (`FLASH`, `rx`) and `--ram` (`RAM`,
  `rwx`), as `origin` and `length`; empty without them, because an SVD
  does not describe memories.
- **`interrupts`**: every interrupt of the device once, by `value`.
- **`peripherals`**: in the SVD's order, each array element its own.
  `derivedFrom` is the peripheral whose registers it has, or `null`;
  `typeName` the header's struct for them. `addressBlocks` gives each
  block's `offset`, absolute `address`, `size` and `usage` (a derived
  peripheral without its own has the other's). `interrupts` are the
  peripheral's own.
- **`registers`**: every register element, by address. `name` is the
  SVD's with its `%s` replaced (`DATA[2]`, `GPIOB_CTRL`); `path` is how C
  reaches it from the peripheral's struct in the generated header
  (`PORT[1].QUEUE[2].DESC[1].FLAGS`, `PRIO1`); `index` is the element's
  position in each array on the way, outermost first (`[1, 2, 1]`, `[]`
  when there is none). `address` is absolute and `offset` from the
  peripheral's base. `size` is in bits; `access` is one of `read-only`,
  `write-only`, `read-write`, `writeOnce` and `read-writeOnce`, inherited
  as the SVD inherits it; `resetValue` and `resetMask` are cut to the
  register's size. `alternate` is the `alternateRegister` or
  `alternateGroup`, or `null`.
- **`fields`**: `bitOffset` and `bitWidth` however the SVD wrote them,
  `access` (the register's when the field says none), and
  `enumeratedValues`: each value's `name`, `value` (`null` for an
  `isDefault` one with no value), `isDefault`, the `usage` of the group
  it is in, and `description`. A value written with don't-care bits
  (`#1x`) has `care`, the bits of the field it fixes.

Numbers are JSON integers; a 64-bit reset value may be larger than a
JavaScript number holds exactly.

## Asking (`--list`, `--show`)

`--list` prints the peripherals: the base address, and either the number
of registers or which peripheral it is `like` (derived from).

```text
STM32F405: CM4, 91 peripherals, 83 interrupts
  USART1       0x40011000  like USART6  Universal synchronous asynchronous receiver transmitter
```

`--show NAME` prints one peripheral's registers, each array element and
cluster expanded and named by its `path`, with their addresses, sizes,
access, reset values and fields:

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
- **Access.** The five SVD access types, in any case (Nordic writes
  `read-writeonce`).
- **Booleans.** `true`, `false`, `1` and `0`.
- **`derivedFrom`** on peripherals, through any chain of them, and on
  registers, clusters, fields and `enumeratedValues`.
- **Clusters and arrays,** as [above](#clusters-and-arrays).

These are refused by name, with the file's line, by every output,
because a header whose offsets are approximately right compiles and then
drives the wrong register:

- a register or cluster not at a multiple of its own alignment;
- two registers at one offset when neither is the other's
  `alternateRegister` (and they are not a read-only and a write-only
  one);
- a register that overlaps the one before it, and an array whose
  elements overlap;
- `[%s]` anywhere but at the end of a name, two `%s` in one name, `%s`
  without `dim` and `dim` without `%s`, and a `dimIndex` that names a
  different number of elements;
- an array of peripherals written `NAME[%s]`;
- a `derivedFrom` that names nothing, or goes round in a circle;
- a peripheral or cluster that is `derivedFrom` another and also has
  registers of its own;
- two different structs that would have one name (two clusters with
  one `headerStructName`, say);
- a field outside its register;
- an `addressUnitBits` other than 8.

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

`tests/golden/svd-clusters.sh` checks clusters, arrays, `derivedFrom`
and `--json` with three witnesses that share no code: `_Static_assert`s
of `offsetof` and `sizeof`, worked out by hand from the SVDs, compiled
against the generated headers by EmbCC and by clang for a Cortex-M; a
Python reading of the SVD by the specification (`svdref.py`), whose
register addresses the JSON's must equal; and an assert per JSON
register that the header's `BASE + offsetof(TYPE, path)` is its address.
It runs them over `features.svd`, which holds every feature,
`nrfshape.svd`, a device in the shape of Nordic's nRF54L files, and
Nordic's own SVD files when `~/EmbRef/svd/nrf*.svd` has them. ARM's
`ARM_Example.svd` is checked against the `TIMER0_Type` that CMSIS's own
svdconv wrote for it, from CMSIS_5's documentation.

## See also

[Bare-metal programming](../embedded.md#cmsis-and-vendor-files),
[`embld`](embld.md) (linker scripts).
