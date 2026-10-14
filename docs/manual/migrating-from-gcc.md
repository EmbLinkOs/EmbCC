# Migrating from GCC

A project built with GCC builds with EmbCC unchanged: EmbCC reads GCC's
option spellings, answers to GCC's program names and links with objects
GCC compiled. This page is the one place in the manual that talks in
GCC's terms. Everywhere else, EmbCC uses its own names.

## Switching a build

| Your build | What to do |
|---|---|
| A Makefile or script that runs `arm-none-eabi-gcc`, `-ar`, `-objcopy`, `-size` | `make install-gnu`, put its `bin/` first in `PATH`, and change nothing else ([The GCC names](build-systems.md#the-gcc-names)). |
| A CMake toolchain file written for GCC | Keep it, with the GCC names installed as above; or use the one EmbCC ships ([The CMake toolchain file](build-systems.md#the-cmake-toolchain-file)). |
| `CC=gcc` (or `CC=cc`) in a Makefile | `CC=embcc`, and `--target=` for a cross build. |
| A new project | Write EmbCC's options directly: `embcc --help` lists them. |

## Options

EmbCC's options are words, plus POSIX's `c99` letters (`-c -o -E -I -D
-U -L -l -g -O`). GCC's spelling of each one is accepted too. The
commonest:

| GCC | EmbCC |
|---|---|
| `-Os`, `-O2` | `--opt=size`, `--opt=speed` (or `-Os`, `-O2`: `-O` is POSIX's) |
| `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard` | `--cpu=cortex-m4 --fpu=fpv4-sp-d16 --float-abi=hard` |
| `-mmcu=atmega328p` | `--mcu=atmega328p` |
| `-ffunction-sections -fdata-sections` | `--sections=functions,data` |
| `-ffreestanding` | `--freestanding` |
| `-fshort-enums` | `--enums=short` |
| `-fno-common` | `--no-common` |
| `-fstack-usage` | `--stack-usage` |
| `-Wall -Wextra` | `--warn=all --warn=extra` |
| `-Wconversion`, `-Wno-unused-parameter` | `--warn=conversion`, `--no-warn=unused-parameter` |
| `-Werror`, `-Werror=format` | `--warnings-are-errors`, `--error=format` |
| `-MMD -MF obj.d` | `--deps=obj.d` |
| `-T link.ld`, `-Wl,-Map=out.map` | `--link-script=link.ld`, `--linker-arg=-Map=out.map` |
| `-nostdlib` | `--no-stdlib` |

`embcc --help=gcc` prints the whole list: every GCC spelling EmbCC reads,
EmbCC's name for each, and the GCC flags EmbCC has no name for. Each of
those is accepted because it changes nothing in EmbCC, or read so that it
can be refused by name. The [option reference](options.md) has the same
list, with what each option does.

## What does not change

These are contracts with the code and objects you already have, so
EmbCC keeps GCC's form exactly:

- **The ABIs.** Calling conventions, struct layout and alignment, and the
  ARM build attributes, as each target's ABI defines them. An object
  EmbCC compiles links with one GCC compiled, in either direction.
- **The runtime helpers.** EmbCC calls libgcc's routines by libgcc's names
  (`__mulsi3`, `__aeabi_uidiv`, `__udivdi3`, ...), and ships its own in
  `librt.a`.
- **The source.** `__attribute__((...))`, `asm` statements and their
  constraints, `#pragma GCC diagnostic`, and GNU C's extensions.
- **The linker scripts.** EmbLD reads GNU ld's script language.
- **JSON diagnostics.** `--diagnostics=json` writes GCC's JSON format, so
  editors and CI tools that read GCC's read EmbCC's.

## What differs

- **A warning names EmbCC's option.** `warning: unused variable 'x'
  [--warn=unused-variable]` where GCC writes `[-Wunused-variable]`.
  `--no-warn=unused-variable` and GCC's `-Wno-unused-variable` both turn
  it off. The JSON output still says `"option": "-Wunused-variable"`.
- **One dialect per language.** `--std=` (and `-std=`) sets the standard's
  macros. EmbCC reads C11 with the GNU extensions, and C++20, whichever
  standard is named ([C language](c-language.md)).
- **`-O3` is `-O2`, and `-Oz` is `-Os`.** EmbCC has one speed goal and one
  size goal ([Optimization](optimization.md)).
- **Signed overflow wraps.** EmbCC compiles as GCC does with `-fwrapv`,
  always: no optimization assumes overflow cannot happen.
- **Some GCC flags are refused** rather than ignored, when EmbCC cannot do
  what they ask:
  - `-flto`;
  - `-fPIC` and `-fPIE` (EmbCC makes no position-independent code yet);
  - `-fsanitize=address` (EmbCC's sanitizer checks trap: `-fsanitize=undefined
    -fsanitize-trap` is accepted).

  The message names the flag. A build that relies on one of them should
  not appear to succeed while getting none of what it asked for.
