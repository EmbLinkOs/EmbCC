# Target reference

Every target `--target=` accepts, generated from EmbCC's target database (`src/targets/`) by `embcc --reference=targets`. The first name in each row is the one EmbCC prints back (`--print-target`); the others are accepted too. [Targets](targets.md) covers each family in depth.

## x86-64

`int` is 4 bytes, `long` 8, a pointer 8, `double` 8 and `long double` 16. Plain `char` is signed. Little-endian; `__int128` exists.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `x86_64-elf` | bare metal | ELF | `x86_64`, `x86_64-none-elf` |
| `x86_64-emblink` | EmbLinkOS | ELF |  |
| `x86_64-linux-gnu` | Linux | ELF | `x86_64-linux` |
| `x86_64-apple-darwin` | macOS | Mach-O | `x86_64-darwin` |
| `x86_64-windows-gnu` | Windows | COFF | `x86_64-w64-mingw32` |

## AArch64

`int` is 4 bytes, `long` 8, a pointer 8, `double` 8 and `long double` 16. Plain `char` is unsigned. Little-endian; `__int128` exists.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `aarch64-elf` | bare metal | ELF | `aarch64`, `arm64`, `aarch64-none-elf` |
| `aarch64-emblink` | EmbLinkOS | ELF |  |
| `aarch64-linux-gnu` | Linux | ELF | `aarch64-linux` |
| `aarch64-apple-darwin` | macOS | Mach-O | `arm64-apple-darwin`, `aarch64-darwin` |

## ARM: Cortex-M and ARMv7-A

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 8. Plain `char` is unsigned. Little-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `thumbv7m-none-eabi` | bare metal, ARMv7-M (Cortex-M3) | ELF | `thumbv7m`, `armv7m-none-eabi`, `arm-none-eabi` |
| `thumbv7em-none-eabi` | bare metal, ARMv7E-M (Cortex-M4, M7) | ELF | `thumbv7em`, `armv7em-none-eabi` |
| `thumbv6m-none-eabi` | bare metal, ARMv6-M (Cortex-M0, M0+) | ELF | `thumbv6m`, `armv6m-none-eabi` |
| `thumbv8m.base-none-eabi` | bare metal, ARMv8-M Baseline (Cortex-M23) | ELF | `thumbv8m.base`, `armv8m.base-none-eabi` |
| `thumbv8m.main-none-eabi` | bare metal, ARMv8-M Mainline (Cortex-M33, M55) | ELF | `thumbv8m.main`, `thumbv8m-none-eabi`, `armv8m.main-none-eabi` |
| `thumbv7em-none-eabihf` | bare metal, ARMv7E-M (Cortex-M4, M7), hardware float | ELF |  |
| `armv7a-none-eabi` | bare metal, ARMv7-A in ARM state | ELF | `armv7a`, `armv7-none-eabi`, `armv7a-unknown-none-eabi` |
| `armv7a-none-eabihf` | bare metal, ARMv7-A in ARM state, hardware float | ELF | `armv7a-unknown-none-eabihf` |
| `thumbv8m.main-none-eabihf` | bare metal, ARMv8-M Mainline (Cortex-M33, M55), hardware float | ELF |  |

## RISC-V, 32-bit

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 16. Plain `char` is unsigned. Little-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `riscv32-unknown-elf` | bare metal | ELF | `riscv32`, `riscv32-elf`, `rv32` |

## RISC-V, 64-bit

`int` is 4 bytes, `long` 8, a pointer 8, `double` 8 and `long double` 16. Plain `char` is unsigned. Little-endian; `__int128` exists.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `riscv64-unknown-elf` | bare metal | ELF | `riscv64`, `riscv64-elf`, `rv64` |

## AVR

`int` is 2 bytes, `long` 4, a pointer 2, `double` 4 and `long double` 4. Plain `char` is signed. Little-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `avr` | bare metal | ELF | `avr-none-elf`, `avr-elf`, `avr-unknown-none` |

## MIPS32

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 8. Plain `char` is signed. Either byte order (the target names it).

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `mipsel-none-elf` | bare metal | ELF | `mipsel-unknown-elf`, `mipsel-elf`, `mipsel` |
| `mips-none-elf` | bare metal, big-endian | ELF | `mips-unknown-elf`, `mips-elf`, `mips` |

## MIPS64

`int` is 4 bytes, `long` 8, a pointer 8, `double` 8 and `long double` 16. Plain `char` is signed. Either byte order (the target names it); `__int128` exists.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `mips64el-none-elf` | bare metal | ELF | `mips64el-unknown-elf`, `mips64el-elf`, `mips64el` |
| `mips64-none-elf` | bare metal, big-endian | ELF | `mips64-unknown-elf`, `mips64-elf`, `mips64` |

## LoongArch64

`int` is 4 bytes, `long` 8, a pointer 8, `double` 8 and `long double` 16. Plain `char` is signed. Little-endian; `__int128` exists.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `loongarch64-unknown-elf` | bare metal | ELF | `loongarch64-none-elf`, `loongarch64-elf`, `loongarch64` |

## TriCore

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 8. Plain `char` is signed. Little-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `tricore-none-elf` | bare metal | ELF | `tricore-elf`, `tricore-unknown-elf`, `tricore` |

## Xtensa (ESP32)

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 8. Plain `char` is unsigned. Little-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `xtensa-none-elf` | bare metal | ELF | `xtensa-esp32-elf`, `xtensa-esp32s3-elf`, `xtensa-esp-elf`, `xtensa-elf`, `xtensa` |

## PowerPC, 32-bit

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 8. Plain `char` is unsigned. Big-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `powerpc-none-eabi` | bare metal | ELF | `powerpc-unknown-eabi`, `powerpc-eabi`, `powerpc`, `ppc` |

## Renesas RX

`int` is 4 bytes, `long` 4, a pointer 4, `double` 4 and `long double` 4. Plain `char` is unsigned. Little-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `rx-none-elf` | bare metal | ELF | `rx-elf`, `rx-unknown-elf`, `rx` |

## SPARC V8 (LEON)

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 16. Plain `char` is signed. Big-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `sparc-none-elf` | bare metal | ELF | `sparc-unknown-elf`, `sparc-elf`, `sparc-gaisler-elf`, `sparc` |

## ColdFire (m68k)

`int` is 4 bytes, `long` 4, a pointer 4, `double` 8 and `long double` 8. Plain `char` is signed. Big-endian.

| Target | Runs on | Object format | Also spelled |
|---|---|---|---|
| `m68k-none-elf` | bare metal | ELF | `m68k-unknown-elf`, `m68k-elf`, `m68k` |
