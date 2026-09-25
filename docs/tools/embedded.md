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

- Aggregates passed or returned **by value**, variadic functions,
  atomics, inline assembly, VLAs, computed `goto`, C++ exceptions, `-g`.

There is also no register allocator for this target yet: every value
lives in a stack slot, so the code is correct and roughly three times
larger than clang's. Optimisation levels work and are worth using —
`-Os` and `-O2` both run the full optimizer — but the win is in the IR,
not in register assignment.

## 64-bit integers

`long long` and `uint64_t` work, in register pairs. Add, subtract,
multiply, the bitwise operations, negation, the shifts and the
comparisons are all inline; **divide and remainder are a call** into
`lib/rt/int64.c`, under libgcc's names (`__divdi3`, `__udivdi3`,
`__moddi3`, `__umoddi3`). Compile that file for the target and link it
in, or link a libgcc that provides them:

```
embcc --target=thumbv7m-none-eabi -Os -c lib/rt/int64.c -o int64.o
```

Objects from another ARM toolchain call `__aeabi_ldivmod` instead, which
returns its quotient and remainder in four registers at once and so
cannot be written in C. EmbCC does not provide that one; an object that
needs it must bring its own.

Bitfields depend on this — the front end assembles a field's storage
unit in a 64-bit accumulator — so they work now too, including
`volatile` ones, which is how a peripheral's registers are written.

## Floating point

`float` and `double` work. ARMv7-M's base profile has no FPU, so every
operation is a **call** into `lib/rt/softfp.c`, under libgcc's names
(`__adddf3`, `__mulsf3`, `__ltdf2`, `__floatsidf`, …). Compile that file
for the target and link it in beside `int64.c`:

```
embcc --target=thumbv7m-none-eabi -Os -c lib/rt/softfp.c -o softfp.o
```

The results are **bit-identical** to hardware — the same IEEE answers a
desktop gives, checked that way rather than to a few digits. Only
binary64 is implemented; a `float` operation is done by widening both
operands, doing it in binary64, and rounding back, which gives the same
answer as computing in binary32 directly because 53 significand bits is
at least 2p+2 for p = 24.

It is not fast. Expect a few hundred cycles for a double multiply where
an M4F with `-mfpu` would take one, and prefer `float` to `double` where
the precision allows — `float` still goes through binary64 here, but the
values are smaller and the conversions cheap. Hardware floating point on
Cortex-M4F and M7 is not supported yet.

`long double` is refused: it is 8 bytes on this ABI (the same as
`double`), and the 16-byte formats the other targets use do not exist
here.

## Sizing the stack

There is no guard page on a microcontroller and nothing to grow into, so
the deepest call path has to be added up ahead of time and has to fit.
`-fstack-usage` writes `FILE.su` beside the object in gcc's format —
`file:line:function`, a tab, the frame in bytes, a tab, `static` — so
the tools that already read those files read these:

```
embcc --target=thumbv7m-none-eabi -Os -fstack-usage -c main.c -o main.o
```

The number is that function's OWN frame: the registers its prologue
pushes plus its locals, temporaries and outgoing arguments. It does not
include what the function calls; combining the two is what a call-graph
tool does with these files, and `embcc inspect callgraph` prints the
graph.

Expect large numbers for now — every value lives in a stack slot until
this target has a register allocator, and the same function that takes
80 bytes on x86-64 can take ten times that here.

## Interrupt handlers

`__attribute__((interrupt))` is accepted, and needs nothing:

```c
__attribute__((interrupt)) void SysTick_Handler(void) { ticks++; }
```

The Cortex-M stacks r0-r3, r12, lr, pc and xPSR itself on exception
entry and leaves EXC_RETURN in lr, so an ordinary prologue saves the
rest and an ordinary `bx lr` **is** the interrupt return. That is why
the attribute is a no-op here and an error on x86-64 and aarch64, where
a handler really would need code no C function emits.

Point the vector table at the handler the same way as at the reset
handler — an entry in the `.vectors` array — and the linker fills in
the address with its Thumb bit already set.

## What proves it

- `tests/golden/thumb-encoding.sh` disassembles every instruction the
  encoder can produce and diffs it against what each call was meant to
  emit, plus all 4093 distinct modified immediates.
- `tests/golden/thumb-float.c` prints IEEE results as BIT PATTERNS and
  requires them to equal the host's, which does the same arithmetic in
  hardware — so a rounding that is off by one unit in the last place
  fails. The soft-float core is also checked on the host against 400,000
  random bit patterns.
- `tests/golden/thumb-exec.sh` compiles a program covering structs,
  arrays, `switch`, recursion, function pointers, bitfields, bit
  manipulation and signed and unsigned division, links it with `embld`,
  runs it on QEMU's Cortex-M3, and requires the output to match what
  `clang` produces for the same source through the same linker and the
  same board. It then does the same for a 64-bit program, against the
  HOST compiler — `long long` arithmetic has one answer whatever the
  register width.
- `tests/golden/thumb-target.sh` checks the data model, and checks that
  the same assertions fail on x86-64.
