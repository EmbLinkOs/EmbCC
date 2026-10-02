# EmbCC

EmbCC is a C and C++ compiler with its own preprocessor, assemblers,
linker (`embld`) and debugger (`embdbg`). One `embcc` process
preprocesses, compiles and assembles, and links x86-64 ELF programs; it
starts no other program. EmbCC is written in C99, compiles its own
sources, and runs on macOS and Linux. Its primary target is EmbLinkOS on
x86-64.

## Targets

| Target | `--target=` | Output |
|---|---|---|
| x86-64, bare metal and EmbLinkOS | `x86_64-elf` (the default), `x86_64-emblink` | ELF64 objects and executables; `embld --embx` writes EmbLinkOS images |
| x86-64 Linux | `x86_64-linux-gnu` | static ELF64 executables on EmbCC's own C library |
| AArch64, bare metal, EmbLinkOS and Linux | `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` | ELF64 objects, linked with another toolchain's linker |
| ARM Cortex-M | `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` | ELF32 objects, linked by `embld` |
| RISC-V | `riscv32-unknown-elf`, `riscv64-unknown-elf` | ELF objects, linked by `embld` |
| AVR (ATmega328P) | `avr` | ELF32 objects, linked by `embld` |
| macOS | `x86_64-apple-darwin`, `aarch64-apple-darwin` | Mach-O objects for the system linker |
| Windows | `x86_64-windows-gnu` | COFF objects, not yet compatible with the Microsoft x64 ABI |

C is supported on every target. C++ is supported on the x86-64 and
AArch64 targets; Cortex-M, RISC-V and AVR are C only. An unknown
`--target=` lists every triple EmbCC accepts. Each target's ABI, options
and limitations are in [Targets](docs/manual/targets.md) and
[Status](docs/internals/status.md).

## Building

EmbCC needs a C99 compiler (`cc`, or set `CC=`) and GNU `make`:

```sh
make embcc     # the compiler, ./embcc
make all       # embcc, embread, embld, embas, embls and embidx
make embdbg    # the debugger, which is not part of all
make check     # compile and run every program in tests/exec and tests/cxx
```

Name a target: plain `make` builds only one object and leaves `./embcc`
as it was. The libraries, `make install`, the test suites and the tools
each needs are described in
[Getting started](docs/manual/getting-started.md).

## Example

```c
#include <stdio.h>

int main(void)
{
    printf("hello, world\n");
    return 0;
}
```

On an Apple silicon Mac, EmbCC compiles `hello.c` and the system linker
links it:

```sh
./embcc --target=aarch64-apple-darwin -O2 -c hello.c -o hello.o
cc hello.o -o hello
./hello
```

On Linux x86-64, `make libc-linux-x86_64` builds the C library, and
`./embcc --target=x86_64-linux-gnu -O2 hello.c -o hello` compiles and
links a static executable in one step. The file is written without
execute permission; run `chmod +x hello` before `./hello`. Cross builds
for the boards are in [Getting started](docs/manual/getting-started.md).

## Documentation

- [docs/README.md](docs/README.md): the index of the manual and the
  internals reference.
- [Getting started](docs/manual/getting-started.md): building,
  installing, first programs and cross builds.
- [Contributing](docs/internals/contributing.md): the rules for changing
  EmbCC, the code style, commit messages and the tests a change needs.
