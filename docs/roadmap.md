# Road to EmbCC 1.0

EmbCC 0.0.1-beta is the first public release. This page says what
1.0.0 will promise, how far EmbCC is from each promise today, and the
releases that close the distance, in order. It is for people trying the
beta who want to know what is coming, and for contributors choosing
what to work on. It is revised at every release.

## What 1.0.0 promises

1.0.0 is the release whose behavior you can build a product on. From
1.0.0 on, an option, a diagnostic's identity or a target triple that
worked keeps working through every 1.x release, a wrong-code bug is
treated as the most urgent kind there is, and the manual describes what
the compiler does, not what it was meant to do.

Each promise below has a measure, so that "ready" is a number and not a
feeling. "Today" is the state at 0.0.1-beta (October 2026).

| Area | 1.0.0 requires | Today |
|---|---|---|
| Wrong code | No open wrong-code bug. The random-program fuzzer runs clean on every QEMU board for each release. GCC's torture suite (`gcc.c-torture/execute`) passes on every board it compiles for. | Fuzzer clean on the boards it covers (x86-64, AArch64, Cortex-M4, Cortex-M0, RV32, RV64). The torture suite has only been run for compile acceptance. |
| Testing on every change | Each pull request is built and tested on Linux and macOS by CI before it can merge. The full matrix and the fuzzer run every night. | No CI on pull requests. The matrix runs on a developer machine. |
| Coverage | Every construct EmbCC refuses is either implemented or listed in the manual as permanent, with the reason and a workaround. At least 99% of the GCC torture files that GCC accepts are accepted. | 110 distinct backend refusals and 134 front-end and driver refusals in the source. 95.4% of the torture files GCC accepts are accepted. |
| Code size | `-Os`, object by object against clang: at most 1.10 on Cortex-M and RISC-V, 1.25 on AVR. | Cortex-M4 1.14, Cortex-M0 1.17, RV32 1.28, RV64 1.28, AVR 1.53, x86-64 1.70, AArch64 1.49; without math, Cortex-M4 1.08 and RISC-V 1.12 (`tools/scoreboard.sh`, 2026-10-24). |
| Debugging | `-g` changes no code (already true). A variable held in a register is shown by the debugger, not reported as optimized out. Breakpoints, stepping and locals are checked through GDB on every board family EmbSim or QEMU runs. | `-g` changes no code on any target. No location lists yet, so register-held variables show as `<optimized out>`. `-g` is refused for Windows. |
| Stable interface | EmbCC's own option names are final. GCC spellings are accepted by one compatibility layer. Removing anything follows the deprecation policy below. The default target is settled. | EmbCC's own option vocabulary is in place. The GCC compatibility layer is still spread through the driver. The default target is `x86_64-elf`. |
| Real hardware | Programs built by EmbCC verified on at least one real board per major family: Cortex-M0+, Cortex-M3/M4, AVR, RISC-V, PIC32. `embflash` checked with a real probe. | Verified on QEMU and EmbSim only. |
| Installing | Linux and macOS (x86-64 and arm64) packages from every release, a Homebrew tap, and `install.sh` with no arguments installing the latest release. A Windows host build. | Linux and macOS tarballs and `.deb` files from the release workflow. No Homebrew tap repository yet. No Windows host build. |
| Documentation | The manual and the status page are re-audited against the compiler before each release. A getting-started page for each EmbSim board. Migration guides from `arm-none-eabi-gcc`, `avr-gcc` and XC32. | The manual is complete, but the status page has drifted (it still lists C++ as refused on Cortex-M). The general [Migrating from GCC](manual/migrating-from-gcc.md) page exists; none yet for a particular board toolchain. |

## The releases

Each release has a theme and is done when its exit criteria hold, not on
a date. The windows are estimates at the current pace; they move when
the work does.

### 0.0.x: the beta, fixed

*From 0.0.1-beta, as needed.*

- Fix what beta users report, wrong code first.
- Release follow-ups: the Homebrew tap repository, the Homebrew, RPM and
  Arch recipes pointed at each release, and the release notes in the
  repository.

**Done when:** no reported wrong-code bug is open.

### 0.1.0: trust

*Target: November 2026.*

The goal is that every change is tested before it lands, without
depending on one developer's machine.

- CI on every pull request: build with GCC and clang on Linux and
  macOS, `make check`, and the golden tests that need no QEMU.
- A nightly workflow: the full matrix on QEMU (every board) and the
  fuzzer, with failures filed as issues.
- The torture suite as a test: compile acceptance and `execute` results
  tracked per board, every regression a failure.
- The status page and the target refusal tables re-audited against the
  compiler (`tools/scoreboard.sh`, the limits audit), and kept true by a
  test.
- Issue templates, a security policy, and a contributor guide that
  explains the test discipline.
- 0.1.0 is the first release without a pre-release suffix, so
  `install.sh` with no `--version` installs it.

**Done when:** a pull request cannot merge with a failing check, and
three nightly runs in a row are clean.

### 0.2.0: a stable interface

*Target: December 2026.*

Everything that would break a user's build has to change before 1.0 or
not at all.

- The GCC compatibility layer: the driver reads options by identity
  from one table; every GCC spelling is an alias in one module,
  documented on a migration page.
- The default target: decide whether `embcc` with no `--target` builds
  for the host (as clang does) or stays `x86_64-elf`, and ship the
  decision with a warning period.
- The source layout: the planned move to `compiler/`, `targets/`,
  `runtime/`, `sim/`, `tools/` happens now or is dropped. Either way,
  the build and install interface (`make`, `./embcc`, `embld`) stays.
- A written deprecation policy: an option or behavior is removed only
  after one minor release in which it warns.
- Diagnostics keep their names and ids from here on, so build scripts
  can rely on them.

**Done when:** the option table, the default target and the layout are
final, and the deprecation policy is published.

### 0.3.0: debugging you can ship

*Target: January 2027.*

Embedded teams debug the stripped `-O2 -g` image they flash. It has to
be fully debuggable.

- Location lists: the DWARF says which register or slot holds each
  variable at each address, so optimized code shows its variables.
- Call frame information for every target, which also gives C++
  exceptions their unwind tables off x86-64 and AArch64.
- `-g` for Windows (CodeView or DWARF in COFF).
- A GDB session test per board family, in EmbSim and QEMU: break,
  step, backtrace, locals, watchpoints.

**Done when:** `bt` and `info locals` are right at every stop of the
GDB test on every board family, at `-O2`.

### 0.4.0: code quality

*Target: February 2027.*

- Close the size gap to the 1.0 targets: register pressure on Thumb
  (the 32-bit encodings that `r8`-`r12` force), 64-bit values on 32-bit
  targets, AVR's register runs.
- Division by a constant on AVR, which still calls a helper for
  `x / 10` though the ATmega has a hardware multiplier.
- Speed measured with the cycle estimates on Cortex-M4 and RV32 and
  compared with clang. The 1.0 speed target is set from that baseline.
- Compile speed tracked per release (`lib/libc` at `-O2`, against
  clang), with no input shape allowed to go quadratic.

**Done when:** the scoreboard meets the 1.0 size targets.

### 0.5.0: coverage

*Target: March 2027.*

Every refusal either implemented or declared permanent:

- `asm goto`, global register variables,
  `__builtin_setjmp`/`__builtin_longjmp`, nested functions (or a
  permanent refusal, as clang has). (`_Float16` and `__fp16` are done:
  a `_Float16` through `...`, and a struct of them by value with the ARM
  and RISC-V hard-float conventions, remain.)
- C++ exceptions on Cortex-M, RV32 and the other 32-bit targets (needs
  0.3.0's unwind tables). C++ on AVR, or a permanent refusal.
- RISC-V without the A extension (`rv32imc`, the ESP32-C3's ISA): done;
  without M (`rv32ic`) and the E base (`rv32ec`, the CH32V003) remain.
- The Microsoft x64 calling convention in full, so Windows objects link
  with other compilers' objects.
- `embld` linking AArch64 programs; an AT&T-syntax x86-64 assembler for
  `.s` files; `-S` on every target.
- A Windows host build of the whole toolchain.

**Done when:** the refusal count is zero apart from the permanent list,
and 99% of the torture files GCC accepts are accepted.

### 0.9.0: release candidate

*Target: April 2027.*

- Feature freeze: bug fixes only until 1.0.0.
- Real hardware: the boards below, each running the same test programs
  as its EmbSim model, flashed with `embflash` through a real probe:
  Raspberry Pi Pico (Cortex-M0+), Blue Pill (Cortex-M3), nRF52840 DK
  (Cortex-M4F), Arduino Uno (AVR), ESP32-C3-DevKitM-1 (RISC-V),
  Explorer 16/32 or Curiosity PIC32MZ EF (PIC32).
- Migration guides from `arm-none-eabi-gcc`, `avr-gcc` and XC32.
- The whole manual re-read against the compiler.

Then `1.0.0-rc.1`, `rc.2`, ... until one goes two weeks without a
release-blocking bug. That one becomes **1.0.0**.

## Decisions to make

Each of these changes the plan, and none can be made by code alone:

| Decision | Needed by | Options |
|---|---|---|
| The default target | 0.2.0 | The host, as clang does; or `x86_64-elf`, as now. EmbLinkOS's build relies on today's default and would need `--target=x86_64-elf` first. |
| The source layout | 0.2.0 | Move to the planned layout before 1.0, or keep `src/`. |
| Which refusals are permanent | 0.5.0 | Nested functions, C++ on AVR, GCC options for other compilers' ABIs (`-mabicalls`, RX `-m64bit-doubles`, ...). |
| Hardware for validation | 0.9.0 | The six boards above and a debug probe (a CMSIS-DAP or J-Link, and a PICkit or MPLAB Snap for PIC32). |
| Windows host | 0.5.0 | A native Windows build, or Windows through WSL only. |
| The Homebrew tap and the website | 0.0.x | Creating `EmbLinkOs/homebrew-embcc` and turning on GitHub Pages are repository settings. |

## Versions

- `0.0.x-beta`: the first betas. GitHub marks them as pre-releases, so
  `install.sh` needs `--version` to install one.
- `0.MINOR.PATCH` from 0.1.0: ordinary releases. A minor release may
  still change behavior (with a note in its release notes). A patch
  release only fixes bugs.
- `1.0.0-rc.N`: release candidates, pre-releases again.
- `1.x`: no change breaks a build that worked, except to fix wrong
  code; removals follow the deprecation policy.

The version is `EMBCC_VERSION` in `src/driver/version.h`. How a release
is made is in [packaging/README.md](../packaging/README.md#making-a-release).
