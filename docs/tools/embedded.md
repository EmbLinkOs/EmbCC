# Building firmware with EmbCC

EmbCC targets ARM Cortex-M (ARMv7-M, Thumb-2) and RISC-V (RV32IM and
RV64IM) alongside x86-64 and aarch64, and `embld` links the result into a
firmware image. No other toolchain is involved: no `arm-none-eabi-gcc`,
no `riscv64-elf-gcc`, no `ld`, no linker script, and no assembler.

Most of this page is written for Cortex-M, which came first.
[RISC-V](#risc-v) says what differs; everything not mentioned there is
the same.

These are early targets. Read [what they cannot do](#what-it-cannot-do-yet)
before planning around them.

## The target

```
embcc --target=thumbv7m-none-eabi -Os -c main.c -o main.o
```

Accepted spellings, all the same target: `thumbv7m-none-eabi` (the
canonical one), `thumbv7m`, `thumbv7em-none-eabi`, `thumbv7em`,
`armv7m-none-eabi`, `arm-none-eabi`. It is freestanding by construction
— a microcontroller has no operating system under the code, so there is
no hosted spelling of it.

If every compile in a project is for the same board, stop typing it:

```
make DEFAULT_TARGET=thumbv7m-none-eabi     # build EmbCC for the board
embcc -Os -c main.c -o main.o              # ...and it stays the board
```

`EMBCC_DEFAULT_TARGET=thumbv7m-none-eabi` does the same for one shell,
and `--target=` still overrides both — the binary keeps every backend.
See [the default target](embcc.md#the-default-target).

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

- Atomics, VLAs, computed `goto`, C++ exceptions, `-g`.

Inline assembly DOES work now on both embedded targets — see
[inline assembly](#inline-assembly) below — and so does register
allocation at `-O2` and `-Os`.

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

## Aggregates

Structs pass and return by value, the way AAPCS32 says and the way
another ARM toolchain does it: a composite of four bytes or fewer comes
back in r0 and a larger one through a hidden pointer the caller
supplies; arguments fill the core registers and then the stack, and a
composite may be **split** across r3 and the stack. A struct of floats
is an ordinary composite here — the homogeneous-aggregate rule belongs
to the VFP variant, not to this one.

`tests/golden/thumb-exec.sh` links a clang-compiled callee against an
EmbCC-compiled caller and back again, so this is checked against another
toolchain and not only against itself.

## Variadic functions

`printf`-shaped functions work. AAPCS32 passes a variadic argument
exactly as it passes a named one, so a `va_list` is a bare `char *` at
the next argument; the prologue of a variadic function spills r0-r3
immediately below the caller's stack arguments so that one pointer walks
from the registers straight into them. `va_copy` is a pointer
assignment here, not a record copy.

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

These are smaller than they were: the register allocator runs at `-O2`
and `-Os`, so a value with a short enough live range never reaches the
frame at all. Code size against clang on the same sources is about 3.7x
for ARMv7-M and 1.7x for both RISC-V widths — down from 5.4x and
5.4x/7.1x before the allocator.

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

## RISC-V

```
embcc --target=riscv32-unknown-elf -Os -c main.c -o main.o
embcc --target=riscv64-unknown-elf -Os -c main.c -o main.o
```

Accepted spellings: the canonical two above, plus `riscv32`/`riscv64`,
`riscv32-elf`/`riscv64-elf`, and `rv32`/`rv64`. Both are freestanding,
soft-float (`-march=rv32im`/`rv64im`: no F, no D, no C, no A), and use
the **medany** code model.

### What differs from Cortex-M

| | riscv32 | riscv64 | thumbv7m |
|---|---|---|---|
| `void *`, `long` | 4 | 8 | 4 |
| `long double` | 16 (IEEE binary128) | 16 | 8 (a double) |
| plain `char` | unsigned | unsigned | unsigned |
| `wchar_t` | **signed** `int` | **signed** `int` | `unsigned int` |
| `__int128` | no | **yes** | no |

`long double` has the right size and format and the front end folds it
correctly, but there is no binary128 ARITHMETIC yet: an operation on one
is refused by name rather than lowered.

### Starting up

This is the one place RISC-V needs something a Cortex-M does not. Every
register is zero at reset and there is no vector table for the hardware
to read an initial stack pointer out of — and C cannot write `sp`. So
`embld` emits four instructions in front of the entry point:

```
embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 boot.o main.o -o fw.elf
```

`-Tstack` is what asks for them; they set `sp` and jump to `-e`'s symbol,
and they come from the same encoder the compiler uses rather than from
four hex constants. Without it, the first function's prologue subtracts
from a stack pointer of zero and faults. It is refused on the other
targets, which do not need it.

The rest is the same: `.data` brackets (`__data_load`, `__data_start`,
`__data_end`, `__bss_start`, `__bss_end`) for a plain C startup, and
`-Tdata` for a flash-to-RAM layout when the two are in different places.
A `virt`-style image loaded wholly into RAM needs no `-Tdata`.

`tests/harness/riscv/` is a complete working example: a startup, a
16550A UART driver, and a linker invocation, all in C.

### Running it

QEMU's `virt` board, which is what the test suite uses:

```
qemu-system-riscv64 -M virt -bios none -nographic -m 8 -kernel fw.elf
```

`-bios none` matters: without it QEMU loads OpenSBI first, prints a
banner and hands control over in supervisor mode. The board's UART is at
`0x10000000` and its SiFive test device at `0x100000`, where writing
`0x5555` exits QEMU — so unlike a Cortex-M image, a RISC-V one can stop
cleanly and the runner can believe its exit status.

## Debugging a running board

EmbDBG speaks the GDB remote serial protocol, so it drives whatever is on
the other end of a stub — QEMU, or OpenOCD over SWD/JTAG to a real chip:

```
qemu-system-riscv64 -M virt -bios none -nographic -m 8 -kernel fw.elf -S -gdb tcp::3333 &
embdbg fw.elf remote :3333
```

It reads commands from stdin, so a session is a script:

```
break compute          # by function, FILE:LINE, or *0xADDR
continue
where                  # symbolized pc, source context, locals in scope
regs                   # by ABI name — a0..a7/s0 on RISC-V, r0..r12/sp/lr/pc on ARM
bt
mem 0x80001000 64
step                   # one SOURCE line (stepi for one instruction)
quit
```

For OpenOCD, point it at the same thing: `embdbg fw.elf remote :3333` against
whatever port `gdb_port` is configured for.

What works without `-g`: breakpoints by function name, registers, memory,
frame 0 and the caller from the return-address register. `-g` is still
refused on these backends, so source lines and locals need a host target
for now.

Note that `embld` keeps a **symbol table** in the image (outside every
`PT_LOAD`, so it costs no flash). That is what makes `break compute`
resolvable, and it also gives `llvm-objdump -d` real function names.

## Inline assembly

`asm()` works on both embedded targets. The vocabulary is what a program
reaches inline assembly FOR and cannot say in C, rather than a general
assembler:

| | ARMv7-M | RISC-V |
|---|---|---|
| special registers | `mrs`/`msr` over all 14 (PRIMASK, BASEPRI, CONTROL, MSP, PSP, …) | `csrr`/`csrw`/`csrs`/`csrc` and the `csrr*` forms, over the machine and supervisor CSRs |
| interrupt masking | `cpsid`/`cpsie i,f` | via `csrc`/`csrs mstatus` |
| barriers | `dsb`, `dmb`, `isb` | `fence`, `fence.i` |
| waiting | `wfi`, `wfe`, `sev`, `yield` | `wfi` |
| returning from a trap | — | `mret`, `sret` |
| atomics' primitives | `ldrex`/`strex` | — (no A extension under `-march=rv32im`) |
| plus | the arithmetic, shifts, loads and stores a hand-written sequence mixes in | the same |

```c
static unsigned enter_critical(void)          /* Cortex-M */
{
    unsigned prev;
    __asm__ volatile("mrs %0, primask" : "=r"(prev));
    __asm__ volatile("cpsid i" ::: "memory");
    return prev;
}

static unsigned long hartid(void)             /* RISC-V */
{
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}
```

Operand constraints are gcc's: `"r"`, `"m"`, `"i"`, `"="`, `"+"`,
`%0`/`%[name]`, and a clobber list. Two things are refused rather than
guessed at, both for the same reason:

- **An instruction outside the vocabulary**, by name. A hand-written
  assembler that guesses is worse than one that stops.
- **A callee-saved register**, in a clobber list or named in the
  template. EmbCC saves nothing around an asm, so writing `r5` on
  ARMv7-M or `s2` on RISC-V would corrupt the caller silently.

A `.w`/`.n` suffix on an ARM mnemonic is accepted and ignored: the
encoder already chooses the width, and honouring the suffix would mean
a second width policy that could disagree with the first.

## What proves it

- `tests/golden/embdbg-remote.sh` starts QEMU with its gdb stub on RV64,
  RV32 and ARMv7-M, connects EmbDBG's own client, breaks on a function by
  name, and requires the argument registers to hold what the caller
  passed — which is what pins the per-architecture register layout.
- `tests/golden/riscv-encoding.sh` round-trips every encoder through
  `llvm-mc --disassemble` at both widths, checks `rv_li`'s constant
  sequences by EXECUTING them over 40,000 values, and requires all
  twelve of the encoder's range checks to fire.
- `tests/golden/riscv-exec.sh` runs the five shared programs
  (`tests/golden/embedded-*.c`) at RV32 **and** RV64, at -O0, -O1, -O2
  and -Os — the stress program against `clang` for the same triple, the
  rest against the host. Running both widths is the point: three of the
  bugs found while writing the backend passed at one width and failed at
  the other.
- `tests/golden/riscv-target.sh` checks both data models, and checks that
  each fails on the other width.
- `tests/golden/thumb-encoding.sh` disassembles every instruction the
  encoder can produce and diffs it against what each call was meant to
  emit, plus all 4093 distinct modified immediates.
- `tests/golden/embedded-float.c` prints IEEE results as BIT PATTERNS and
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
