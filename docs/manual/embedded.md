# Bare-metal and freestanding programming

This chapter is for people who build firmware or kernels with EmbCC: code
that runs with no operating system underneath it. It covers, per board
family, what an image needs before `main` runs (vector table, reset
handler, stack pointer, `.data` and `.bss`), how interrupt and trap
handlers are written, how an image is linked for a memory map with
[EmbLD](tools/embld.md), which runtime routines the compiler expects to
find, how to size the stack, and how to run an image under QEMU the way
EmbCC's own test harnesses do. Target triples, data models and `-m`
options are listed in [Targets](targets.md); this chapter refers to them
rather than repeating them.

## Overview

| Board family | Triples | Startup written in | Interrupt handlers | Runtime archive |
|---|---|---|---|---|
| ARM Cortex-M (ARMv6-M, ARMv7-M, ARMv7E-M, ARMv8-M Mainline) | `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` | C | `__attribute__((interrupt))` or a plain function | `librt.a` per triple |
| RISC-V | `riscv32-unknown-elf`, `riscv64-unknown-elf` | C, with a stack stub from EmbLD | assembly entry, C body | `librt.a` for RV32 only |
| MIPS32 (MIPS32r2, little-endian, a PIC32's core) | `mipsel-none-elf` | C, with a stack stub from EmbLD | assembly entry (`.S` or naked), C body | `librt.a` and `libc.a` |
| AVR (ATmega328P) | `avr` | assembly | `__attribute__((signal))`, `__attribute__((interrupt))` | `librt.a` |
| x86-64 kernel | `x86_64-elf`, `x86_64-emblink` | assembly | assembly entry, C body | none built; libgcc's names (see [below](#x86-64-kernels-and-emblinkos)) |

The aliases each triple accepts are listed in [Targets](targets.md).

### Compile, then link

A firmware build compiles each file with `-c` and links the objects, as
a Makefile written for `arm-none-eabi-gcc` does. The driver links them,
adding the target's `librt.a` (and `libc.a`, if built) itself; the memory
map is a linker script (`-T`) or `-Wl,-Ttext`/`-Wl,-Tdata`:

```sh
embcc --target=thumbv7m-none-eabi -Os -c startup.c -o startup.o
embcc --target=thumbv7m-none-eabi -Os -c main.c    -o main.o
embcc --target=thumbv7m-none-eabi -T board.ld startup.o main.o -o fw.elf
```

or, naming every input to `embld` explicitly:

```sh
embld -e Reset_Handler -Ttext 0x0 -Tdata 0x20000000 --rom-limit 262144 \
      startup.o main.o build/libc/thumbv7m-none-eabi/librt.a -o fw.elf
```

Without a memory map the driver refuses the link
(`embcc: error: linking a thumbv7m-none-eabi image needs its memory
map`).

`embld` reads ARM, RV32, RV64, MIPS32, AVR and x86-64 objects. It does not read
AArch64 objects; a bare-metal AArch64 image is linked with another
toolchain's linker (see [Running images under QEMU](#running-images-under-qemu)).

`-nostdlib`, `-nodefaultlibs` and `-nostartfiles` mean what they mean to
gcc, and `-lc`, `-lm` and `-lgcc` name EmbCC's own `libc.a` and
`librt.a`. `-Wl,` options are ignored by a compile with `-c`, so a build
system that passes them to every command still compiles. See [Libraries](libraries.md#how-the-driver-links-the-libraries).

## Freestanding compilation

### `-ffreestanding` and `-fno-builtin`

Both are accepted and change nothing. EmbCC makes no hosted assumptions to
drop: it recognises no library function by name, only the `__builtin_`
spellings, and it never turns a loop into a call to `memset` or `memcpy`.

`__STDC_HOSTED__` is defined as `1` on every target, with or without
`-ffreestanding`. Code that tests it to detect a freestanding build gets
the hosted answer.

### Headers

The headers a freestanding program may use are found without any `-I`:

| Header | Supplied by |
|---|---|
| `<stddef.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stdint.h>`, `<limits.h>`, `<float.h>` | EmbCC's freestanding directory (`include/` in a build tree, `freestanding/` in an installation) |
| `<iso646.h>`, `<stdalign.h>`, `<stdnoreturn.h>`, `<stdatomic.h>` | the C library's header directory (`lib/libc/include`), which needs no library code |
| `<string.h>` | a declarations-only version in the freestanding directory, and the C library's version; see [Search order](invoking.md#search-order) for which one a given layout finds |

`<stdint.h>` and `<limits.h>` derive every width from the target's
predefined macros, so `int32_t` is `long` on AVR (where `int` is 16 bits)
and `int64_t` is `long long` on every 32-bit and 16-bit target.

The other C library headers (`<stdio.h>`, `<stdlib.h>`, `<math.h>` and the
rest) also resolve, but no C library is built for the embedded targets:
a firmware image that calls `printf` or `malloc` has to supply them.
`-nostdinc` removes EmbCC's directories from the search; see
[Invoking](invoking.md#-nostdinc).

### What the compiler emits calls to

Code compiled for an embedded target refers to symbols outside the
program in exactly these cases:

- **Runtime helpers** for operations the machine has no instruction for.
  These are libgcc's names and are listed per target below; they are in
  the target's `librt.a`.
- **`memcpy`, `memmove`, `memset`, `memcmp`** and the other string
  functions, only where the program calls them, by name or as
  `__builtin_memcpy` and so on. Struct assignment, struct arguments and
  return values, and zero or partial initialisation of arrays are always
  expanded inline; EmbCC never introduces a call to `memcpy` or `memset`
  that the source does not contain.
- **C++ runtime** routines, from C++ code (see [C++](#c-on-the-embedded-targets)).

It emits no stack-protector calls (`-fstack-protector` is refused), no
`__aeabi_*` calls, no `__atomic_*` or `__sync_*` library calls (atomic
operations are inline or refused; see each board's section), no
profiling or sanitizer runtime calls (`-fsanitize=` checks trap in place),
and no `abort`: `__builtin_trap()` is `udf #0` on ARM, `unimp` on
RISC-V and `break` on MIPS32, and on AVR a jump to itself.

Which operations become helper calls depends on the target:

| Operation | Cortex-M soft-float | Cortex-M `-eabihf` | RV32 | RV64 | MIPS32 | AVR |
|---|---|---|---|---|---|---|
| 8/16/32-bit multiply | inline | inline | inline (M) | inline (M) | inline (`mul`) | `__mulsi3` |
| 8/16/32-bit divide, remainder | inline (`sdiv`/`udiv`) | inline | inline (M) | inline (M) | inline (`div`/`divu`) | `__divsi3` `__udivsi3` `__modsi3` `__umodsi3` |
| 64-bit add, subtract, shift, compare, multiply | inline | inline | inline | inline | inline | inline, except multiply: `__muldi3` |
| 64-bit divide, remainder | `__divdi3` `__udivdi3` `__moddi3` `__umoddi3` | same | same | inline | same | same |
| `float` arithmetic, comparison, conversion | `__addsf3` `__mulsf3` `__ltsf2` `__fixsfsi` ... | inline (VFP), except conversions to and from 64-bit integers (`__fixsfdi`, `__floatdisf`) | `__addsf3` ... | `__addsf3` ... | `__addsf3` ... | `__addsf3` ... |
| `double` arithmetic, comparison, conversion | `__adddf3` `__muldf3` `__ltdf2` `__floatsidf` `__extendsfdf2` ... | same (the FPU is single-precision) | same | same | same | not applicable: `double` is `float` |
| `_Complex` multiply, divide | `__mulsc3` `__muldc3` `__divsc3` `__divdc3` | same | same | same | same | same |

The complete list of what each `librt.a` defines is in
[Libraries](libraries.md#the-compiler-runtime-librt). The names are
libgcc's, so an object compiled by EmbCC links against another
toolchain's libgcc, and objects from GCC or Clang that call the libgcc
names link against EmbCC's `librt.a`. EmbCC does not provide the ARM
run-time ABI names (`__aeabi_ldivmod`, `__aeabi_fadd`, ...); an object
from another ARM compiler that calls them must bring its own.

### The runtime archive

`make rt-embedded` builds one `librt.a` per embedded triple with
`tools/build-rt.sh`, at `-Os`, into `build/libc/TRIPLE/librt.a`:
`avr`, `thumbv6m-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi`,
`thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`,
`thumbv8m.main-none-eabihf`, `riscv32-unknown-elf` and
`mipsel-none-elf`. `make install`
copies each to `PREFIX/lib/embcc/VERSION/TRIPLE/librt.a`. To build one by
hand:

```sh
sh tools/build-rt.sh thumbv7em-none-eabihf out/    # writes out/librt.a
```

The hard-float triples have archives of their own because their objects
do not link with soft-float ones (see [The float ABI](#the-float-abi)).

There is no archive for `riscv64-unknown-elf`: two of the runtime files
use 128-bit integers, which the RV64 backend does not lower. An RV64
image compiles the files it needs itself:

```sh
embcc --target=riscv64-unknown-elf -Os -c lib/rt/softfp.c  -o softfp.o
embcc --target=riscv64-unknown-elf -Os -c lib/rt/complex.c -o complex.o
```

Put the archive after the objects that use it. A helper missing from the
link is reported with what it is:

```text
embld: undefined symbol '__divdi3' (referenced by main.o)
  note: this is a compiler-runtime helper (libgcc's __muldi3 family) — the routine a backend calls for an operation the machine has no instruction for, such as 128-bit multiply or divide. EmbCC ships these in librt.a (lib/rt); the driver puts it on the link line by itself, and a hand-written link has to name it after libc.a
```

Each runtime source file is one archive member, so an image pays for a
file when it uses any routine in it. On Cortex-M and RISC-V all of
binary32 and binary64 is one member (`softfp.o`, about 8 KB). On AVR the float routines are split into one member per group
(add, multiply, divide, compare, conversions), because the whole set does
not fit in the part's flash.

### Startup code and the linker's symbols

A C startup on every embedded target does the same four things before
`main`: set the stack pointer, copy `.data` from flash to RAM, zero
`.bss`, and run static constructors if the program has any. EmbLD
provides the symbols for the middle two when `-Tdata` is given:

| Symbol | Value |
|---|---|
| `__data_load` | the flash address the initial values of `.data` are stored at |
| `__data_start`, `__data_end` | the RAM range `.data` is addressed at |
| `__bss_start`, `__bss_end` | the RAM range to zero |
| `__init_array_start`, `__init_array_end` | the `.init_array` table (`__attribute__((constructor))`, C++ globals) |

These are defined only when something references them, and a definition
in the program wins. They are all a C runtime needs:

```c
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

static void crt_init(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
}
```

The EmbLD options that shape a firmware image:

| Option | Effect |
|---|---|
| `-Ttext ADDR` | where the image starts: the flash base. `-Ttext 0x0` is honoured. Default `0x400000`. |
| `-Tdata ADDR` | address the writable data at `ADDR` (RAM) while storing its initial values directly after the text (flash), and define the bracket symbols above. Without it, data follows the text and is addressed where it is stored. |
| `--rom-limit N`, `--rom-limit=N` | refuse an image whose stored bytes (text plus initial data) exceed `N` |
| `-Tstack ADDR` | RISC-V only: emit an entry stub that sets `sp` to `ADDR` and jumps to the entry symbol |
| `-e SYM` | the entry symbol (default `_start`); on ARM the ELF entry carries the Thumb bit |

An input section named `.vectors` or `.isr_vector` is placed first in the
image, ahead of `.text`. Other sections are grouped by prefix (`.text.*`
with `.text`, `.data.*` with `.data`); a section with any other name is
kept as a group of its own, after `.rodata` if it is read-only and after
`.data` if it is writable, and is bracketed by `__start_NAME`/`__stop_NAME`
(for a C-identifier name) or `__NAME_start`/`__NAME_end` (for `.NAME`).
There is no garbage collection of unused sections.

A project that comes with a GNU ld linker script links with it instead:
`embld -T SCRIPT` replaces all of the options above, and the startup uses
whatever symbols the script defines (`_sidata`, `_sdata`, `_ebss` in
STM32CubeMX's, for example). See [Linker scripts](tools/embld.md#linker-scripts).
A section of your own that holds only `const` objects (a command table, a
list of drivers) is read-only, as gcc makes it, so a script's orphan rule
stores it in flash after `.rodata`.

An image too large for `--rom-limit` is refused:

```text
embld: the image needs 9072 bytes of flash and the part has 4096 (--rom-limit): 9055 of text, 16 of initial data
```

The output is an ELF executable. Its second `PT_LOAD` has the RAM address
as its virtual address and the flash address as its physical address, so
`llvm-objcopy -O binary fw.elf fw.bin` produces the flash image with
`.data`'s initial values in place, and `-O ihex` an Intel HEX file. EmbCC
ships no `objcopy`. The symbol table is kept in the image, outside every
loadable segment, so it costs no flash. All EmbLD options are in
[EmbLD](tools/embld.md).

### `volatile` and memory-mapped registers

An access through a `volatile` lvalue is performed every time the source
makes it, at every optimization level, so a peripheral register can be
declared the usual way:

```c
struct uart {
    volatile unsigned data;
    volatile unsigned status : 8;
    volatile unsigned mode : 3;
};
#define UART0 ((struct uart *)0x4000C000u)

void put(char c)
{
    while (UART0->status & 0x20)    /* one read of the register per test */
        ;
    UART0->data = c;                /* one store */
}
```

- A read or write through a pointer to `volatile` is never removed,
  merged with another, moved out of a loop or replaced by a value already
  known. `(void)UART0->data;` performs the read.
- A `volatile` bit-field is accessed through its storage unit. Reading
  the field loads the unit once; assigning to it loads the unit once and
  stores it once. The value of an assignment such as `x = (r->mode = 5)`
  is computed from the stored value, without reading the register again,
  which matters for a register that clears when read.
- A `volatile` local variable stays in memory, so a delay loop such as
  `for (volatile int i = 0; i < 1000; i++) ;` performs every read and
  write of `i`.

See [Optimization](optimization.md#volatile).

### Sizing the stack: `-fstack-usage`

A microcontroller has no guard page and no room to grow a stack, so the
deepest call path has to be added up before the image runs.
`-fstack-usage` writes one line per emitted function to a `.su` file
beside the object (the output name with its extension replaced), in
GCC's format: `FILE:LINE:FUNCTION`, a tab, the frame size in bytes, a
tab, `static`.

```sh
embcc --target=thumbv7m-none-eabi -Os -fstack-usage -c su.c -o su.o
```

```text
su.c:1:leaf	0	static
su.c:2:buf_user	264	static
su.c:3:caller	8	static
```

The number is that function's own frame and nothing it calls:

| Target | What the number includes |
|---|---|
| Cortex-M | the registers the prologue pushes, plus the frame; 0 for a function that pushes nothing |
| RISC-V | the frame the prologue allocates, saved registers included |
| AVR | the frame, the pushed call-saved registers, the frame pointer, and the 2-byte return address |
| x86-64 | the return address, the saved `rbp` where there is one, and the frame |

Functions the inliner absorbed or that were never emitted are left out
rather than reported as 0. Combine the numbers along the call graph to
get a worst case; `embcc inspect callgraph FILE.c` prints the graph the
optimizer sees at the same `-O` level. Add the hardware's own exception
frame for each level of interrupt nesting (on Cortex-M, 32 bytes per
level, more with an active FPU context).

### Leaving out what is never called: `--gc-sections`

Flash is the limit a firmware build meets first. Compile with
`-ffunction-sections -fdata-sections`, so each function and object is a
section of its own, and link with `--gc-sections`: the linker keeps what
the program can reach and drops the rest -- a driver's functions the
program never calls, a table nothing reads.

```sh
CF="--target=thumbv7em-none-eabi -O2 -ffunction-sections -fdata-sections"
embcc $CF -c main.c -o main.o
embcc $CF -c drivers.c -o drivers.o
embcc --target=thumbv7em-none-eabi -T board.ld \
      -Wl,--gc-sections,--print-memory-usage,-Map=fw.map \
      startup.o main.o drivers.o -o fw.elf
```

```text
Memory region         Used Size  Region Size  %age Used
             RAM:       1968 B        64 KB      3.00%
           FLASH:        844 B       256 KB      0.32%
```

What is kept is what the entry symbol, `-u` and the script's `KEEP()`
reach, through every relocation of everything kept. Some things are
reached without a reference, and are kept too:

- the constructor and destructor arrays (`.init_array`, `.fini_array`,
  `.preinit_array`, `.ctors`, `.dtors`) and `.init`/`.fini`;
- a section the program walks with `__start_NAME`/`__stop_NAME`;
- without a linker script, the vector table (`.vectors`, `.isr_vector`);
  with one, keep it with `KEEP(*(.isr_vector))`, as every vendor script
  does;
- notes, and a section marked `SHF_GNU_RETAIN`.

An interrupt handler is reached from the vector table, and a function
from a table of pointers, so nothing else is needed for them. A
function that is entered only from outside the image -- by a debugger
script, say -- needs `-Wl,-u,NAME`. `--print-gc-sections` lists what was
dropped; the map file (`-Map`) lists it too, with every section that was
kept, its address, and the symbols in it; `--print-memory-usage` is the
table above, region by region. The details are in
[`embld`](tools/embld.md#garbage-collection).

## ARM Cortex-M

### Choosing the target

| Part | Triple | Notes |
|---|---|---|
| Cortex-M0, M0+, M1 | `thumbv6m-none-eabi`, or `-mcpu=cortex-m0` (`m0plus`, `m1`) on any ARM triple | Thumb-1; divide, 64-bit multiply and atomics are `librt.a` calls |
| Cortex-M3 | `thumbv7m-none-eabi` | no FPU |
| Cortex-M4, M7 without FPU use | `thumbv7em-none-eabi` | soft-float |
| Cortex-M4F | `thumbv7em-none-eabihf`, or `thumbv7em-none-eabi -mfpu=fpv4-sp-d16 -mfloat-abi=hard` | single-precision FPU, hard-float convention |
| Cortex-M7 | `thumbv7em-none-eabihf -mcpu=cortex-m7`, or `thumbv7em-none-eabi -mfpu=fpv5-d16 -mfloat-abi=hard` | double-precision FPU (FPv5-D16), hard-float convention |
| Cortex-M33 | `thumbv8m.main-none-eabi`; `thumbv8m.main-none-eabihf` for the FPU | FPv5-SP-D16 |

`-mcpu=cortex-m0`, `cortex-m0plus`, `cortex-m1`, `cortex-m3`,
`cortex-m4`, `cortex-m7` and `cortex-m33` select the sub-architecture as
GCC's options do. `-mcpu=cortex-m23` is refused: an ARMv8-M Baseline part
is a different subset, and EmbCC emits neither ARMv6-M nor ARMv7-M code
it can run. `-mthumb` is
accepted and has no effect; `-marm` is
refused (`-marm is not supported: a Cortex-M has no ARM instruction set,
only Thumb`). Plain `char` is unsigned, `long double` is 8 bytes, and an
`enum` is `int`-sized unless its values need a wider type
(`-fshort-enums` is refused). The rest of the
data model is in [Targets](targets.md).

### Vector table and reset handler

A Cortex-M reads its initial stack pointer from the first word of the
vector table and the reset handler's address from the second, so the
whole startup is C: the table is an array placed in `.vectors`, and the
reset handler is an ordinary function.

```c
/* startup.c -- for a part with 64 KiB of SRAM at 0x20000000 */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;

int main(void);
void Reset_Handler(void);
void SysTick_Handler(void);

#define SRAM_TOP 0x20010000u

__attribute__((section(".vectors"), used))
void *const vectors[16] = {
    (void *)SRAM_TOP,          /* 0: initial stack pointer */
    (void *)Reset_Handler,     /* 1: reset */
    [15] = (void *)SysTick_Handler,
};

void Reset_Handler(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    main();
    for (;;)
        ;
}
```

`used` keeps the table even though nothing refers to it. The linker sets
the Thumb bit in every function address it writes into the table. CMSIS's
section name `.isr_vector` is placed the same way as `.vectors`.

A function that must run with no prologue -- a context switch, or a
reset handler that sets up the stack itself -- is written as
`__attribute__((naked))`, whose body is its asm (see
[Naked functions](inline-asm.md#naked-functions)); on a Cortex-M a reset
handler needs none, since the processor loads the stack pointer from the
table. FreeRTOS's Cortex-M3 port, whose PendSV and SVC handlers are
naked functions, builds unmodified and runs
(`tests/golden/freertos-cm3.sh`).

### Interrupt handlers

The processor saves `r0`-`r3`, `r12`, `lr`, `pc` and `xPSR` on exception
entry and puts an `EXC_RETURN` value in `lr`, so an ordinary function is a
correct handler: its prologue saves the rest and its `bx lr` performs the
exception return. `__attribute__((interrupt))` is accepted on the
Cortex-M targets, as CMSIS headers write it, and changes nothing in the
generated code.

```c
volatile unsigned ticks;

__attribute__((interrupt)) void SysTick_Handler(void) { ticks++; }
```

Put the handler's address in its slot of the vector table. On a part with
an FPU, the exception entry also preserves `s0`-`s15` and `FPSCR` when the
FPU's automatic state preservation is enabled, which it is at reset.

`__attribute__((signal))` is refused on ARM: it is AVR's spelling.
Inline assembly covers the special registers a handler or a critical
section needs (`mrs`/`msr` on `PRIMASK`, `BASEPRI` and the others,
`cpsid`/`cpsie`, `dsb`, `dmb`, `isb`, `wfi`, `wfe`, `sev`, `bkpt`); see
[Inline assembly](inline-asm.md).

### Atomics

32-bit and narrower atomic operations are inline: `ldrex`/`strex` loops
with `dmb` barriers. 64-bit atomic read-modify-write operations are
refused:

```text
embcc: atom.c:6: error: the ARMv7-M backend cannot lower this operation at 64 bits yet (function bump64) [xadd w=8 size=8]
```

### Enabling the FPU

The FPU is off at reset. Grant full access to coprocessors CP10 and CP11
in `CPACR` before the first floating-point instruction runs, and follow
the write with `dsb` and `isb`:

```c
#define CPACR (*(volatile unsigned *)0xE000ED88u)

void Reset_Handler(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    CPACR |= (3u << 20) | (3u << 22);     /* CP10 and CP11: full access */
    __asm__ volatile("dsb");
    __asm__ volatile("isb");
    /* ... copy .data, zero .bss, call main ... */
}
```

Do this first: the optimizer may use a VFP register for a value anywhere
after it, including inside the copy loops. Without it the first VFP
instruction takes a UsageFault.

### The float ABI

`-mfpu=` and `-mfloat-abi=` are read together after the whole command
line, in either order:

| Option | Meaning |
|---|---|
| `-mfloat-abi=soft` | the default: no FPU instructions, even with an `-mfpu=` |
| `-mfloat-abi=softfp` | FPU instructions; floating-point arguments and results in core registers, so the objects link with soft-float ones |
| `-mfloat-abi=hard` | FPU instructions; floating-point arguments and results in `s0`-`s15`/`d0`-`d7` (AAPCS-VFP) |
| `-mfpu=fpv4-sp-d16` | the Cortex-M4F unit; needs an ARMv7E-M target or `-mcpu=cortex-m4` |
| `-mfpu=fpv5-d16` | the Cortex-M7 unit, with double precision; needs an ARMv7E-M target, and `-mcpu=` other than `cortex-m7` is refused |
| `-mfpu=fpv5-sp-d16` | the Cortex-M33 unit; needs `thumbv8m.main` |
| `-mfpu=none`, `-mfpu=auto`, `-mfpu=soft` | no unit named |

The `-eabihf` triples mean the part's unit with `-mfloat-abi=hard`: the
Cortex-M4F's for `thumbv7em-none-eabihf`, or the Cortex-M7's when
`-mcpu=cortex-m7` is given too. An explicit `-mfloat-abi=` overrides
that, and `-dumpmachine` reports the result
(`thumbv7em-none-eabihf -mfloat-abi=soft` prints `thumbv7em-none-eabi`).

The Cortex-M4F and M33 units are single-precision: `float` arithmetic
becomes VFP instructions and `double` arithmetic stays a call into
`librt.a`. The Cortex-M7's computes both. With `-mfpu=fpv5-d16`,
`double` addition, subtraction, multiplication, division, negation,
`fabs`, `__builtin_sqrt`, comparisons, and conversions to and from
`float` and 32-bit integers are VFP `.f64` instructions, and a double
that needs a register lives in `d8`-`d15`, which a function that uses
them saves with `vpush`. Only the conversions between `double` or
`float` and a 64-bit integer remain calls (`__fixdfdi`, `__floatdidf`
and their kin); `lib/rt/softfp.c` built for that unit keeps those eight
routines and nothing else. The runtime helpers use the base
(core-register) convention under every float ABI.

The FPU's predefined macros follow: `__ARM_FP` is `0x4` for the
single-precision units and `0xc` for FPv5-D16, which also defines
`__ARM_FPV5__`. (clang says `0x6` and `0xe`; EmbCC leaves out bit 1,
hardware half-precision conversion, which it does not emit.)

Invalid combinations are refused:

```text
embcc: <embcc>: error: -mfloat-abi=hard needs an FPU to use: add -mfpu=fpv4-sp-d16 (Cortex-M4F), -mfpu=fpv5-d16 (Cortex-M7) or -mfpu=fpv5-sp-d16 (Cortex-M33)
embcc: <embcc>: error: -mfpu=fpv4-sp-d16 is an ARMv7E-M unit, and the part is ARMv7-M (a Cortex-M3 has no FPU); add -mcpu=cortex-m4
embcc: <embcc>: error: -mfpu=fpv5-d16 is the Cortex-M7's double-precision unit, and -mcpu=cortex-m4 does not have it; the Cortex-M4F's is -mfpu=fpv4-sp-d16
embcc: <embcc>: error: -mfloat-abi=bogus is not an ARM float ABI: it is one of soft, softfp and hard
```

Each object records its FPU (`Tag_FP_arch`: VFPv4-D16 for the M4F's
unit, FPv5-D16 for the M7's and the M33's, with `Tag_ABI_HardFP_use`
saying single precision on the two `-sp-` units), its float ABI and its
enum size in its ARM attributes, and EmbLD refuses to link objects that
disagree about the float ABI or the enum size:

```text
embld: 'startup.o' and 'main-hf.o' disagree about where floating-point arguments go: one passes them in the core registers (-mfloat-abi=soft) and the other in s0-s15 (-mfloat-abi=hard). Linking them would leave every float argument read from a register the caller never wrote
```

That includes the runtime archive: link `-eabihf` objects with the
`-eabihf` `librt.a`. The one built for `thumbv7em-none-eabihf` is the
Cortex-M4F's, and links with Cortex-M7 objects -- the convention is the
same, and the M7 runs every M4F instruction -- but an image for the M7
that converts a 64-bit integer then carries the whole of software
binary64. `lib/rt/softfp.c` compiled with `-mcpu=cortex-m7` is the eight
conversion routines alone.

### Linking for a memory map

Flash is `-Ttext`, RAM is `-Tdata`, and the flash size is `--rom-limit`.
For an STM32F4-class part with 512 KiB of flash at `0x08000000` and SRAM
at `0x20000000`:

```sh
embld -e Reset_Handler -Ttext 0x08000000 -Tdata 0x20000000 --rom-limit 524288 \
      startup.o main.o librt.a -o fw.elf
```

Or with the linker script the part's project template provides, which
also checks RAM: a script's `MEMORY` regions are enforced, and an image
that does not fit stops with `region RAM overflowed by N bytes`.

```sh
embld -T STM32F407VGTx_FLASH.ld startup.o main.o librt.a -o fw.elf
```

The vector table's stack-pointer entry is the top of RAM. Without a
script EmbLD has no RAM-size check, so keep `.data`, `.bss` and the stack
inside the part's SRAM yourself (`llvm-size fw.elf` gives `data` and
`bss`). `-Tstack` is a
RISC-V option and is refused here:

```text
embld: -Tstack is a RISC-V option: every other target here starts with a stack pointer already set (a Cortex-M reads its own from the vector table)
```

### CMSIS and vendor files

A Cortex-M project's vendor files build unmodified: ARM's CMSIS-Core
headers (`core_cm4.h`, `cmsis_gcc.h` and their intrinsics), a vendor's
device header and `system_*.c`, its GNU-syntax startup file, and a
CubeMX-style linker script. CMSIS chooses its compiler support by
`__GNUC__`, so compile the C with `-fgnuc-version=4.2.1`:

```sh
CF="-O2 -ICMSIS/Core/Include -Icmsis-device-f4/Include -DSTM32F405xx -fgnuc-version=4.2.1"
embcc --target=thumbv7em-none-eabi $CF -c system_stm32f4xx.c -o system.o
embcc --target=thumbv7em-none-eabi $CF -c main.c -o main.o
embcc --target=thumbv7em-none-eabi -c startup_stm32f405xx.s -o startup.o
embcc --target=thumbv7em-none-eabi -T STM32F405RGTx_FLASH.ld \
      startup.o system.o main.o -o fw.elf
qemu-system-arm -M netduinoplus2 -nographic -kernel fw.elf
```

`tests/golden/cmsis-stm32f4.sh` builds that from ST's startup and system
files as they ship, with a linker script in CubeMX's shape, and runs it
on QEMU's netduinoplus2 (an STM32F405) with SysTick interrupts, when ARM's
CMSIS_5 and ST's cmsis-device-f4 are cloned under `~/EmbRef`.

### Starting from the vendor's SVD

A vendor that ships no CMSIS device pack still publishes an SVD file for
its part. [`embsvd`](tools/embsvd.md) writes the device header, a startup
file and a linker script from it:

```sh
embsvd STM32F405.svd --header STM32F405.h --startup startup.c \
       --ld STM32F405.ld --flash 0x08000000:1M --ram 0x20000000:128K
```

The header has CMSIS's shape -- `USART1->CR1`, `USART1_IRQn`,
`RCC_APB2ENR_USART1EN_Msk` -- and includes CMSIS-Core for the SVD's
core. `embsvd --show USART1` prints the registers and their fields.
`tests/golden/svd-stm32f405.sh` checks the generated layout against ST's
own header, and runs a firmware built from the generated files.

### Stopping and printing under a debugger or emulator

`bkpt` is in the inline-assembly vocabulary, so ARM semihosting works.
With QEMU's `-semihosting`, operation `0x04` (`SYS_WRITE0`) prints a
string on the host and operation `0x20` (`SYS_EXIT_EXTENDED`) ends the
emulator with an exit status:

```c
static int semihost(int op, const void *arg)
{
    int r;
    __asm__ volatile("mov r0, %1\n\tmov r1, %2\n\tbkpt #0xab\n\tmov %0, r0"
                     : "=r"(r) : "r"(op), "r"(arg) : "r0", "r1", "memory");
    return r;
}

void exit_qemu(int code)
{
    static unsigned block[2];
    block[0] = 0x20026;            /* ADP_Stopped_ApplicationExit */
    block[1] = (unsigned)code;
    semihost(0x20, block);
}
```

On real hardware a `bkpt` with no debugger attached escalates to a
HardFault; remove semihosting calls from production images.

### An RTOS kernel

What a preemptive kernel on Cortex-M asks of its compiler, and where
EmbCC provides it:

| The kernel needs | EmbCC |
|---|---|
| A context switch with no prologue (PendSV) | `__attribute__((naked))` with a basic `asm` body, or file-scope `asm`; see [Naked functions](inline-asm.md#naked-functions) |
| Critical sections on `BASEPRI` or `PRIMASK` | extended `asm` with `msr`/`mrs`; an `always_inline` wrapper inlines, and its operands live in registers |
| Handlers for SysTick, PendSV and SVC | ordinary functions (see [Interrupt handlers](#interrupt-handlers)) in the vector table |
| The vector table at the reset address | a `const` array with `__attribute__((section(".isr_vector"), used))` and a linker script (`-T`) |
| 64-bit division (tick conversions) | the compiler runtime, `librt.a`, which the driver links |
| A stack size for each task | `-fstack-usage` (see [Sizing the stack](#sizing-the-stack--fstack-usage)) |

#### FreeRTOS as the worked example

FreeRTOS's kernel and its GCC Cortex-M ports compile unmodified, and run
on QEMU at `-O0`, `-O1`, `-O2` and `-Os` (tasks, a queue, a mutex, a
software timer and time slicing; on the FPU parts, two tasks doing
floating-point arithmetic through every switch):

| Port | Target | QEMU board | Test |
|---|---|---|---|
| `ARM_CM3` | `thumbv7m-none-eabi` | `lm3s6965evb` | `freertos-cm3.sh` |
| `ARM_CM4F` | `thumbv7em-none-eabihf -mfpu=fpv4-sp-d16` | `mps2-an386` | `freertos-ports.sh` |
| `ARM_CM7/r0p1` | `thumbv7em-none-eabihf -mfpu=fpv4-sp-d16` | `mps2-an500` | `freertos-ports.sh` |
| `ARM_CM33_NTZ/non_secure` | `thumbv8m.main-none-eabihf` | `mps2-an505` | `freertos-ports.sh` |
| `RISC-V` (CLINT timer) | `riscv32-unknown-elf`, `riscv64-unknown-elf` | `virt` | `freertos-ports.sh` |

The files the Cortex-M3 test uses, in `tests/golden/freertos-cm3/`, are a
starting point for a board of your own (`tests/golden/freertos-ports/`
has the same for the MPS2 boards):

- `FreeRTOSConfig.h`, whose interrupt priorities must match the part:
  `configPRIO_BITS` is 3 on the LM3S6965 and 4 on most STM32 parts, and
  `configMAX_SYSCALL_INTERRUPT_PRIORITY` is shifted by `8 - configPRIO_BITS`;
- `startup.c`, whose vector table sends SVC to `vPortSVCHandler`, PendSV
  to `xPortPendSVHandler` and SysTick to `xPortSysTickHandler`;
- `lm3s.ld`, the memory map.

With `K` the FreeRTOS-Kernel directory and the three files above in
`board/`:

```sh
CF="--target=thumbv7m-none-eabi -Os -Iboard -I$K/include -I$K/portable/GCC/ARM_CM3"
for f in board/startup.c main.c $K/tasks.c $K/queue.c $K/list.c $K/timers.c \
         $K/portable/MemMang/heap_4.c $K/portable/GCC/ARM_CM3/port.c; do
    embcc $CF -c "$f" -o "$(basename "$f" .c).o"
done
embcc --target=thumbv7m-none-eabi -T board/lm3s.ld *.o -o fw.elf
qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -kernel fw.elf
```

For a Cortex-M4F, use `--target=thumbv7em-none-eabihf -mfpu=fpv4-sp-d16`
and the `ARM_CM4F` port, whose PendSV handler also saves `s16`-`s31`; the
startup code must enable the FPU (`CPACR`) before `main` when anything
before the scheduler may use it. The ARMv8-M ports name their handlers
as CMSIS does (`SVC_Handler`, `PendSV_Handler`, `SysTick_Handler`), and
want `configENABLE_FPU`, `configENABLE_MPU`, `configENABLE_TRUSTZONE` and
`configRUN_FREERTOS_SECURE_ONLY` set in `FreeRTOSConfig.h`.

The RISC-V port is `port.c` and `portASM.S`, compiled with `-I` naming its
chip-specific directory (`chip_specific_extensions/RISCV_MTIME_CLINT_no_extensions`
for a CLINT timer). Its `FreeRTOSConfig.h` gives the timer's addresses
(`configMTIME_BASE_ADDRESS`, `configMTIMECMP_BASE_ADDRESS`; on QEMU's
`virt` `0x0200BFF8` and `0x02004000`), and `configCPU_CLOCK_HZ` is the
timer's rate, 10 MHz there. The startup points `mtvec` at
`freertos_risc_v_trap_handler` before calling `main` (see
`tests/golden/freertos-ports/riscv-start.S`).

Under QEMU, add `-icount shift=2` when the program measures time:
without it SysTick follows the host's clock, and a loaded host delivers
fewer ticks than the guest's code expects.

#### Writing your own

The port's three hard parts are the same for any kernel:

- **The context switch.** A naked PendSV handler saves `r4`-`r11` (and,
  with an FPU, `s16`-`s31` when `lr` says the task used it) on the
  process stack, stores the stack pointer in the current task's control
  block, calls the scheduler with interrupts masked, and restores the
  next task the same way. Nothing in a naked function may need a frame:
  only basic `asm` is allowed in its body, and EmbCC refuses anything
  else by name.
- **Critical sections.** Raise `BASEPRI` rather than masking everything,
  so that interrupts above the kernel's priority still run:

  ```c
  static inline __attribute__((always_inline)) unsigned raise_basepri(void)
  {
      unsigned old, pri = 5 << (8 - 3);
      __asm__ volatile("mrs %0, basepri\n\tmsr basepri, %1\n\tisb\n\tdsb"
                       : "=r"(old) : "r"(pri) : "memory");
      return old;
  }
  ```

  The `"memory"` clobber is what keeps the compiler from moving loads
  and stores of shared data across the boundary.
- **Shared data.** A flag or counter an interrupt writes is `volatile`;
  a structure the kernel protects with a critical section is not, and
  must not need to be.

## RISC-V

### The target

`riscv32-unknown-elf` and `riscv64-unknown-elf` generate code for the
RV32IMAC and RV64IMAC instruction sets with the soft-float ABIs (`ilp32`
and `lp64`) and the `medany` code model. There is no `-march=` or
`-mabi=` option (`embcc: error: unknown argument '-march=rv32imac'`):
the M, A and C extensions are always used, so EmbCC's output does not run
on a core without them. The predefined macros say so (`__riscv_a`,
`__riscv_c`, `__riscv_compressed`, `__riscv_float_abi_soft`,
`__riscv_cmodel_medany`).

Plain `char` is unsigned. `long double` is 16 bytes (binary128) and has
no arithmetic: any operation on one, like any 128-bit integer on RV64, is
refused with `the RV32 backend cannot lower a 128-bit value yet`
(`RV64` on the other width). The full data model is in
[Targets](targets.md).

### Boot

A RISC-V core starts with every register zero and has no vector table to
load a stack pointer from, and C cannot set `sp`. EmbLD supplies the
missing instructions: `-Tstack ADDR` places a stub at the start of the
image that loads `ADDR` into `sp` and jumps to the `-e` symbol, and makes
the stub the ELF entry point. Everything after that is C, and the core
stays in machine mode.

```c
/* boot.c */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
int main(void);

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    main();
    for (;;)
        ;
}
```

```sh
embcc --target=riscv32-unknown-elf -Os -c boot.c -o boot.o
embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 boot.o main.o \
      build/libc/riscv32-unknown-elf/librt.a -o fw.elf
```

An image loaded wholly into RAM, as on QEMU's `virt` board, needs no
`-Tdata`: the bracket symbols are still defined, `__data_load` equals
`__data_start`, and the copy loop moves nothing. A part that executes
from flash uses `-Ttext FLASH -Tdata RAM` as on Cortex-M.

### Trap handlers

`__attribute__((interrupt))` is refused on RISC-V:

```text
embcc: isr.c:2: error: __attribute__((interrupt)) is not supported: the handler would return with an ordinary return instead of the interrupt return the CPU needs, and without saving the registers (on ARMv7-M it needs neither, and is accepted; on AVR it is implemented)
```

Write the trap entry in assembly: save the caller-saved registers, call a
C function, restore them and return with `mret`. EmbCC assembles `.S`
files for RISC-V itself, and CSRs may be named (`csrr a0, mcause`) or
numbered (`csrr a0, 0x342`), in a `.S` file as in inline assembly.

```asm
/* trap.S -- machine-mode trap entry, RV32 */
	.text
	.globl	trap_entry
	.p2align 2
trap_entry:
	addi	sp, sp, -64
	sw	ra, 0(sp)
	sw	t0, 4(sp)
	sw	t1, 8(sp)
	sw	t2, 12(sp)
	sw	a0, 16(sp)
	sw	a1, 20(sp)
	sw	a2, 24(sp)
	sw	a3, 28(sp)
	sw	a4, 32(sp)
	sw	a5, 36(sp)
	sw	a6, 40(sp)
	sw	a7, 44(sp)
	sw	t3, 48(sp)
	sw	t4, 52(sp)
	sw	t5, 56(sp)
	sw	t6, 60(sp)
	csrr	a0, 0x342		/* mcause */
	csrr	a1, 0x341		/* mepc */
	call	trap_handler
	lw	ra, 0(sp)
	lw	t0, 4(sp)
	lw	t1, 8(sp)
	lw	t2, 12(sp)
	lw	a0, 16(sp)
	lw	a1, 20(sp)
	lw	a2, 24(sp)
	lw	a3, 28(sp)
	lw	a4, 32(sp)
	lw	a5, 36(sp)
	lw	a6, 40(sp)
	lw	a7, 44(sp)
	lw	t3, 48(sp)
	lw	t4, 52(sp)
	lw	t5, 56(sp)
	lw	t6, 60(sp)
	addi	sp, sp, 64
	mret
```

`.p2align 2` asks for 4-byte alignment, which `mtvec`'s direct mode
requires; `.align 2` means the same, since EmbCC's assembler reads
`.align N` as 2^N bytes on every target it assembles files for, as GNU as
does (`.balign N` is a byte count). For RV64, store and load with
`sd`/`ld` in 8-byte slots.

The C side installs the entry in `mtvec` and enables interrupts with
inline assembly:

```c
extern void trap_entry(void);

#define CLINT_MTIMECMP (*(volatile unsigned long long *)0x2004000u)
#define CLINT_MTIME    (*(volatile unsigned long long *)0x200bff8u)

volatile unsigned ticks;

void trap_handler(unsigned long cause, unsigned long epc)
{
    (void)epc;
    if (cause == 0x80000007ul) {               /* machine timer interrupt */
        ticks++;
        CLINT_MTIMECMP = CLINT_MTIME + 10000;
    }
}

void timer_start(void)
{
    __asm__ volatile("csrw mtvec, %0" :: "r"(trap_entry));
    CLINT_MTIMECMP = CLINT_MTIME + 10000;
    __asm__ volatile("csrs mie, %0" :: "r"(1ul << 7));       /* MTIE */
    __asm__ volatile("csrs mstatus, %0" :: "r"(1ul << 3));   /* MIE */
}
```

The CLINT addresses are those of QEMU's `virt` board. For a synchronous
exception, the handler must advance `mepc` past the faulting instruction
(`csrw mepc`) before returning, or the instruction runs again.

### Atomics: the A extension

Atomic operations up to the register width are inline A-extension
instructions: `atomic_fetch_add` on an `int` is one `amoadd.w.aqrl`, a
compare-exchange is an `lr.w.aq`/`sc.w.rl` loop, and
`atomic_thread_fence` is `fence rw, rw`. No library is involved. 64-bit
atomics on RV32 are refused (`the RV32 backend cannot lower this
operation at 64 bits yet`).

### The C extension

Compressed (16-bit) instructions are always emitted, and every object
carries `EF_RISCV_RVC` in its ELF header. EmbLD does not relax: it
ignores `R_RISCV_RELAX` and `R_RISCV_ALIGN` and keeps every sequence at
the length the compiler or assembler chose.

With the C extension, functions are aligned to 2 bytes, and
`__attribute__((aligned(4)))` on a function does not change that. An
address that must be 4-byte aligned, such as a direct-mode `mtvec`
target, belongs in an assembly file with `.p2align 2`, as in the trap
entry above.

### Runtime

`riscv32-unknown-elf` has a `librt.a` with the 64-bit division routines,
software `float` and `double`, and complex multiply and divide. RV64
divides 64-bit integers in hardware and needs only the floating-point
and complex routines, which have to be compiled by hand (see
[The runtime archive](#the-runtime-archive)).

## MIPS32

### The target

`mipsel-none-elf` generates MIPS32 Release 2 code, little-endian, with
the o32 ABI and soft float: the configuration of a PIC32MX or PIC32MZ
part's core used without its FPU. The code is not abicalls code and puts
nothing in small data: addresses are built with `lui`/`addiu`, calls are
`jal`, and `$gp` is never read. Plain `char` is signed, `long double` is
`double`, and a misaligned word access is avoided rather than trapped:
a packed member is read with `lwl`/`lwr`. The options and the full data
model are in [Targets](targets.md#mips32).

### Boot

As on RISC-V, C cannot set `sp`, and `embld -Tstack ADDR` puts a stub at
the start of the image that loads `ADDR` into `sp` and jumps to the `-e`
symbol through `t9`. The startup is then C:

```sh
embcc --target=mipsel-none-elf -Os -c boot.c -o boot.o
embld -e _start -Ttext 0x80100000 -Tstack 0x80800000 boot.o main.o \
      build/libc/mipsel-none-elf/libc.a build/libc/mipsel-none-elf/librt.a \
      -o fw.elf
```

`tests/harness/mips/boot.c` is a complete startup for QEMU's `malta`
board: it copies `.data`, zeroes `.bss`, runs the static constructors and
calls `main`. Its image is linked in KSEG0 (cached, unmapped), so
`.data` needs no `-Tdata`. A PIC32 runs from flash in KSEG1 or KSEG0 and
keeps its data in RAM: link it with `-Ttext` at the flash address and
`-Tdata` at the RAM address, as on Cortex-M. A `jal` keeps the caller's
top four address bits, so a call must stay in its 256 MiB region: code in
the boot flash (KSEG1, `0xbfc00000`) calling program flash in KSEG0
(`0x9d000000`) must go through a function pointer, and EmbLD refuses a
`jal` that does not reach (`a jal or j at ADDR to 'f' at ADDR, which is
in another 256 MiB region`).

### Exceptions and interrupts

`__attribute__((interrupt))` is refused on MIPS32. An exception or
interrupt handler is an entry written in a `.S` file (or a naked
function, or a file-scope `asm` block) that saves the registers C may
change, calls a C function, restores them and returns with `eret`.
`tests/golden/mips-exc/vector.S` is a complete one, run on `malta` by
`tests/golden/mips-exc.sh`, and its shape is:

```asm
/* vector.S */
#define FRAME (16 + 4 * 24)
    .text
    .set noat                       /* $at is saved by hand */

    .globl exc_stub, exc_stub_end   /* copied to EBase + 0x180 */
    .set push
    .set noreorder
exc_stub:
    lui     $k0, %hi(exc_entry)
    addiu   $k0, $k0, %lo(exc_entry)
    jr      $k0
    nop
exc_stub_end:
    .set pop

exc_entry:
    addiu   $sp, $sp, -FRAME
    sw      $at, 16($sp)            /* ...and v0-v1, a0-a3, t0-t9, ra */
    mfhi    $t0                     /* ...and HI, LO */
    sw      $t0, 88($sp)
    mfc0    $a0, $13                /* Cause */
    mfc0    $a1, $14                /* EPC */
    addiu   $a2, $sp, 16            /* the saved words, for C to edit */
    jal     exc_c                   /* returns where to resume */
    mtc0    $v0, $14
    ehb
    lw      $t0, 88($sp)            /* restore in reverse */
    mthi    $t0
    lw      $at, 16($sp)
    addiu   $sp, $sp, FRAME
    eret
```

The file is in `.set reorder` mode outside the stub, so the assembler
puts a `nop` after `jal` and `jr`; the stub is in `.set noreorder` so it
is exactly four words. The C side copies the stub to the vector and
clears `Status.BEV`, `ERL` and `EXL` (`tests/harness/mips/boot.c` does
the same for its fault report):

```c
extern char exc_stub[], exc_stub_end[];

unsigned exc_c(unsigned cause, unsigned epc, unsigned *frame)
{
    switch ((cause >> 2) & 31) {
    case 8:                                 /* syscall: resume after it */
        frame[1] = handle_syscall(frame[3]);    /* v0 from a0 */
        return epc + 4;
    case 0:                                 /* an interrupt: resume at it */
        if (cause & (1u << 15))             /* IP7, the CP0 timer */
            timer_tick();                   /* writes Compare: the ack */
        return epc;
    }
    return epc + 4;
}

void install(void)
{
    volatile unsigned *vec = (volatile unsigned *)0x80000180u;
    for (unsigned k = 0; k < (unsigned)(exc_stub_end - exc_stub) / 4; k++)
        vec[k] = ((unsigned *)exc_stub)[k];
}
```

A real core caches the vector's words: write them back from the data
cache and invalidate the instruction cache (`cache`, or `synci`) before
the first exception; QEMU models no caches. Interrupts are enabled and
disabled with inline `ei`, `di` and `ehb`, and CP0 registers are read and
written with `mfc0` and `mtc0`; see [MIPS32](inline-asm.md#mips32) in
Inline Assembly. `cache` and `synci` are not in the assembler's
vocabulary yet.

### Atomics

4-byte atomic read-modify-write operations are inline `ll`/`sc` loops
bracketed by `sync`. An atomic load of 4 bytes or fewer is one `lw`
(`lhu`, `lbu`) followed by `sync`, and an atomic store one `sw` (`sh`,
`sb`) with a `sync` before and after: never the two-instruction
`lwl`/`lwr` form a packed member gets, which an interrupt could split. A read-modify-write of a 1- or 2-byte object, and any 8-byte
atomic, is refused by name. `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4` is
defined, and the 1-, 2- and 8-byte forms are not.

### Runtime

`make rt-embedded libc-embedded` builds `librt.a` (the 64-bit division
routines, soft `float` and `double`, complex multiply and divide) and
`libc.a` (EmbCC's C library on its bare-metal backend: `write` is a weak
function the program overrides to reach its UART) for `mipsel-none-elf`.

## AVR (ATmega328P)

### The target and its data model

`--target=avr` generates code for the ATmega328P (`__AVR_ATmega328P__`,
AVR architecture 5). There is no `-mmcu=` option
(`embcc: error: unknown argument '-mmcu=atmega328p'`); the part is fixed.

| Type | Size |
|---|---|
| `char` | 1 (signed) |
| `short`, `int` | 2 |
| `long` | 4 |
| `long long` | 8 |
| pointers, `size_t`, `ptrdiff_t` | 2 |
| `float`, `double`, `long double` | 4 (IEEE binary32) |
| `wchar_t` | 2 (`int`) |

`double` is binary32, as in avr-gcc's default configuration: `DBL_DIG` is
6 and `DBL_MAX` is about 3.4e38.

### Memory map and limits

| Space | Range | Size |
|---|---|---|
| Flash (program space) | `0x0000`-`0x7FFF` | 32 KiB |
| Registers and I/O | `0x0000`-`0x00FF` of data space | 256 bytes |
| SRAM | `0x0100`-`0x08FF` of data space | 2 KiB |

Link with `-Ttext 0x0 -Tdata 0x100 --rom-limit 32768`. Program space and
data space are separate on this machine, and the instructions EmbCC emits
for a pointer dereference read data space only. EmbLD therefore puts
`.rodata` in the writable segment on AVR: string literals and `const`
data are stored in flash after the text, copied to SRAM by the startup,
and occupy SRAM for the life of the program. `.data`, `.rodata`, `.bss`
and the stack all share the 2 KiB; `-fstack-usage` and `llvm-size` are
how to keep track. `--rom-limit` counts the stored copy of `.data` and
`.rodata` as well as the text.

### Startup and the interrupt vector table

The AVR startup is assembly, because three of its jobs have no C
spelling: set `SPH`/`SPL` (the stack pointer is zero at reset), clear
`r1` (every AVR object treats `r1` as zero), and copy the data image with
`lpm`, which reads program space. EmbCC assembles AVR `.S` files itself.
`tests/harness/avr/boot.S` is a complete startup:

- `__vectors` at address 0 is the 26-entry vector table, one `jmp` per
  entry. Entry 0 jumps to `reset`; entry *n* jumps to `__vector_n`, each
  declared `.weak`, so a vector the program does not define resolves to
  address 0 and restarts the program.
- `reset` sets the stack pointer to `0x08FF`, clears `r1` and `SREG`,
  copies `__data_load` to `__data_start`..`__data_end`, zeroes
  `__bss_start`..`__bss_end`, and calls `main`.
- It defines `__do_copy_data` and `__do_clear_bss` at those loops.
  avr-gcc and Clang emit references to both names from any unit with
  initialised or zeroed data; defining them is what lets objects from
  those compilers link into the same image.

```sh
embcc --target=avr -c tests/harness/avr/boot.S -o boot.o
embcc --target=avr -Os -c main.c -o main.o
embld -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 \
      boot.o main.o build/libc/avr/librt.a -o fw.elf
```

The entry symbol must be at address 0: QEMU refuses an AVR image whose
ELF entry point is anything else, and the part starts there regardless.

### Interrupt handlers

Both of avr-gcc's attributes are implemented, and the handler names are
avr-libc's, so `__vector_13` lands in vector 13 of a table like the one
above:

```c
volatile unsigned char ticks;

/* TIMER1_OVF is vector 13 on the ATmega328P. */
__attribute__((signal)) void __vector_13(void) { ticks++; }
```

A handler saves `r0`, `SREG`, `r1`, every call-clobbered register
(`r18`-`r27`, `r30`, `r31`) and the frame pointer, clears `r1` for its
own body, and returns with `reti`. `signal` runs the body with interrupts
disabled. `interrupt` re-enables them with `sei` at entry, so the handler
can be interrupted. Interrupts are enabled globally with
`__asm__ volatile("sei")` and disabled with `__asm__ volatile("cli" :::
"memory")`. `__builtin_avr_sei`, `__builtin_avr_cli` and the other
`__builtin_avr_*` functions are not implemented, although their
`__BUILTIN_AVR_*` macros are predefined.

### Data in program memory

EmbCC has no address-space qualifiers. `__flash` is predefined, as
`__attribute__((__address_space__(1)))`, but the attribute is ignored
with a warning, and so is `__attribute__((progmem))`:

```text
embcc: flash3.c:1: warning: attribute 'progmem' is not one EmbCC knows, and is ignored [-Wattributes]
```

Such data goes to `.rodata` and is copied to SRAM like any other `const`
data. To keep a table in flash only, place it in a `.text` section and
read it with `lpm` through inline assembly:

```c
__attribute__((section(".text"))) const char banner[] = "from flash\n";

static unsigned char flash_byte(const char *p)
{
    unsigned char r;
    __asm__ volatile("lpm %0, Z" : "=r"(r) : "z"(p));
    return r;
}
```

Never dereference such an object directly: a C access compiles to `ld`,
which reads SRAM at the same address. A section with any other name that
is not `.text` or `.text.*` is placed in RAM.

### The calling convention

EmbCC follows avr-gcc's convention, so EmbCC and avr-gcc objects call
each other:

- Arguments are assigned from `r25` downwards, each rounded up to an even
  number of registers, down to `r8`. Once one argument does not fit, it
  and every later argument go on the stack, packed at their natural size.
- All arguments of a call to a variadic function go on the stack, the
  named ones included.
- A value is returned in `r24` (1 or 2 bytes), `r22`-`r25` (up to 4) or
  `r18`-`r25` (up to 8), its size rounded up to a power of two. A larger
  struct is returned through a pointer the caller passes in `r24:r25`.
- `r0` is a scratch register, `r1` is always zero, `r2`-`r17` and
  `r28`-`r29` are preserved by the callee, and `r18`-`r27`, `r30`-`r31`
  are not.

### Runtime helpers

EmbCC does not use the part's `mul` instruction, and the part has no
divide, so every integer multiply, divide and remainder is a call: `__mulsi3`, `__divsi3`,
`__udivsi3`, `__modsi3`, `__umodsi3` for 8-, 16- and 32-bit operands, and
`__muldi3`, `__divdi3`, `__udivdi3`, `__moddi3`, `__umoddi3` for 64-bit.
A quotient and a remainder of the same operands are two calls. Every
floating-point operation is a call into the binary32 routines (`__addsf3`,
`__mulsf3`, `__divsf3`, `__ltsf2`, `__fixsfsi`, ...). EmbCC does not call
avr-gcc's `__divmodsi4` family, and its `librt.a` does not define it.

### What is refused

| Construct | Diagnostic |
|---|---|
| Variable-length arrays | `the AVR backend cannot lower a variable-length array yet` |
| Atomic read-modify-write (`atomic_fetch_add`, compare-exchange, ...) | `the AVR backend cannot lower xadd yet` |
| Atomic load or store wider than one byte | `an atomic access of 2 bytes is not one access on this target (it moves 1 at once): the halves could be split by an interrupt or another core` |

One-byte atomic loads and stores are single instructions and compile;
fences compile to nothing.

## x86-64 kernels and EmbLinkOS

`x86_64-elf` is the freestanding x86-64 target and the default when no
`--target=` is given. `x86_64-emblink` is the same code generator with
`__EmbLinkOS__`, `__emblink__` and `__emblink` predefined, for EmbLinkOS
userland.

### Code generation options for a kernel

| Option | Effect |
|---|---|
| `-mno-sse`, `-mno-sse2`, `-mgeneral-regs-only` | never emit an SSE instruction; any floating-point operation is then an error (`floating point needs SSE, which -mno-sse forbids`). The `__SSE__` and `__SSE2__` macros stay defined. |
| `-mno-mmx`, `-mno-80387` | accepted; EmbCC emits no MMX |
| `-mno-red-zone` | accepted; EmbCC never uses the red zone |
| `-mcmodel=kernel` | accepted; the default code model already suits a higher-half link, and `__code_model_small__` stays defined |

Most kernel code needs no runtime routines. `__int128` multiply, divide
and shifts, conversions between `__int128` and floating point, and
`_Complex` multiply and divide are calls (`__multi3`, `__udivti3`,
`__ashlti3`, `__floattidf`, `__muldc3`, ...). No `librt.a` is built for
`x86_64-elf`; link libgcc, or compile the needed files of `lib/rt` for
the target (see [Libraries](libraries.md#the-compiler-runtime-librt)).

`__attribute__((interrupt))` is refused on x86-64, as on RISC-V. Interrupt
and exception entry stubs are written in assembly and call C handlers.
EmbCC assembles NASM-syntax `.asm` files (`embcc -c isr.asm -o isr.o`, or
the standalone [`embas`](tools/embas.md)); GNU-syntax `.s`/`.S` files are
refused on x86-64.

```nasm
section .text
global isr_stub
extern isr_handler
isr_stub:
    push rax
    push rcx
    call isr_handler
    pop rcx
    pop rax
    iretq
```

### Linking a kernel

EmbLD links a kernel without a linker script:

| Option or symbol | Use |
|---|---|
| `-Ttext ADDR` | the kernel's base virtual address, for example `0xFFFFFFFF80100000` |
| `--lma-offset N` | load each segment at its virtual address minus `N`: a higher-half kernel loaded low |
| `kernel_end`, `__kernel_end`, `_end`, `end`, `__bss_end` | the address past the last `.bss` byte, defined when referenced |
| `__init_array_start`/`_end`, `__fini_array_*`, `__ctors_*`, `__dtors_*` | constructor and destructor tables |
| `__start_NAME`/`__stop_NAME`, `__NAME_start`/`__NAME_end` | the bounds of any other named section, such as an export table |

```sh
embld -e _start -Ttext 0xFFFFFFFF80100000 -o kernel.elf *.o
```

`--embx` writes an EmbLinkOS EMBX executable instead of ELF, with the
capabilities given by `--cap NAME`. See [EmbLD](tools/embld.md).

The test harness's x86-64 kernel (`tests/harness/x86_64`) shows the boot
path QEMU's `-kernel` needs: a Multiboot header, the climb from 32-bit
protected mode to long mode, SSE enabled in `CR0`/`CR4` before any C
that uses it, and `.bss` zeroed. It is assembled and linked with a GNU
cross toolchain; only the program under test comes from EmbCC.

## C++ on the embedded targets

EmbCC's C++ front end lays out classes for a target whose `long` and
pointers are 8 bytes. On the Cortex-M targets, RV32, MIPS32 and AVR it
refuses to generate code for a C++ unit:

```text
embcc: error: C++ is not yet supported for thumbv7m-none-eabi: the C++ front end lays out types for 8-byte long and pointers, and this target's long is 4 bytes and its pointers 4
```

`-fsyntax-only`, `-E`, `-M` and `-MM` still accept C++ there; `-c`, `-S`,
`--emit-c` and `--emit-interfaces` do not.

C++ compiles for `riscv64-unknown-elf`, but no C++ runtime is built for
it. Code that needs a landing pad (a `try` block, or a local
object whose destructor must run while an exception unwinds) does not
compile:

```text
embcc: cx.cc:7: error: the RV64 backend cannot lower this operation yet (function _Z1gi) [landing w=8 size=4]
```

Compile with `-fno-exceptions`, and with `-fno-rtti` unless the program
supplies the `__cxxabiv1` type-information vtables. The program must then
define what the compiled code refers to: `operator new` and `operator
delete` if it uses them, `__cxa_pure_virtual` for abstract classes, and
`__cxa_atexit` and `__dso_handle` for static objects with destructors.
See [C++](cxx.md).

## Running images under QEMU

EmbCC's tests run every embedded image on QEMU. The harnesses under
`tests/harness/` are working examples of a startup, a UART driver and a
link line for each board:

| Harness | QEMU command | Output | How the run ends |
|---|---|---|---|
| `thumb-m0/` (Cortex-M0) | `qemu-system-arm -M microbit -nographic -kernel IMAGE`; `-global nrf51-soc.sram-size=65536` for more than the part's 16 KiB | nRF51 UART0 at `0x40002000`: `ENABLE` (`+0x500`) = 4, trigger `TASKS_STARTTX` (`+0x008`), then each byte to `TXD` (`+0x51C`) and wait for `EVENTS_TXDRDY` (`+0x11C`) | `_exit` prints `==EXIT n==`, and `run.sh` stops QEMU there and exits with `n` (125 for a fault, reported with the faulting pc) |
| `thumb/` (Cortex-M3) | `qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -kernel IMAGE` | UART0 data register at `0x4000C000` | the startup executes `__builtin_trap()` after `main`; with no HardFault handler the core locks up and QEMU exits |
| `thumb-m4f/` (Cortex-M4F) | `qemu-system-arm -M mps2-an386 -cpu cortex-m4 -nographic -kernel IMAGE` | CMSDK UART at `0x40004000`; set `CTRL` (`0x40004008`) to 1 first | killed at the timeout |
| `thumb-m33/` (Cortex-M33) | `qemu-system-arm -M mps2-an505 -cpu cortex-m33 -nographic -kernel IMAGE` | CMSDK UART at `0x40200000` | killed after the sentinel or the timeout |
| `riscv/` (RV32, RV64) | `qemu-system-riscv32` or `qemu-system-riscv64 -M virt -bios none -nographic -m 8 -kernel IMAGE` | 16550A UART at `0x10000000` | the startup writes `0x5555` to the SiFive test device at `0x100000`, and QEMU exits with status 0 |
| `mips/` (MIPS32r2, 24Kc) | `qemu-system-mipsel -M malta -cpu 24Kc -m 64 -display none -monitor none -serial null -serial null -serial stdio -no-reboot -kernel IMAGE` | the FPGA UART at `0xbf000900` (third serial port) | the program prints `==EXIT n ==`, then writes `0x42` to the board's reset register at `0xbf000500`, which `-no-reboot` turns into QEMU's exit |
| `avr/` (ATmega328P) | `qemu-system-avr -M uno -nographic -bios IMAGE` | USART0 (`UDR0` at `0xC6`) | killed after the sentinel or the timeout |
| `x86_64/` | `qemu-system-x86_64 -cpu max -m 128M -display none -no-reboot -monitor none -serial none -debugcon stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04 -kernel IMAGE` | debug console, port `0xE9` | the program prints `@@EMBCC-EXIT n@@` and writes to `isa-debug-exit` |
| `aarch64/` | `qemu-system-aarch64 -M virt -cpu cortex-a72 -semihosting -nographic -kernel IMAGE` | ARM semihosting | semihosting exit, carrying the status |

Board details the harnesses encode:

- On the M33 board the core starts in the Secure state, so code is linked
  at the Secure alias of SSRAM1: `-Ttext 0x10000000 -Tdata 0x10100000`,
  stack top `0x10200000`. An image linked at `0` fails with QEMU's
  `Lockup: can't escalate 3 to HardFault`.
- On `virt`, `-bios none` keeps OpenSBI out, so the image runs in machine
  mode from `0x80000000`.
- On `uno`, the image is given with `-bios`, not `-kernel`.
- On `malta`, `-kernel` loads the image at its KSEG0 link address
  (`0x80100000`) and QEMU's own reset code jumps to its entry.
- `isa-debug-exit` can only report `(v << 1) | 1`, which is why the x86-64
  harness prints the real exit code first and `run.sh` reads it back
  (124 for a timeout, 125 for a crash).

A bare-metal image that does not end QEMU itself runs until killed.
`tests/harness/qrun.sh SECONDS [--until TEXT] COMMAND...` runs a QEMU
command under a hard timeout; with `--until`, it watches the output and
kills QEMU as soon as `TEXT` appears, so a test program that prints a
sentinel such as `DONE` as its last line costs only its own run time:

```sh
tests/harness/qrun.sh 10 --until DONE \
    qemu-system-avr -M uno -nographic -bios fw.elf
```

The harness scripts read these environment variables:

| Variable | Meaning |
|---|---|
| `EMBCC_QEMU_TIMEOUT` | seconds before a run is killed (10 for the boards, 20 for x86-64 and AArch64) |
| `EMBCC_QEMU_UNTIL` | the sentinel `avr/run.sh` and `thumb-m33/run.sh` pass to `--until` |
| `EMBCC_QEMU_ARM`, `EMBCC_QEMU_RISCV`, `EMBCC_QEMU_MIPS`, `EMBCC_QEMU_AVR`, `EMBCC_QEMU_X86`, `EMBCC_QEMU_AARCH64` | the QEMU binary to run |
| `EMBLD` | the `embld` the board `link.sh` scripts use |
| `EMBCC_THUMB_HARNESS`, `EMBCC_M33_HARNESS`, `EMBCC_RISCV_HARNESS`, `EMBCC_MIPS_HARNESS`, `EMBCC_AVR_HARNESS` | where `boot.o` and `io.o` were built |

## Debugging a board

`-g` works on every embedded target, and EmbLD carries the DWARF
sections into the image and writes a `.embdbg` index beside it. The image
keeps its symbol table, so a debugger can resolve function names even
without `-g`.

[EmbDBG](tools/embdbg.md) connects to anything that speaks the GDB remote
serial protocol: QEMU's gdb stub, or OpenOCD in front of a real part. Its
remote client knows the register layouts of ARM M-profile, RV32, RV64,
x86-64 and AArch64; AVR is not among them.

```sh
qemu-system-riscv32 -M virt -bios none -nographic -m 8 -kernel fw.elf -S -gdb tcp::3333 &
embdbg fw.elf remote :3333
```

Breakpoints, stepping, registers and memory are described in
[Debugging](debugging.md).
