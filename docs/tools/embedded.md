# Building firmware with EmbCC

EmbCC targets ARM Cortex-M (ARMv7-M, Thumb-2) alongside x86-64 and
aarch64, and `embld` links the result into a firmware image. No other
toolchain is involved: no `arm-none-eabi-gcc`, no `ld`, no linker script,
and — because of how a Cortex-M starts — no assembler.

This is an early target. Read [what it cannot do](#what-it-cannot-do-yet)
before planning around it.

## The target

```
embcc --target=thumbv7m-none-eabi -Os -c main.c -o main.o
```

Accepted spellings, all the same target: `thumbv7m-none-eabi` (the
canonical one), `thumbv7m`, `thumbv7em-none-eabi`, `thumbv7em`,
`armv7m-none-eabi`, `arm-none-eabi`. It is freestanding by construction
— a microcontroller has no operating system under the code, so there is
no hosted spelling of it.

### The data model

It is the first ILP32 target here, and the differences from the 64-bit
ones are the ones that bite:

| | thumbv7m | x86-64 / aarch64 |
|---|---|---|
| `void *`, `long` | 4 bytes | 8 |
| `long long` | 8 | 8 |
| `long double` | 8 (an IEEE double) | 16 |
| plain `char` | **unsigned** | signed on x86-64 |
| `wchar_t` | `unsigned int` | |
| `__int128` | does not exist | exists |
| `_Alignof(long long)` | **8**, not 4 | 8 |

`embcc --target=thumbv7m-none-eabi --dump-predef` prints the whole
predefined-macro table, which is generated from
`clang -target thumbv7m-none-eabi -dM -E` and matches it exactly.

## Starting up

A Cortex-M needs no assembly to boot. The processor fetches the initial
stack pointer from the first word of the image and the reset address
from the second, so the vector table is an array and the reset handler
is a function:

```c
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;

int main(void);
void reset(void);

__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)0x20010000u, (void *)reset };

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end) *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; ) *d++ = 0;
    main();
    for (;;) ;
}
```

`.vectors` is placed at the very start of the image by the linker — it
is an output section of its own, ahead of `.text`, because the processor
does not look the table up, it reads address zero. `.isr_vector`, the
CMSIS name, works the same way.

The five bracket symbols are provided by `embld` when it is given
`-Tdata`, and they are all a C runtime needs on this machine.

## Linking

```
embld -e reset -Ttext 0x0 -Tdata 0x20000000 boot.o main.o -o firmware.elf
```

`-Ttext` is where the image is stored and executed from (flash).
`-Tdata` is where the writable data is **addressed** (RAM); its bytes
are **stored** immediately after the text in flash, and `__data_load`
points at them. That split is the whole of what a linker script's
`AT> FLASH` says, and it is the reason a startup can copy `.data` into
place without one.

The output is an ELF32 `EM_ARM` executable with `EF_ARM_EABI_VER5`,
segments aligned to 4 bytes rather than a page, and the entry point
carrying the Thumb bit. `objcopy -O binary` it for a flash tool, or hand
it straight to QEMU:

```
qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -kernel firmware.elf
```

`embld` reads ordinary ARM objects too — both `SHT_REL` (what every ARM
toolchain emits) and `SHT_RELA` — so an object from another compiler can
be linked in beside EmbCC's.

## What it cannot do yet

Each of these stops the compile with a message naming the construct and
the IR operation behind it, rather than emitting something plausible:

- **64-bit integers.** `long long`, `uint64_t`, and packed bitfields
  (which the front end assembles in a 64-bit accumulator). This needs a
  pass that splits 64-bit operations into register pairs.
- **Floating point.** ARMv7-M's base profile has no FPU, so every
  operation is a call into `__aeabi_fadd` and its family — a lowering
  that does not exist yet.
- Aggregates passed or returned **by value**, variadic functions,
  atomics, inline assembly, VLAs, computed `goto`, C++ exceptions, `-g`.

There is also no register allocator for this target yet: every value
lives in a stack slot, so the code is correct and roughly three times
larger than clang's. Optimisation levels work and are worth using —
`-Os` and `-O2` both run the full optimizer — but the win is in the IR,
not in register assignment.

## What proves it

- `tests/golden/thumb-encoding.sh` disassembles every instruction the
  encoder can produce and diffs it against what each call was meant to
  emit, plus all 4093 distinct modified immediates.
- `tests/golden/thumb-exec.sh` compiles a program covering structs,
  arrays, `switch`, recursion, function pointers, bit manipulation and
  signed and unsigned division, links it with `embld`, runs it on QEMU's
  Cortex-M3, and requires the output to match what `clang` produces for
  the same source through the same linker and the same board.
- `tests/golden/thumb-target.sh` checks the data model, and checks that
  the same assertions fail on x86-64.
