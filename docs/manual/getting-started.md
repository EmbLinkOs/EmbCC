# Getting started

This page takes you from a source checkout to running programs: building
EmbCC and its libraries, checking the build, installing it, compiling a
first program for the host, and cross-compiling for each supported board
and running the result under QEMU. It is for people who are new to EmbCC.
The command-line reference is [Invoking EmbCC](invoking.md).

All commands on this page are run from the top of the EmbCC source tree
unless they say otherwise.

## Host requirements

EmbCC builds and runs on macOS and Linux. To build the compiler you need:

- a C99 compiler, run as `cc` (set `CC=` to use another);
- GNU `make`.

Nothing else is needed for `embcc` itself. The libraries, the test suites
and running cross-compiled programs need further tools:

| Tool | Needed for |
|---|---|
| `x86_64-elf-ar` (or set `EMBCC_X86_AR`) | the x86-64 C and C++ libraries (`libc-x86_64`, `libc-linux-x86_64`, `libcxx-x86_64`, `libcxx-linux-x86_64`, `libc-emblinkos`) |
| `aarch64-elf-ar` (or set `EMBCC_AARCH64_AR`) | the AArch64 C and C++ libraries (`libc-aarch64`, `libc-linux-aarch64`, `libcxx-aarch64`, `libcxx-linux-aarch64`) |
| `llvm-ar`, or the system `ar` (or set `EMBCC_AR`) | the embedded compiler runtimes (`rt-embedded`) |
| `qemu-system-arm`, `qemu-system-riscv32`, `qemu-system-riscv64`, `qemu-system-avr`, `qemu-system-aarch64`, `qemu-system-x86_64` | running cross-compiled programs and the test suites |
| `x86_64-elf-gcc`, `x86_64-elf-ld`, `x86_64-elf-objcopy` and a newlib built for `x86_64-elf` | `make check` and `make test` on any host other than Linux x86-64 |
| `aarch64-elf-gcc`, `aarch64-elf-ld` and a newlib built for `aarch64-elf` | `make test-arm64` |
| `llvm-mc`, `clang` | the parts of some golden tests that use them as a reference |

A golden test whose tools are missing reports `SKIP` with the reason; it
is never counted as a pass.

## Building from source

Build the compiler with:

```sh
make embcc
```

This compiles the sources into `build/` and links `./embcc` at the top of
the tree. Use `make embcc` (or `make all`), not plain `make`: the first
rule in the Makefile builds only `build/embdbg_core.o`, so a plain `make`
leaves `./embcc` as it was.

The other tools are separate targets:

| Target | Builds |
|---|---|
| `make embcc` | the compiler driver, with the integrated linker inside it |
| `make embld` | the linker, [`embld`](tools/embld.md), needed to link firmware |
| `make embas` | the NASM-syntax x86-64 assembler, [`embas`](tools/embas.md) |
| `make embread` | the EMBX image reader, [`embread`](tools/embread.md) |
| `make embls` | the language server, [`embls`](tools/embls.md) |
| `make embidx` | the cross-unit index, [`embidx`](tools/embidx.md) |
| `make embdbg` | the debugger, [`embdbg`](tools/embdbg.md) |
| `make all` | `embcc`, `embread`, `embld`, `embas`, `embls` and `embidx` (not `embdbg`) |
| `make clean` | removes `build/` and the tool binaries other than `embidx` |

The build can be adjusted with these `make` variables:

| Variable | Default | Meaning |
|---|---|---|
| `CC` | `cc` | the host C compiler |
| `CFLAGS` | `-std=c99 -Wall -Wextra -Werror -g` | flags for the host compiler |
| `BUILD` | `build` | the object directory |
| `DEFAULT_TARGET` | empty (`x86_64-elf`) | the target `embcc` uses when given no `--target=` |

`make CC=clang BUILD=/tmp/embcc-clang /tmp/embcc-clang/embcc` builds a
separate compiler entirely inside `/tmp/embcc-clang`, leaving `./embcc`
untouched.

Every object depends on every header under `src/`, so after editing one
`make` rebuilds the whole compiler.

### Choosing the default target

A compiler that is used mostly for one board can be built to target it
when no `--target=` is given:

```sh
make clean
make DEFAULT_TARGET=riscv32-unknown-elf embcc
./embcc -dumpmachine                     # riscv32-unknown-elf
./embcc -c main.c -o main.o              # compiled for RV32
```

The binary still contains every backend, and `--target=` still selects
any of them. Run `make clean` first: the value is compiled into one
object, and `make` does not rebuild objects when only a variable changes.

The environment variable `EMBCC_DEFAULT_TARGET` overrides the built-in
default for one shell, and `--target=` overrides both. See
[The default target](invoking.md#the-default-target).

### Building the libraries

EmbCC's C library (`lib/libc`), C++ runtime (`lib/libcxx`) and compiler
runtime (`lib/rt`) are compiled by the `./embcc` just built. Each target
below builds `embcc` first if it is out of date.

| Target | Produces | For `--target=` |
|---|---|---|
| `libc-x86_64` | `build/libc/x86_64/libc.a` | `x86_64-elf` |
| `libc-aarch64` | `build/libc/aarch64/libc.a` | `aarch64-elf` |
| `libc` | both of the above | |
| `libc-linux-x86_64` | `build/libc/linux-x86_64/libc.a`, `librt.a`, `crt1.o` | `x86_64-linux-gnu` |
| `libc-linux-aarch64` | `build/libc/linux-aarch64/libc.a`, `librt.a`, `crt1.o` | `aarch64-linux-gnu` |
| `libc-linux` | both of the above | |
| `libcxx-x86_64`, `libcxx-aarch64` | `build/libcxx/x86_64/libcxx.a`, `build/libcxx/aarch64/libcxx.a` | `x86_64-elf`, `aarch64-elf` |
| `libcxx-linux-x86_64`, `libcxx-linux-aarch64` | `build/libcxx/linux-x86_64/libcxx.a`, `build/libcxx/linux-aarch64/libcxx.a` | the `-linux-gnu` targets |
| `libcxx` | `libcxx-x86_64` and `libcxx-aarch64` | |
| `libc-linux-all` | `libc-linux` and both Linux C++ runtimes | |
| `rt-embedded` | `build/libc/TRIPLE/librt.a` for `avr`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` and `riscv32-unknown-elf` | the embedded targets |
| `libc-embedded` | `build/libc/TRIPLE/libc.a` for `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf`, `riscv32-unknown-elf` and `riscv64-unknown-elf` | the embedded targets, with no operating system |
| `libc-emblinkos` | `build/libc/emblinkos/libc.a` | EmbLinkOS; needs the EmbLinkOS source tree (`make libc-emblinkos EMBLINKOS=/path/to/EmbLinkOs`, default `$HOME/EmbLinkOs`) |

There is no compiler runtime for `riscv64-unknown-elf`. The libraries and
what they contain are described in [Libraries](libraries.md).

### Checking the build

| Target | Runs | Builds first |
|---|---|---|
| `make check` | every program in `tests/exec` and `tests/cxx`, compiled, linked and run; the quick check while working on the compiler | `embcc`, `libc-x86_64`, `libcxx-x86_64` |
| `make test` | the full suite: the programs above plus every golden test | `embcc`, `embread`, `embld`, `embdbg`, `embls`, `embas`, the x86-64 and x86-64 Linux C and C++ libraries |
| `make test-arm64` | the suite for `aarch64-elf`, run under `qemu-system-aarch64` | `embcc` and the AArch64 C and C++ libraries |
| `make test-libstdcxx` | GCC's libstdc++ built by EmbCC, and the C++ tests linked against it, for both targets; takes several minutes | `embcc` |

The test programs run on the architecture they were compiled for. For
`x86_64-elf`, on a Linux x86-64 host they are linked with the host `cc`
and run directly; on any other host they are linked into a Multiboot image
against a cross newlib and booted under `qemu-system-x86_64`.
`EMBCC_X86_RUNNER=host` or `EMBCC_X86_RUNNER=qemu` forces one or the
other. The cross newlib is looked for in `~/cross/newlib-c99/x86_64-elf`
(`EMBCC_X86_NEWLIB`) and `~/cross/newlib-aarch64-c99/aarch64-elf`
(`EMBCC_AARCH64_NEWLIB`).

One golden test uses `embidx`, which `make test` does not build; run
`make embidx` first, or that test reports that it was skipped. The test
suite is described in [Testing](../internals/testing.md).

## Installing

```sh
make install PREFIX=/opt/embcc
```

`PREFIX` defaults to `/usr/local`. `make install` builds `all`, the C and
C++ libraries for the `-elf` and `-linux-gnu` targets and the embedded
runtimes, so the `ar` tools listed under
[Host requirements](#host-requirements) must be present. It then copies
`embcc`, `embld`, `embas`, `embread`, `embdbg`, `embls` and `embidx` into
`PREFIX/bin`. `embdbg` is not part of `all`; run `make embdbg` before
installing if you want it installed.

`DESTDIR` stages the installation under another root, as package builds
do: `make install DESTDIR=/tmp/stage PREFIX=/usr` writes
`/tmp/stage/usr/...`. `make install-files` performs only the copying step,
without building anything. `make uninstall PREFIX=...` removes the
binaries and the versioned library directory.

The installed layout is:

```text
PREFIX/bin/embcc  (and embld, embas, embread, embdbg, embls, embidx)
PREFIX/lib/embcc/VERSION/
    include/            C library headers
    include/c++/        C++ library headers
    freestanding/       <stddef.h>, <stdarg.h>, <stdint.h>, <limits.h>, <float.h>, ...
    x86_64-elf/         libc.a, libcxx.a
    aarch64-elf/        libc.a, libcxx.a
    x86_64-linux-gnu/   crt1.o, libc.a, librt.a, libcxx.a, link.ld
    aarch64-linux-gnu/  crt1.o, libc.a, librt.a, libcxx.a, link.ld
    TRIPLE/             librt.a, and libc.a where libc-embedded builds one
```

`VERSION` is the version `embcc --version` prints (`1.0.0-m2.complete`).
Nothing in the binary refers to `PREFIX`: the compiler finds this
directory relative to its own location, so an installation can be moved
or unpacked anywhere. Check what a compiler found with:

```sh
/opt/embcc/bin/embcc --print-search-dirs
```

A compiler in a build tree uses the tree's `lib/` and `build/` directories
in the same way, so `./embcc` works without installing. Setting
`EMBCC_PREFIX` points a compiler at a different installation. The search
rules are in
[How the compiler finds its own files](invoking.md#how-the-compiler-finds-its-own-files).

## A first program on the host

### Linux on x86-64

The `x86_64-linux-gnu` target produces static executables that run on a
Linux kernel with no other C library, linked in the same `embcc` process.
Build the compiler and the Linux library, then compile and link:

```c
#include <stdio.h>

int main(void)
{
    printf("hello, world\n");
    return 0;
}
```

```sh
make embcc libc-linux-x86_64
./embcc --target=x86_64-linux-gnu -O2 hello.c -o hello
chmod +x hello
./hello
```

The linker writes the executable without execute permission, hence the
`chmod`. On a host that is not Linux x86-64, the same image can be booted
as the only process of a Linux kernel under QEMU with
`tests/harness/linux/run.sh x86_64 hello`. That script needs a kernel
image, which is not part of the repository: it looks for
`tests/harness/linux/vmlinuz-x86_64` or the file named by
`EMBCC_LINUX_KERNEL_X86_64`, and exits with status 126 when there is none.

To make `x86_64-linux-gnu` the default, see
[Choosing the default target](#choosing-the-default-target). C++ programs
also need `make libcxx-linux-x86_64`.

### macOS

On macOS, EmbCC writes Mach-O objects and the system linker links them.
On Apple silicon:

```sh
./embcc --target=aarch64-apple-darwin -O2 -c hello.c -o hello.o
cc hello.o -o hello
./hello
```

On an Intel Mac use `--target=x86_64-apple-darwin`. The driver also takes
Apple's spelling of the same choice, `-arch arm64` or `-arch x86_64`, and
the other flags Apple's toolchain and CMake pass: `-isysroot DIR`,
`-mmacosx-version-min=V` (ignored) and `-pthread`.

The headers are the macOS SDK's, so a program sees the C library it links
against, POSIX headers such as `<pthread.h>` and `<unistd.h>` included.
EmbCC uses the SDK named by `-isysroot`, else by `$SDKROOT`, else the
Command Line Tools or Xcode SDK. A few SDK headers are corrected for a
compiler that does not define `__GNUC__` (`include/darwin/`). `-g` is not
supported for the Darwin targets. See [Targets](targets.md).

### Bare-metal x86-64 and EmbLinkOS

`x86_64-elf` (the default target) and `x86_64-emblink` are freestanding:
`embcc` links them only when the program defines its own `_start`.
Normally you compile with `-c` and link with [`embld`](tools/embld.md)
together with your start-up code, as described in
[Bare-metal programming](embedded.md). To run a hosted-style program on
bare-metal x86-64 under QEMU, see
[x86-64 and AArch64 under QEMU](#x86-64-and-aarch64-under-qemu).

## Cross-compiling for a board

A firmware build has three steps: compile each file with `--target=` and
`-c`, link the objects together with start-up code using `embld`, and run
the image. The directories under `tests/harness/` hold the start-up code
(`boot.c` or `boot.S`), a minimal UART output (`io.c`, which provides
`writec`, `puts_` and `putn`), and `link.sh` and `run.sh` scripts for one
QEMU board each. They are what EmbCC's own tests use, and they are a
working starting point for your own firmware.

Build the compiler and the linker first:

```sh
make embcc embld
```

### The boards

| QEMU board | Processor | `--target=` | Harness | `embld` options |
|---|---|---|---|---|
| `microbit` | Cortex-M0 | `thumbv6m-none-eabi` | `tests/harness/thumb-m0` | `-e reset -Ttext 0x0 -Tdata 0x20000000` |
| `lm3s6965evb` | Cortex-M3 | `thumbv7m-none-eabi` | `tests/harness/thumb` | `-e reset -Ttext 0x0 -Tdata 0x20000000` |
| `mps2-an386` | Cortex-M4F | `thumbv7em-none-eabihf` | `tests/harness/thumb-m4f` | `-e reset -Ttext 0x0 -Tdata 0x20000000` |
| `mps2-an505` | Cortex-M33 | `thumbv8m.main-none-eabi` | `tests/harness/thumb-m33` | `-e reset -Ttext 0x10000000 -Tdata 0x10100000` |
| `virt` | RV32 | `riscv32-unknown-elf` | `tests/harness/riscv` | `-e _start -Ttext 0x80000000 -Tstack 0x80800000` |
| `virt` | RV64 | `riscv64-unknown-elf` | `tests/harness/riscv` | `-e _start -Ttext 0x80000000 -Tstack 0x80800000` |
| `uno` | ATmega328P | `avr` | `tests/harness/avr` | `-e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768` |

`-Ttext` is where the image is stored and executed from. `-Tdata` is where
writable data is addressed in RAM; `embld` stores its initial contents
after the text and defines the symbols the start-up code uses to copy it
into place and clear `.bss`. `-Tstack` makes `embld` set the stack pointer
before jumping to the entry point. The options are described in
[embld](tools/embld.md).

### Cortex-M3, step by step

This program uses the harness's output functions:

```c
void puts_(const char *s);
void putn(long v);

static int square(int x) { return x * x; }

int main(void)
{
    puts_("hello from EmbCC\n");
    for (int i = 1; i <= 5; i++)
        putn(square(i));
    puts_("\nDONE\n");
    return 0;
}
```

Compile the start-up code, the output functions and the program, then
link and run:

```sh
T=--target=thumbv7m-none-eabi
mkdir -p out/m3
./embcc $T -c tests/harness/thumb/boot.c -o out/m3/boot.o
./embcc $T -c tests/harness/thumb/io.c   -o out/m3/io.o
./embcc $T -O2 -c hello.c                -o out/m3/hello.o
./embld -e reset -Ttext 0x0 -Tdata 0x20000000 \
    out/m3/boot.o out/m3/io.o out/m3/hello.o -o out/m3/hello.elf
sh tests/harness/thumb/run.sh out/m3/hello.elf
```

```text
hello from EmbCC
1 4 9 16 25 
DONE
```

The vector table in `boot.c` is placed at address 0 by its `.vectors`
section; the reset handler copies `.data`, clears `.bss` and calls
`main`. When `main` returns, the handler executes a trap instruction,
the processor locks up, and QEMU stops. `run.sh` hides QEMU's own
messages, including the register dump it prints on lock-up, and limits
the run to `EMBCC_QEMU_TIMEOUT` seconds (default 10).

`tests/harness/thumb/link.sh OUT.elf PROGRAM.o...` runs the same `embld`
command, taking `boot.o` and `io.o` from the directory named by
`EMBCC_THUMB_HARNESS` and `embld` from `EMBLD` (default `./embld`):

```sh
EMBCC_THUMB_HARNESS=out/m3 sh tests/harness/thumb/link.sh out/m3/hello.elf out/m3/hello.o
```

### Cortex-M4F and Cortex-M33

The other two ARM boards work the same way with their own harness
directory, target and link addresses:

```sh
# Cortex-M4F with its FPU, hard-float calling convention
T=--target=thumbv7em-none-eabihf
H=tests/harness/thumb-m4f
./embcc $T -c $H/boot.c -o out/m4f/boot.o
./embcc $T -c $H/io.c   -o out/m4f/io.o
./embcc $T -O2 -c hello.c -o out/m4f/hello.o
./embld -e reset -Ttext 0x0 -Tdata 0x20000000 \
    out/m4f/boot.o out/m4f/io.o out/m4f/hello.o -o out/m4f/hello.elf
sh $H/run.sh out/m4f/hello.elf

# Cortex-M33 (ARMv8-M Mainline)
T=--target=thumbv8m.main-none-eabi
H=tests/harness/thumb-m33
./embcc $T -c $H/boot.c -o out/m33/boot.o
./embcc $T -c $H/io.c   -o out/m33/io.o
./embcc $T -O2 -c hello.c -o out/m33/hello.o
./embld -e reset -Ttext 0x10000000 -Tdata 0x10100000 \
    out/m33/boot.o out/m33/io.o out/m33/hello.o -o out/m33/hello.elf
EMBCC_QEMU_UNTIL=DONE sh $H/run.sh out/m33/hello.elf
```

(Create the `out/m4f` and `out/m33` directories first.) These two
start-up files loop forever when `main` returns, so QEMU does not stop by
itself. The Cortex-M4F run ends at the timeout (`EMBCC_QEMU_TIMEOUT`,
default 10 seconds). The Cortex-M33 `run.sh` also accepts
`EMBCC_QEMU_UNTIL=TEXT`, which ends the run as soon as the program prints
`TEXT`.

The ARM processor and floating-point options (`-mcpu=`, `-mfpu=`,
`-mfloat-abi=`) are described in
[Invoking EmbCC](invoking.md#arm-options).

### RISC-V

```sh
T=--target=riscv32-unknown-elf
H=tests/harness/riscv
mkdir -p out/rv32
./embcc $T -c $H/boot.c -o out/rv32/boot.o
./embcc $T -c $H/io.c   -o out/rv32/io.o
./embcc $T -O2 -c hello.c -o out/rv32/hello.o
./embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 \
    out/rv32/boot.o out/rv32/io.o out/rv32/hello.o -o out/rv32/hello.elf
sh $H/run.sh out/rv32/hello.elf 32
```

For RV64 use `--target=riscv64-unknown-elf` and pass `64` (the default)
as the second argument of `run.sh`, which selects `qemu-system-riscv64`.
The `virt` board has a test device that the start-up code writes to after
`main` returns, so QEMU exits by itself.

### AVR (ATmega328P)

The AVR start-up code is assembly, assembled by EmbCC itself. The
ATmega328P has no divide instruction, so the program also needs the
compiler runtime's 32-bit routines (`lib/rt/avr.c`; `lib/rt/avr64.c` adds
the 64-bit ones):

```sh
T=--target=avr
H=tests/harness/avr
mkdir -p out/avr
./embcc $T -c $H/boot.S -o out/avr/boot.o
./embcc $T -Os -c $H/io.c -o out/avr/io.o
./embcc $T -Os -c lib/rt/avr.c -o out/avr/rt.o
./embcc $T -Os -c hello.c -o out/avr/hello.o
./embld -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 \
    out/avr/boot.o out/avr/io.o out/avr/rt.o out/avr/hello.o -o out/avr/hello.elf
EMBCC_QEMU_UNTIL=DONE sh $H/run.sh out/avr/hello.elf
```

The entry point must be address 0, where the interrupt vector table is;
QEMU refuses an AVR image whose entry point is anything else.
`--rom-limit 32768` makes `embld` refuse an image larger than the part's
32 KiB of flash. On this target `.rodata` is linked into the writable
segment with `.data`: an ordinary pointer dereference reads data memory,
not flash, so the start-up code copies both from flash to RAM before
`main` runs.

### The compiler runtime

Operations a processor has no instruction for, such as floating point on
a Cortex-M3 or 64-bit division on a 32-bit core, are compiled as calls to
routines with libgcc's names (`__muldf3`, `__divdi3`, ...). EmbCC's
versions are in `lib/rt`. `make rt-embedded` builds one archive per
embedded target; name it after your objects on the `embld` command line:

```sh
make rt-embedded
./embld -e reset -Ttext 0x0 -Tdata 0x20000000 \
    out/m3/boot.o out/m3/io.o out/m3/prog.o \
    build/libc/thumbv7m-none-eabi/librt.a -o out/m3/prog.elf
```

Without it, a program that uses such an operation fails to link, and
`embld` explains the missing symbol:

```text
embld: undefined symbol '__muldf3' (referenced by out/m3/prog.o)
  note: this is a compiler-runtime helper (libgcc's __muldi3 family) ...
```

Because the runtime is an archive, only the routines a program calls are
linked in. For the AVR example above, `rt.o` could be replaced by
`build/libc/avr/librt.a`.

### The C library on a board

`make libc-embedded` builds EmbCC's C library for the embedded targets,
on a backend for a part with no operating system
(`lib/libc/os/baremetal`). It needs nothing from your program to link.
To see `printf`'s output, define `write`; the library calls it for
standard output and standard error:

```c
extern void uart_putc(int c);          /* your board's */

long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    for (unsigned long i = 0; i < n; i++)
        uart_putc(p[i]);
    return (long)n;
}
```

Without it, output is discarded and input is at end of file. `malloc`
takes memory from the end of the image (`embld`'s `_end`) up to the
stack, and returns `NULL` rather than reaching it. `exit` stops in a
loop. There is one thread, no clock and no filesystem: those calls fail
with `ENOSYS`. A board that has any of them defines the classic function
(`read`, `sbrk`, `_exit`, `open`, `close`, `lseek`, `isatty`) and the
library uses it.

Name `libc.a` before `librt.a`, after your objects:

```sh
make libc-embedded rt-embedded
./embld -e reset -Ttext 0x0 -Tdata 0x20000000 \
    out/m3/boot.o out/m3/prog.o \
    build/libc/thumbv7m-none-eabi/libc.a \
    build/libc/thumbv7m-none-eabi/librt.a -o out/m3/prog.elf
```

It is not built for `avr`, where the library's two-byte locks are not
one access.

### x86-64 and AArch64 under QEMU

The x86-64 and AArch64 harnesses run ordinary hosted-style C programs
(with `<stdio.h>`) on bare metal, against a newlib C library, with the
cross GNU toolchain doing the link. They need the tools listed under
[Host requirements](#host-requirements).

```sh
mkdir -p out/x64 out/a64

# x86-64: a Multiboot image booted by qemu-system-x86_64
./embcc --target=x86_64-elf -O2 -c hello.c -o out/x64/hello.o
sh tests/harness/x86_64/link.sh -o out/x64/hello.elf out/x64/hello.o
sh tests/harness/x86_64/run.sh out/x64/hello.elf

# AArch64: QEMU's virt board, output through ARM semihosting
./embcc --target=aarch64-elf -O2 -c hello.c -o out/a64/hello.o
sh tests/harness/aarch64/link.sh -o out/a64/hello.elf out/a64/hello.o
sh tests/harness/aarch64/run.sh out/a64/hello.elf
```

`run.sh` exits with the program's exit status. The newlib is looked for
in `~/cross/newlib-c99/x86_64-elf` and
`~/cross/newlib-aarch64-c99/aarch64-elf`; set `EMBCC_X86_NEWLIB` or
`EMBCC_AARCH64_NEWLIB` to its `TRIPLE` directory if it is elsewhere.

## Where to go next

- [Invoking EmbCC](invoking.md): every command-line option.
- [Targets](targets.md): each target's data model, ABI, predefined macros
  and limitations.
- [Bare-metal programming](embedded.md): start-up code, interrupts,
  memory-mapped I/O and linking firmware for your own board.
- [Libraries](libraries.md): the C library, the C++ runtime and the
  compiler runtime.
- [Diagnostics](diagnostics.md): reading EmbCC's messages, `--explain`
  and `--fix`.
- [Optimization](optimization.md) and [Debugging](debugging.md).
- [embld](tools/embld.md): the linker's options.
