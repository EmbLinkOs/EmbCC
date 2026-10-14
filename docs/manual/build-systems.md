# Build systems: CMake, Make and GCC toolchain files

A project usually reaches its compiler through a build system: a CMake
toolchain file, or a Makefile that names `arm-none-eabi-gcc`, `-ar`,
`-objcopy` and `-size`. EmbCC answers all of those names. There are two
ways to use it:

- **The CMake toolchain file EmbCC ships**, for a new project or one you
  are moving to EmbCC.
- **The GCC names**, for a project whose build is written for a GCC cross
  toolchain. Install the names, put them first in `PATH`, and change
  nothing else.

## The CMake toolchain file

```sh
cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/usr/local/share/embcc/cmake/embcc.cmake \
    -DEMBCC_TARGET=thumbv7em-none-eabihf \
    "-DEMBCC_FLAGS=-mcpu=cortex-m4 -Os"
cmake --build build
```

| Variable | Meaning |
|---|---|
| `EMBCC_TARGET` | The triple, as `embcc --target=` takes it. Without it, the compiler's default target. |
| `EMBCC_FLAGS` | Flags for every compile and link: a CPU, an FPU, an optimization level. |

The file sets:

- the C, C++ and assembler compilers to `embcc`;
- `CMAKE_SYSTEM_NAME` from the triple: `Generic` for a bare-metal triple
  (CMake's compiler checks then build a static library, not a program),
  `Linux`, `Darwin` or `Windows` otherwise;
- `CMAKE_AR` to `embar`, which writes the symbol index as it writes the
  archive, so there is no separate ranlib step;
- `CMAKE_OBJCOPY`, `CMAKE_SIZE` and `CMAKE_NM` to `embcc-objcopy`,
  `embcc-size` and `embcc-nm`;
- header dependencies: every compile writes a `-MD` dependency file, so
  editing a header rebuilds the sources that include it.

It finds the tools next to itself: in an installation it is in
`share/embcc/cmake/` and they are in `bin/`; in a source tree it is in
`cmake/` and they are at the top (`make` builds the `embcc-*` names
there).

A post-build step written for binutils works as it is:

```cmake
add_custom_command(TARGET app POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:app> app.hex
  COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:app>)
```

## The GCC names

```sh
make install-gnu PREFIX=/opt/embcc TRIPLES="arm-none-eabi riscv64-unknown-elf"
export PATH=/opt/embcc/bin:$PATH
```

For each triple this links `<triple>-gcc` and `<triple>-cc` to `embcc`,
`-ar` and `-ranlib` to `embar`, `-objcopy` to `embpack`, and `-size` and
`-nm` to `embmap`. The default `TRIPLES` are `arm-none-eabi`,
`riscv64-unknown-elf`, `riscv32-unknown-elf` and `avr`.

Called by such a name, `embcc` compiles for the triple in it, as clang does.
A version after the name is skipped (`arm-none-eabi-gcc-13.2`), and
`--target=` on the command line still decides. An unknown triple is
refused:

```text
embcc: error: called as 'bogus-thing-gcc', and 'bogus-thing' is not a target EmbCC knows; they are:
```

`riscv64-unknown-elf-gcc` builds RV32 code under `-march=rv32...`, as GCC's
multilib toolchain of that name does. The other way round is refused: a
`riscv32` triple is RV32 only, and so is a hosted one of either width.

One thing a CMake toolchain file must say: the archiver. CMake derives
`<prefix>-ar` and `<prefix>-ranlib` from the compiler's name only for a
compiler it identifies as GCC or Clang, and it does not identify EmbCC
(which does not define `__GNUC__`). It then uses the system `ar`, which on
Linux handles ELF archives, and on macOS does not: Apple's `ranlib` drops
the ELF members. So name them, as many arm-none-eabi toolchain files
already do:

```cmake
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)
```

A Makefile that names `$(PREFIX)ar` needs nothing.

What each binutils name does:

| Name | Program | What it takes |
|---|---|---|
| `-ar` | `embar` | `r`, `q`, `c`, `s`, `t`, `x`, `d` (and `D`, `u`, `v`, `o`, which change nothing: the output is always deterministic) |
| `-ranlib` | `embar` | `ranlib ARCHIVE...`: each archive's symbol index, written again |
| `-objcopy` | `embpack` | `-O binary`, `-O ihex`, `-O srec`, `-R SECTION`, `-j SECTION` (both with `*` and `?` patterns), `--gap-fill BYTE`, `--pad-to ADDRESS`, `-I elf...`. `-S`, `-g` and `--strip-*` are accepted and change nothing in an image. |
| `-size` | `embmap` | Berkeley (the default, `-B`) and SysV (`-A`) tables, `-d`, `-o`, `-x`, `-t` |
| `-nm` | `embmap` | `-S`, `-g`, `-u`, `--defined-only`, `-n`, `-p`, `-r` |

The same programs answer to `embcc-ar`, `embcc-ranlib`, `embcc-objcopy`,
`embcc-size` and `embcc-nm`, which `make install` always installs.

What they do not do:

- `objcopy` writes images. An ELF-to-ELF copy (no `-O`, or `-O elf32-...`)
  is refused by name.
- `size` and `nm` read ELF files, not archives.

## Linking

A bare-metal link needs a memory map: a linker script (`-T FILE.ld`,
`-TFILE.ld`) or `-Wl,-Ttext=` and `-Wl,-Tdata=`. GNU ld scripts are read
by embld; see [embld](tools/embld.md). `--specs=nano.specs` and
`--specs=nosys.specs` are accepted and ignored, because EmbCC's C library
is already small and its system calls are weak defaults (see
[Libraries](libraries.md)).

On macOS a program is linked by Apple's linker; see
[Getting started](getting-started.md#macos).
