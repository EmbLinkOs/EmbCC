# EmbCC and the Emb toolchain

EmbCC is a C and C++ compiler for embedded systems, and the Emb
toolchain is everything around it: an assembler, a linker, a debugger,
a simulator, a flash programmer, a tracer and the tools that size,
pack and analyse firmware. One `embcc` binary targets every chip below,
from an 8-bit AVR to a 64-bit RISC-V. It preprocesses, compiles,
assembles and links without starting any other program.

EmbCC is also a full compiler for operating systems and hosted
programs: x86-64 and AArch64 kernels (EmbLinkOS among them), Linux,
macOS and Windows objects.

- **No dependencies.** The whole toolchain is ISO C99. A C compiler and
  `make` build it on macOS, on Linux, or on a system with neither GCC
  nor clang. EmbCC compiles its own sources.
- **Checked, not assumed.** Every target's code is run on QEMU against
  the same programs compiled by clang or GCC. Every encoder is compared
  byte for byte with llvm-mc or GNU as. Every calling convention is
  linked against another compiler's objects. Random programs are fuzzed
  on four architectures.
- **It refuses rather than guesses.** A construct EmbCC cannot compile
  correctly is an error that names it, never code that does something
  else.

## Targets

| Family | `--target=` | C | C++ | Assembler | Simulator |
|---|---|---|---|---|---|
| Arm Cortex-M (M0/M0+, M3, M4/M4F, M7, M23, M33) | `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi[hf]`, `thumbv8m.base-none-eabi`, `thumbv8m.main-none-eabi[hf]` | yes | yes¹ | yes, with DSP and TrustZone-M | EmbSim |
| Arm Cortex-A/R (ARMv7-A) | `armv7a-none-eabi[hf]` | yes | yes¹ | yes | |
| RISC-V (RV32/RV64 IMAC, F and D) | `riscv32-unknown-elf`, `riscv64-unknown-elf` | yes | yes¹ | yes | |
| AVR (ATmega) | `avr` | yes | | yes | |
| Xtensa (ESP32 class, windowed ABI) | `xtensa-none-elf` | yes | yes¹ | yes | |
| Infineon TriCore (AURIX) | `tricore-none-elf` | yes | yes¹ | yes | |
| Renesas RX | `rx-none-elf` | yes | yes¹ | yes | |
| NXP ColdFire | `m68k-none-elf` | yes | yes¹ | yes | |
| MIPS32 and MIPS64, both byte orders | `mipsel-none-elf`, `mips-none-elf`, `mips64el-none-elf`, `mips64-none-elf` | yes | yes¹ | yes | |
| PowerPC (e500, 32-bit) | `powerpc-none-eabi` | yes | yes¹ | yes | |
| SPARC V8 (LEON3) | `sparc-none-elf` | yes | yes¹ | yes | |
| LoongArch64 | `loongarch64-none-elf` | yes | yes¹ | yes | |
| x86-64 and AArch64: bare metal, EmbLinkOS | `x86_64-elf`, `aarch64-elf`, `x86_64-emblink`, `aarch64-emblink` | yes | yes | yes (x86-64: NASM syntax) | |
| Linux, macOS, Windows | `x86_64-linux-gnu`, `aarch64-linux-gnu`, `x86_64-apple-darwin`, `aarch64-apple-darwin`, `x86_64-windows-gnu` | yes | yes | | |

¹ With `-fno-exceptions`: there are no unwind tables for these targets
yet, so exceptions are refused by name.

An unknown `--target=` lists every triple EmbCC accepts. Each target's
ABI, options and limitations are in [Targets](docs/manual/targets.md).

## The tools

| Tool | What it does |
|---|---|
| `embcc` | The compiler: C (C89 to C23 with GNU extensions) and C++, plus GCC-compatible command lines |
| `embas` | The assembler, for GNU-syntax `.s` and `.S` files on every embedded target |
| `embld` | The linker: GNU linker scripts, `--gc-sections`, map files, long-branch and TrustZone veneers |
| `embdbg` | The debugger: symbols, source lines, fault dumps, and live debugging through the GDB remote protocol |
| `embsim` | The simulator: runs Cortex-M firmware without a board, with deterministic time and cycle estimates |
| `embflash` | Programs a target's flash through a GDB server (OpenOCD, pyOCD, J-Link, Black Magic Probe, QEMU) |
| `embtrace` | Function-level tracing on the target: call counts, times, a call tree, Perfetto traces |
| `embmap` | Where the bytes went: sections, symbols and regions of a firmware image |
| `embpack` | Turns a linked image into what a programmer or bootloader takes: raw binary, Intel HEX, S-records or UF2, with a CRC |
| `embrt` | The worst-case stack depth of a firmware, proved from its call graph |
| `embsvd` | From a vendor's SVD file: the device header, a startup file and a linker script, or the registers as JSON |
| `embread`, `embls`, `embidx`, `embar` | ELF reader, language server, cross-unit interface index, archiver |

## Building and installing

```sh
make all       # the compiler and every tool but the debugger
make embdbg    # the debugger
make check     # compile and run every program in tests/exec and tests/cxx
make install   # into PREFIX (default /usr/local)
```

`make` alone builds a single object; name a target. The runtime and C
library for each board, and the test suites, are described in
[Getting started](docs/manual/getting-started.md).

## A first firmware, without a board

The program below runs on a Cortex-M3. Its startup code and output
functions come from the test harness:

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

```sh
T=--target=thumbv7m-none-eabi
./embcc $T -c tests/harness/thumb/boot.c -o boot.o
./embcc $T -c tests/harness/thumb/io.c   -o io.o
./embcc $T -O2 -c hello.c                -o hello.o
./embld -e reset -Ttext 0x0 -Tdata 0x20000000 boot.o io.o hello.o -o hello.elf
./embsim hello.elf --stats
```

```text
hello from EmbCC
1 4 9 16 25 
DONE
embsim: lockup: a fault with HardFault already active at 0x00000000; 933 instructions, 1665 cycles (est.)
```

When `main` returns, the harness's startup code traps, and EmbSim stops
there. The same image runs on QEMU's `lm3s6965evb`, and on a board once
the link layout matches the part. Other boards and families, with a
startup file, a linker script and a C library, are in
[Getting started](docs/manual/getting-started.md).

## Operating systems and hosted programs

On Linux x86-64, `make libc-linux-x86_64` builds the C library, and
`./embcc --target=x86_64-linux-gnu -O2 hello.c -o hello` compiles and
links a static executable. On macOS, EmbCC writes Mach-O objects for the
system linker:

```sh
./embcc --target=aarch64-apple-darwin -O2 -c hello.c -o hello.o
cc hello.o -o hello
```

`x86_64-elf` and `aarch64-elf` build freestanding kernels; `embld
--embx` writes EmbLinkOS images.

## Documentation

- [The manual](docs/README.md): the language, the targets, every option
  and every tool.
- [Getting started](docs/manual/getting-started.md): building,
  installing, and first programs on each family of boards.
- [Contributing](docs/internals/contributing.md): the rules for changing
  the toolchain, and the tests a change needs.
