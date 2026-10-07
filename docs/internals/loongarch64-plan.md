# LoongArch64 (LP64S, bare metal): the plan

The target is 64-bit little-endian LoongArch, LA64, the LP64S calling
convention (soft float): `loongarch64-unknown-elf`. The board it is
tested on is QEMU's `virt` (`qemu-system-loongarch64 -M virt`). Every
fact below was read off clang 23 (`--target=loongarch64-unknown-elf
-msoft-float`, which is `-mabi=lp64s -mfpu=none`), llvm-mc or QEMU 11.1
rather than remembered; there is no LoongArch gcc on this machine.

## Why the RISC-V backend is the model

LoongArch's base integer ISA and its LP64 calling convention are, rule
for rule, RISC-V's RV64 LP64 ones under different encodings. Read off
clang for both triples:

- a0-a7 carry arguments and a0-a1 results; a scalar of two registers
  (`__int128`, `long double`) is passed in a pair with no alignment rule
  when named, and in an EVEN-aligned pair when variadic (`v(0, x)` with
  an `__int128` x skips a1 and uses a2:a3) -- the same asymmetry as
  RISC-V;
- a 2*GRLEN scalar with one register left goes half in a7, half on the
  stack;
- an aggregate of at most 16 bytes travels packed in up to two
  registers, splitting across a7 and the stack like anything else; a
  larger one goes BY REFERENCE to a copy the caller owns;
- a struct result of at most 16 bytes comes back packed in a0:a1, a
  larger one through a hidden pointer in a0;
- a 32-bit value in a 64-bit register is kept SIGN-EXTENDED, `unsigned`
  included (`f(unsigned)` loads its argument with `ld.w`, not `ld.wu`),
  and the `.w` instructions maintain it on the way out;
- soft float: a float or double is its bits in a GPR, and every operation
  is a libgcc-named call (`__adddf3`, `__extendsfdf2`, ...);
- `va_list` is a bare `void *`, 8 bytes; the stack is 16-aligned.

So `src/arch/loongarch/codegen.c` is RV64's code generator
(`src/arch/riscv/codegen.c`) taken as a COPY, with the RV32-only machinery
removed (register pairs, 64-bit-at-RV32 lowering, RV32's long double, the
C extension) and every instruction re-selected for LoongArch. Sharing the
file was considered and not done: the two would have to agree about
immediates that are signed on one and unsigned on the other, branch
reaches that differ by a factor of 32, two different PC-relative
relocation schemes and the compressed-instruction machinery -- each an
`if` in code that the RISC-V goldens and the exec boards prove today. A
copy cannot destabilise RISC-V; a shared file with a second machine
threaded through it could.

## Registers (psABI)

| Register | Name | Role |
| --- | --- | --- |
| r0 | zero | always 0 |
| r1 | ra | return address |
| r2 | tp | thread pointer (never touched) |
| r3 | sp | stack pointer |
| r4-r11 | a0-a7 | arguments; a0-a1 results |
| r12-r20 | t0-t8 | temporaries (caller-saved) |
| r21 | -- | RESERVED by the psABI; EmbCC never names it |
| r22 | fp (s9) | frame pointer / callee-saved |
| r23-r31 | s0-s8 | callee-saved |

EmbCC's use: t0, t1, t2, t4, t5 and t6 are the code generator's scratch
(the RV64 backend's t0-t2/t4-t6 roles), so the allocator's pool is
a0-a7, t3, t7, t8 (caller-saved first) and fp, s0-s8 (callee-saved) --
21 registers, as on RV64. A function with a variable-length array
addresses its frame from fp.

## Data model

LP64: `int` 4, `long` and pointers 8, `long long` 8, `__int128` 16;
`long double` is IEEE binary128 (16 bytes, `__LDBL_MANT_DIG__` 113),
done by lib/rt's soft-float helpers as at RV64. Plain `char` is SIGNED
(clang defines no `__CHAR_UNSIGNED__` here, unlike RISC-V); `wchar_t` is
a signed `int`. An unnamed bit-field does not raise a structure's
alignment (`struct { char c; int :0; char d; }` is 5 bytes, aligned 1).
`__BIGGEST_ALIGNMENT__` is 16.

## Instructions

Every instruction is one 32-bit little-endian word. The differences from
RISC-V that the selection has to respect:

- **Immediates.** `addi.w/.d`, `slti`, `sltui` and the loads and stores
  take a SIGNED 12-bit field; `andi`, `ori` and `xori` take an UNSIGNED
  one (0..4095). A RISC-V `andi rd, rs, -16` or `xori rd, rs, -1` has no
  direct counterpart: `not` is `nor rd, rs, zero`, and an AND with a
  negative constant goes through a register (or `bstrpick`/`bstrins`).
- **Constants** are built as LLVM's LoongArchMatInt builds them -- `ori`
  or `addi.w` for 12 bits, `lu12i.w` + `ori` for 32, then `lu32i.d` (bits
  51:32, sign-extended from 51) and `lu52i.d` (bits 63:52) only where the
  sign extension of what is below does not already give them.
- **32-bit division.** `div.w`, `mod.w`, `div.wu` and `mod.wu` are
  UNDEFINED when an operand is not the sign extension of its low 32 bits
  (clang emits `addi.w r, r, 0` before them); the code generator reads
  their operands through `rd32` for that reason. `add.w`, `sub.w`,
  `mul.w` and the `.w` shifts read only the low 32 bits.
- **Comparisons** are `slt`/`sltu`/`slti`/`sltui`, as RISC-V's; `sltui`
  sign-extends its immediate and compares unsigned.
- **Branches** compare two registers: `beq/bne/blt/bge/bltu/bgeu rj, rd`
  reach +-128 KiB, `beqz/bnez rj` +-4 MiB, `b`/`bl` +-128 MiB -- each 32
  times RISC-V's reach. A function is generated with every branch in its
  short form; one that does not reach is made the inverse branch over a
  `b`, and the function generated again until every branch fits.
- **Select** has `maskeqz`/`masknez`; zero and sign extension have
  `bstrpick.d rd, rj, 31, 0`, `ext.w.b`, `ext.w.h` and `addi.w rd, rj, 0`.
- **Unaligned access** to ordinary memory is permitted (clang reads
  `p[0] | p[1] << 8` of a `char *` with one `ld.hu`); only the atomics
  require natural alignment.
- **Atomics.** `amswap_db`, `amadd_db`, `amand_db`, `amor_db`,
  `amxor_db` at .w and .d (full barrier), `ll`/`sc` for the rest; `sc`
  writes 1 on SUCCESS (RISC-V's writes 0). An `am*` instruction's rd may
  not be its rj or rk. There is no byte or halfword form in the base ISA,
  so a one- or two-byte atomic is refused by name, as on RISC-V, and
  `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1/_2` are left out of the predefined
  macros. `dbar 0` is the full fence.
- **Traps.** `break 0` is IR_UD2.

## Code model and relocations

EmbCC emits the NORMAL code model, PC-relative and not PIC:

| Use | Instructions | Relocations |
| --- | --- | --- |
| a call | `bl sym` | `R_LARCH_B26` (+-128 MiB) |
| a tail call | `b sym` | `R_LARCH_B26` |
| an address | `pcalau12i rd, %pc_hi20(sym)` + `addi.d rd, rd, %pc_lo12(sym)` | `R_LARCH_PCALA_HI20`, `R_LARCH_PCALA_LO12` |
| a pointer in data | `.dword sym` | `R_LARCH_64` |

Unlike RISC-V's `%pcrel_lo`, the low half names the SYMBOL, not the
high half's instruction: `pcalau12i` yields the 4 KiB PAGE of
`pc + (hi20 << 12)`, and the low half is the symbol's offset in its page.
The linker computes hi20 as `((S + A + 0x800) & ~0xfff) - (P & ~0xfff)`
shifted right 12, and lo12 as `(S + A) & 0xfff`, the +0x800 because
`addi.d` and the loads sign-extend lo12.

clang 23 itself defaults to the MEDIUM model (`pcaddu18i ra` + `jirl`,
`R_LARCH_CALL36`) and reaches an external global through the GOT
(`R_LARCH_GOT_PC_HI20/LO12`) even with `-fno-pic`, each with an
`R_LARCH_RELAX` beside it. EmbLD links all of those, so clang-built
objects and libraries mix with EmbCC's: CALL36 is patched in place, a
GOT access is rewritten to the direct `pcalau12i`/`addi.d` pair (the
linker relaxation GNU ld and lld perform when the symbol is in the
image, which on bare metal it always is), and RELAX is a hint.

`e_machine` is `EM_LOONGARCH` (258), ELFCLASS64, little-endian, RELA;
`e_flags` 0x41 = `EF_LOONGARCH_ABI_SOFT_FLOAT | EF_LOONGARCH_OBJABI_V1`,
what clang writes for `-mabi=lp64s`.

## Predefined macros

clang's for `--target=loongarch64-unknown-elf -msoft-float` (generated
by tools/gen-predef.sh): `__loongarch__`, `__loongarch64`,
`__loongarch_grlen 64`, `__loongarch_frlen 0`, `__loongarch_soft_float`,
`__loongarch_lp64`, `__loongarch_arch "loongarch64"`. Without
`-mfpu=none` clang would claim `__loongarch_frlen 64` and LSX
(`__loongarch_sx`), hardware the soft-float code never uses.

## The board: QEMU `virt`

- `-kernel` loads an ELF at its physical address (the high bits of a
  `0x9000...` kernel address are ignored, too) and starts it in direct
  address mode, so addresses equal physical ones. QEMU keeps its boot
  information at 0-0x100000 and the device tree at 0x100000-0x200000
  (`info roms`), and refuses an image that overlaps them -- which one
  linked at 0x200000 did as soon as embld put its ELF header in the first
  page -- so the harness links at 0x1000000 (16 MiB) with the stack's top
  at 0x3000000, in the low 256 MiB of RAM.
- **Faults.** EmbCC has no LoongArch inline assembler yet, so the
  harness writes the privileged instructions it needs (`csrwr a0,
  EENTRY`, `csrrd a0, ESTAT/ERA/BADV`, and the exception entry, which must
  be 4 KiB-aligned) as words into a page of RAM and calls them there. An
  exception prints `==FAULT ecode n ...==` and powers off, so a run that
  faults never reports an exit status. The FPU is disabled at reset, so a
  floating-point instruction in soft-float code faults too.
- **Output.** A 16550 UART at 0x1fe001e0 (the first serial port, so the
  board runs with `-serial stdio`).
- **Ending a run.** The ACPI GED's registers sit at 0x100e001c (`info
  mtree`): writing 0x34 (`SLP_TYP` S5 | `SLP_EN`) to the sleep-control
  byte powers the machine off and QEMU exits at once; 0x42 to the reset
  byte at 0x100e001e resets it, which `-no-reboot` turns into an exit.
  The harness prints a sentinel `==EXIT n ==` first, and the runner
  (tests/harness/qrun.sh --until) stops at it in any case.

## Status

Done, each committed and pushed:

1. This plan; the encoder and its referee (tests/golden/loongarch-encoding.sh:
   1669 forms against `llvm-mc -show-encoding`, 6000 constants against
   llvm-mc's own `li.d` expansion and an interpreter, 26 range checks).
2. The target (`loongarch64-unknown-elf`, aliases `-none-elf`, `-elf`,
   bare `loongarch64`), its data model and predefined macros, the code
   generator at -O0 and -O1/-O2/-Os with the register allocator, `-g`
   (frame base `DW_OP_breg3`, `breg22` with alloca), C++ with
   `-fno-exceptions`.
3. EmbLD for EM_LOONGARCH (EmbCC's and clang's objects), lib/rt and
   lib/libc (`make rt-embedded libc-embedded`), the QEMU virt harness.
4. Goldens: loongarch-exec (the exec corpus, 208 of 208 at every level,
   6 not applicable), loongarch-abi (EmbCC and clang calling each other:
   the shared embedded pair, the 128-bit pair and the LP64S pair),
   loongarch-refuse; and LoongArch64 joined predef, libc-embedded,
   debug-embedded and embedded-runtime.

Not done, refused by name meanwhile:

- inline assembly, `.s`/`.S` files, file-scope instructions and naked
  functions (no LoongArch assembler vocabulary);
- one-, two- and sixteen-byte atomics (no such am*/ll/sc in the base ISA;
  a masked ll.w/sc.w loop, as clang emits, would lift the first two);
- `__builtin_frame_address`/`__builtin_return_address` (no frame-pointer
  chain), computed goto, interrupt functions, unwind tables and C++
  exceptions, a scalar local aligned above 16;
- the LP64D/LP64F hard-float conventions and the FPU (`-mabi=lp64d`), the
  extreme code model, LSX/LASX, TLS beyond one shared instance.
