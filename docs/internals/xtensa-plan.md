# Xtensa (ESP32, windowed ABI): the plan

The target is little-endian Xtensa with the windowed-register calling
convention, the instruction set of Espressif's ESP32 (Xtensa LX6) and
ESP32-S3 (LX7): `xtensa-none-elf` (also `xtensa-esp32-elf`). EmbCC emits
the subset those cores share with QEMU's `de212` core, on which it is
tested. Every fact below was read off a source rather than remembered:

- **Espressif's GCC** -- `xtensa-esp32-elf-gcc` 16.1.0 from Espressif's
  crosstool-NG release `esp-16.1.0_20260609` (the aarch64-apple-darwin
  build, kept in `~/EmbRef/xtensa-esp-elf-16.1.0`; `EMBCC_REF_GCC_XTENSA`
  points the tests at it). Its `-dM` dump is the predefined-macro
  table, its `-S` output shows the calling convention and frame, and
  its objects are the other side of the ABI test.
- **GCC's sources** (`gcc/config/xtensa/xtensa.h`, `xtensa.cc`, `elf.h`):
  the argument rules (`xtensa_function_arg_1`, `..._advance`), the return
  rule (`xtensa_return_in_memory`), the frame (`compute_frame_size`) and
  the va_list record (`xtensa_build_builtin_va_list`, `xtensa_va_start`,
  `xtensa_gimplify_va_arg_expr`).
- **QEMU 11.1's de212 core** (`target/xtensa/core-de212/core-isa.h`) and
  its disassembler, generated from the core's ISA description, which is
  the encoding referee (there is no Xtensa llvm-mc here).
- **Running code on QEMU**, which is the second referee for every rule:
  the exec corpus, the window exceptions, the alloca exception.

## The core

QEMU offers eight Xtensa cores. `de212` (Diamond 212GP, `HW_VERSION_NAME`
"LX6.0.2") is the one closest to an ESP32: an LX6, no MMU (region
protection), windowed registers (32 physical AR registers; the ESP32 has
64), MUL16, MUL32, DIV32, NSA, MINMAX, SEXT, ABS, ADDX, L32R, density,
S32C1I, loops and MAC16, no FPU, unaligned accesses raising an exception
(as on the ESP32). `sample_controller` (LX7) has no loops or MAC16 but is
otherwise similar; `dc232b`/`dc233c`/`de233_fpu` carry an MMU; `lx106`
(the ESP8266) has no register windows, so it cannot run the windowed ABI.

What EmbCC emits is the intersection of the de212, the ESP32 and the
ESP32-S3: the core ISA, windowed registers, MUL32 (`mull`), DIV32
(`quos`/`quou`/`rems`/`remu`), MINMAX, SEXT, NSA/NSAU, ABS, ADDX/SUBX and
L32R, and S32C1I for atomics. It does not use MUL32_HIGH (`mulsh`/`muluh`:
the ESP32 has it, the de212 does not), CLAMPS (the de212 has it, the
ESP32 does not), the FPU (the ESP32 has one, the de212 does not; the ABI
is the same either way, see below), MAC16, the loop instructions or the
boolean registers. The density option's 16-bit instructions are not
emitted yet. The ESP32-S2 (LX7) has no S32C1I, so atomics need a
different lowering there; it is not a target yet.

## The windowed calling convention

- **Registers.** The sixteen visible registers a0-a15 are a window onto
  the physical file. In a function, a0 holds the return address (its top
  two bits are the caller's window increment), a1 is the stack pointer,
  a2-a7 are the incoming arguments. A call8 rotates the window by eight:
  the callee's a0-a7 are the caller's a8-a15, so the caller's own a0-a7
  survive the call untouched and a8-a15 do not. Nothing is saved or
  restored by either side; the hardware's window overflow and underflow
  exceptions spill and refill the oldest windows when the physical file
  is full. So for the register allocator a2-a7 behave as callee-saved
  registers that cost nothing to keep, and a8-a15 as caller-saved.
  Espressif's GCC uses call8 exclusively, and so does EmbCC.
- **Arguments** are counted in words, six in registers: the caller puts
  them in a10-a15 and the callee finds them in a2-a7. An argument whose
  type is aligned beyond 4 (a `long long`, a `double`, a struct containing
  one) starts at an even word (a10/a12/a14). An argument that does not fit
  entirely in the words left goes wholly on the stack, and so does every
  argument after it (`arg_words` jumps to six) -- `f(int a, int b, int c,
  int d, int e, long long f, int g)` passes a-e in a2-a6, leaves a7 empty
  and puts f at the caller's sp+0 and g at sp+8. Stack arguments are laid
  out from the caller's sp upward, each aligned to its type (4 to 16) and
  occupying whole words; the callee reads them at its incoming sp. A
  struct of any size is passed by value under the same rule (never by
  reference). A `_Complex` argument is split into its two parts, each
  placed as its own argument. A variadic argument follows the same rules
  as a named one.
- **Results.** Up to 16 bytes come back in a2-a5 (the caller's a10-a13):
  an int or pointer in a2, a `long long` or `double` in a2:a3 (low word
  first), a struct of up to 16 bytes in a2-a5 in its memory order. A
  larger struct is returned through a hidden pointer passed as the first
  argument (a2, moving the others up a word), which the callee leaves in
  a2. Arguments and results narrower than a word are extended to 32 bits
  by the side that produces them (GCC's
  `default_promote_function_mode_sign_extend`).
- **The frame.** `entry a1, N` at the function's first instruction moves
  the window and sets sp = caller's sp - N, N a multiple of 16 (the stack
  is 16-aligned) and at most 32760. From sp upward: the outgoing
  argument area, then the locals; the top 32 bytes are reserved: the
  upper 16 hold the caller's caller's a0-a3 when the window overflow
  handler spills them (the "base save area", which belongs to the 16
  bytes below the caller's sp), the lower 16 this function's own a4-a7
  when it is spilled as an 8-register window. So N = align16(locals +
  outgoing + 32), at least 32 -- `entry sp, 32` is GCC's empty function.
  Return is `retw`, which undoes the window rotation and with it the
  stack pointer. A frame larger than 32760 is `entry a1, 32` and then a
  `movsp` to the full size, and an `alloca` moves sp with `movsp` too:
  movsp raises the Alloca exception when the caller's registers have
  already been spilled to the old base save area, and the runtime's
  handler moves them (the ESP-IDF's `_xt_alloca_exc`; the test harness
  has its own).
- **va_list** is GCC's 12-byte record, so that a `va_list` passes between
  EmbCC and GCC objects (`vprintf`, `esp_log_writev`):

      typedef struct { int *__va_stk; int *__va_reg; int __va_ndx; } va_list;

  `va_start` stores the unnamed register arguments into a 24-byte area in
  the frame (`__va_reg`, indexed by word number 0-5), sets `__va_stk` to
  the incoming sp minus 32 and `__va_ndx` to the named words times four,
  plus eight when they already reached six. `va_arg` rounds `__va_ndx` up
  to the type's alignment, advances it by the size in words, and reads
  from `__va_reg` if the new index is at most 24, else from `__va_stk`
  (moving an argument that straddles 24 to index 32 first, because it was
  passed wholly on the stack). The record is passed by value, a struct of
  three words.
- **Data model** (`xtensa-esp32-elf-gcc -dM`): ILP32; `long long`,
  `double` and `long double` are 8 bytes and 8-aligned (long double is
  double); plain `char` is UNSIGNED; `wchar_t` is a 16-bit `unsigned
  short` (xtensa/elf.h); `int32_t` is `long`; `size_t` is `unsigned int`;
  `__BIGGEST_ALIGNMENT__` is 16. Unnamed bit-fields do not raise a
  structure's alignment (GCC's generic `PCC_BITFIELD_TYPE_MATTERS` rule).
- **Floating point.** The ABI keeps every float and double in the address
  registers whether or not the core has an FPU -- GCC for the ESP32 uses
  its FPU only inside a function -- so EmbCC's soft float (lib/rt,
  libgcc's names) links with hard-float GCC code. There is no
  `-msoft-float` in Espressif's GCC and no `__XTENSA_SOFT_FLOAT__` in its
  table, and EmbCC's table is that one.
- **Volatile.** GCC's default `-mserialize-volatile` puts a `memw` before
  every volatile load and store, which the ESP32's write buffer needs for
  device registers; EmbCC does the same.

## Constants and literal pools

`movi` takes a signed 12-bit value. Anything else that is not a 12-bit
value shifted (movi + slli), or such a value plus a small remainder
(+ addi), is loaded with `l32r` from a literal pool. `l32r` reaches only
BACKWARDS, 4 to 262144 bytes from its own address rounded up to 4, so
each function's pool is placed in .text immediately before the function:
the backend generates the body, collects its literals (constants and
addresses), writes the pool, then the body. The pool is 4-aligned and a
multiple of 4 bytes, so the function's entry is 4-aligned, which a call
needs (a CALLn target is `(pc & ~3) + 4 + offset * 4`). An address
literal carries an `R_XTENSA_32` relocation, so the function itself has
no relocated instruction except its calls. A function body over 256 KiB
cannot reach its pool and is refused by name.

## Branches

A two-register branch (`beq`, `blt`, `bltu`, ..., `bany`, `bbci`) and the
compare-with-constant branches (`beqi`, `blti`, `bltui`, ... against the
sixteen b4const/b4constu values) reach -128..+127 bytes from the
instruction after them; `beqz`/`bnez`/`bltz`/`bgez` reach +-2 KiB; `j`
reaches +-128 KiB. Branches inside a function are resolved by the
compiler. Every branch is first tried in its short form; one that does
not reach becomes the inverse branch over a `j` and the function is
generated again until nothing new fails (the MIPS backend's relaxation).
A `j` that does not reach (a function over 128 KiB) is refused by name.
There is no "set if less than": a comparison's 0 or 1 is a branch over a
`movi`, or a conditional move.

## Relocations

Xtensa objects carry RELA relocations. EmbCC writes:

| Type | Where | Value |
| --- | --- | --- |
| `R_XTENSA_32` (1) | a literal or data word | S + A |
| `R_XTENSA_SLOT0_OP` (20) | `call8`'s offset18 | (S + A - ((P & ~3) + 4)) >> 2 |

EmbLD also applies `R_XTENSA_SLOT0_OP` to `j`, the branches and `l32r`
(what GNU as leaves against a symbol in another section), ignores
`R_XTENSA_ASM_EXPAND` (an assembler relaxation hint) and accepts
`R_XTENSA_NONE`; `R_XTENSA_DIFF*` (debug-section differences) and every
other type are refused by name.

## ELF header

`e_machine` is `EM_XTENSA` (94), class 32, little-endian, `e_flags`
`0x300` (`EF_XTENSA_XT_INSN | EF_XTENSA_XT_LIT`), what GNU as writes for
the ESP32 objects.

## The board: QEMU sim, de212

- `qemu-system-xtensa -M sim -cpu de212 -semihosting -kernel IMAGE`. The
  sim machine maps the core's local memories and system RAM at
  0x60000000 (`overlay_tool.h`, region-protection cores) and loads the
  ELF into it, starting at its entry.
- **Output and exit** are simcalls (`simcall` with a2 = the call number,
  a3.. its arguments, `target/xtensa/xtensa-semi.c`): `SYS_write` (4) of
  fd 1 prints, `SYS_exit` (1) ends QEMU with a3 as its status. The harness
  still prints the `==EXIT n ==` sentinel, which tests/harness/qrun.sh
  --until waits for.
- **Startup.** `embld -Tstack` writes an entry stub: sp from a literal,
  PS = WOE | UM (window exceptions on, user vector mode, interrupts at
  level 0), and a `callx8` to `_start`, so `_start` is an ordinary
  windowed C function. VECBASE resets to 0x60000000 on the de212; images
  are linked at 0x60010000 and the harness copies its vectors to
  0x60000000: the window overflow/underflow handlers (4, 8, 12) at
  offsets 0x00-0x17f, the user exception vector at 0x340 (the Alloca
  exception is handled; every other cause reports `==FAULT cause n pc
  addr==` and exits), and the kernel and double exception vectors.

## What the first backend refuses

By name: `__int128` (no 128-bit type on ILP32), atomics wider than a word
(one and two bytes are an s32c1i loop on the word around them), computed goto, `__builtin_frame_address`
and `__builtin_return_address`, inline and file-scope assembly, naked and
interrupt functions, tail calls (the windowed ABI has none), C++, `-S`
(no Xtensa assembler to read it back), a function over 128 KiB, and every
machine flag except `-mabi=windowed`, `-mlongcalls`/`-mno-longcalls`
(calls are direct either way; the linker refuses one out of reach),
`-mtext-section-literals` and `-mserialize-volatile`.

## Tests

| Test | What it checks |
| --- | --- |
| `tests/golden/xtensa-encoding.sh` | every encoder form decoded by QEMU's de212 disassembler; the constant sequences executed; every range check |
| `tests/golden/xtensa-exec.sh` | `tests/exec/*.c` on the de212 at -O0, -O1, -O2 and -Os |
| `tests/golden/xtensa-abi.sh` | calls in both directions against Espressif's GCC (skipped without it) |
| `tests/golden/xtensa-refuse.sh` | the object's header, the accepted and refused options and constructs |

## Status

- Encoder and referee: 1489 forms decoded by QEMU's de212 disassembler,
  the literal-free constant sequences executed, 40 range checks
  (`xtensa-encoding.sh`).
- Code generator at -O0, -O1, -O2 and -Os, with the shared optimizer and
  register allocator (a pair pass for 64-bit values, the shorter of the
  two attempts kept); EmbLD; lib/rt and lib/libc.
- `xtensa-exec.sh`: the exec corpus on the de212, 197 of 197 programs at
  every level (14 of them judged against GCC for an LP64 or 32-bit-wchar_t
  assumption, 17 not applicable). Also run with the pair pass forced on
  and off and with `EMBCC_RA_MAXPOOL=3` and `=1`: 197 of 197 each.
- `xtensa-abi.sh`: EmbCC and Espressif's GCC (for the de212, through its
  -mdynconfig plugin, tools/xtensa-ref-gcc.sh) call each other identically
  in both directions at -O0 and -O2. A random ABI test of the same kind
  (1100 seeds: random signatures, structs aligned up to 16, _Complex and
  variadic arguments) found one bug, a variadic _Complex read as one
  composite, now fixed; 1400 random gen2 programs agree with the host at
  -O0, -O2 and -Os.
- `-g` (frame base a1, a7 under alloca), lib/libc's output on the board
  (`libc-embedded.sh`), the predefined macros (`predef.sh`), the refusals
  (`xtensa-refuse.sh`).

Known gaps, each refused by name rather than miscompiled: inline and
file-scope assembly (and so naked functions), `-S`, jump tables (a dense
switch is a decision tree), unwind tables, computed goto, the frame and
return address builtins, atomics other than a word, interrupt
attributes, C++. Code size: no density (16-bit) instructions, no zero-
overhead loops, no MUL32_HIGH (the ESP32 has it, the de212 does not), and
the 64-bit operations go through scratch registers; GCC -Os code is
smaller. A struct argument or return value is read with l32i only where
irgen promises its address is aligned (ir_arg.natural); a packed
structure's member goes a byte at a time
(tests/exec/packed-member-by-value.c).
