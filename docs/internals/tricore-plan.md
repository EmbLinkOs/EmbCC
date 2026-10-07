# TriCore (AURIX, tricore-none-elf): the plan

The target is Infineon's TriCore, the core of the AURIX automotive
microcontrollers (TC2xx and TC3xx): 32-bit, little-endian, bare metal,
`tricore-none-elf` (also `tricore-elf`, `tricore-unknown-elf`, `tricore`).
The instruction set emitted is TriCore 1.6.1 -- what a TC2xx core
executes, and a subset of a TC3xx's 1.6.2 -- with soft float.

**Where the facts come from.** There is no TriCore compiler, assembler or
disassembler on this machine: LLVM has no TriCore target, there is no
tricore-gcc, and QEMU's `-d in_asm` prints TriCore code as raw bytes. So
the sources are, in order of trust:

1. **QEMU 11's TriCore translator and board** (`qemu-system-tricore -M
   tricore_testboard`), for every instruction encoding and its semantics.
   This is the referee (tests/golden/tricore-encoding.sh, below), and the
   board runs every program the tests compile.
2. **The TriCore 1.6 architecture manual** (Infineon, "TriCore TC1.6P &
   TC1.6E Core Architecture", volumes 1 and 2) as I know it, for what QEMU
   does not show: the context-save areas, the call depth counter, the
   CSFR numbers.
3. **The TriCore EABI** (Infineon, "TriCore Embedded Applications Binary
   Interface", v2.x) and the GCC port's behaviour **as remembered, not
   verified** -- the calling convention, the data layout, the predefined
   macros and the relocation numbers. Every ABI fact below that comes from
   here is marked *(EABI, unverified)*. The tests keep these
   self-consistent -- EmbCC calling EmbCC, compiled into one image -- and
   they are the first thing to check against a real reference compiler
   (HighTec or TASKING) when one is available.

## Registers

Two files of sixteen 32-bit registers, which is the hard part for a C
compiler:

- **D0-D15**, data registers: all arithmetic, comparisons and shifts. An
  even/odd pair is E0, E2, ... E14 (low word in the even register): 64-bit
  results of `MUL`, `DIV`, and the 64-bit arguments and results.
- **A0-A15**, address registers: every load and store takes its base from
  one. A10 is the stack pointer, A11 the return address. A0, A1, A8 and A9
  are the system's global address registers (small-data bases, the OS) and
  EmbCC never touches them.

**The hardware saves registers at a call.** `CALL` stores the *upper
context* -- PCXI, PSW, A10-A11, D8-D11, A12-A15, D12-D15 -- into a
context-save area (CSA) taken from a free list, and `RET` restores it.
So D8-D15 and A10-A15 survive any call without the compiler saving
anything, and a function may use them freely: they are "callee-saved" at
no cost, and there is no prologue or epilogue save code at all. The stack
pointer itself is restored by `RET`, so a function never restores A10.
The *lower context* -- D0-D7, A2-A7 -- is the caller's to lose.

The CSA free list (FCX, LCX) must be built before the first `CALL`; the
boot code does it (the harness's, and EmbLD's `-Tstack` stub, below).
PSW.CDC, the call-depth counter, is set to 0x7f (counting off) at boot, or
the 64th nested call would trap.

## The pointer/integer question

EmbIR does not distinguish pointers from integers: a vreg is a 32-bit
value. TriCore needs addresses in A registers to load or store through
them and passes pointer *arguments* in A4-A7 where integers go in D4-D7.
The decision:

- **Every vreg lives in a data register (or a frame slot).** The register
  allocator's pool is D registers only. Address registers are scratch:
  a load or store moves its address into one (`MOV.A`) just before the
  access, or loads it there straight from the address's frame slot
  (`LD.A`). The frame is addressed from A10 directly, and a global's
  address is built with `MOVH`/`ADDI` (a data register) or
  `MOVH.A`/`LEA` (an address register).
- **The calling convention is decided by the C types**, which irgen
  still has: each argument carries its type (`ir_arg.ty`), each parameter
  its type (`param_abi[k].ty`), the function its return type
  (`ret_abi.ty`), and each call whether its result is a pointer
  (`ir_ins.ret_ptr`, set on TriCore only). At a call a pointer argument
  is moved into its A register; in a prologue a pointer parameter is
  moved out of its A register; a pointer result is read from A2. A
  parsed IR, which carries no types, places every argument as an integer.

This costs one `MOV.A` per access through a pointer held in a register.
Allocating pointer-typed vregs to address registers is the obvious next
step for code size and is not done yet.

## The calling convention *(EABI, unverified)*

- **Arguments**, left to right:
  - a pointer goes in the next free of A4-A7;
  - any other scalar of 32 bits or fewer in the next free of D4-D7;
  - a 64-bit scalar (`long long`, `double`) in the next free even pair,
    E4 (D4:D5) or E6 (D6:D7), low word in the even register; a register
    skipped to reach an even pair is filled by a later 32-bit argument;
  - a structure or union of 8 bytes or fewer as an integer of its size
    (1-4 bytes in a D register, 5-8 in an E pair), its bytes in memory
    order from the low end;
  - a larger structure or union BY REFERENCE: the caller copies it into
    its own frame and passes the copy's address as a pointer argument;
  - anything that finds no register goes on the stack: in order, each in
    whole 4-byte words at a 4-byte-aligned offset from the caller's SP
    (8-byte values are 4-aligned on the stack too).
- **Variadic functions:** the named arguments as above; every unnamed
  argument on the stack, after any named ones there. So `va_list` is a
  bare `char *` walking the caller's outgoing area, and `va_arg` steps it
  in whole words. A `float` arrives as a `double`; a large struct as its
  address.
- **Results:** a pointer in A2; another scalar of 32 bits or fewer in D2;
  a 64-bit scalar in E2 (D2:D3); a structure or union of 8 bytes or fewer
  in D2 or E2 like an argument; a larger one through a hidden pointer the
  caller passes as the first pointer argument (A4), which moves the
  pointer arguments along by one.
- **Stack:** grows down, 8-byte aligned at every call. No home area, no
  red zone, no frame pointer.

## Data layout *(EABI, unverified, except where QEMU decides nothing)*

ILP32: `int`, `long` and pointers are 4 bytes; `long long` and `double`
are 8 bytes and **4-aligned** -- the EABI aligns nothing beyond a word,
because a TriCore doubleword access needs only word alignment. `long
double` is `double`. Plain `char` is **signed**; `wchar_t` is a signed
`int`; `size_t` is `unsigned int`. An unnamed bit-field does not raise a
structure's alignment. There is no `__int128`.

## Instruction selection

The 16-bit forms are emitted wherever one says the same as the 32-bit
instruction (mov, mov.a, mov.d, mov.aa, mov of -8..7, the loads and stores
at offset 0 that have one, RET; emit.h's tc_set_short); otherwise these
32-bit formats: RR, RR2, RC, RLC, RRR, RRR2, RRRR,
RRPW, RCPW, BOL, BRR, BRC, B and SYS. Everything is a data-register
operation except addressing:

- **Constants:** `MOV` (signed 16), `MOV.U` (unsigned 16), `MOVH` (high
  half), else `MOVH` + `ADDI` of the sign-extended low half (the high half
  rounded by 0x8000).
- **Comparisons** compute 0/1 directly (`EQ`, `NE`, `LT`, `LT.U`, `GE`,
  `GE.U`, with a register or a 9-bit constant); `SEL` is a select.
- **Branches** compare two data registers, or one with a 4-bit constant
  (`JEQ`, `JNE`, `JLT(.U)`, `JGE(.U)`), +-32 KiB. One that does not reach
  becomes the inverse branch over a `J` (+-16 MiB), and the function is
  generated again until nothing new fails.
- **Shifts:** `SH`/`SHA` shift left by a positive count and right by a
  negative one, so a variable right shift negates its count first (`RSUB`).
- **Multiply and divide:** `MUL` (32), `MUL`/`MUL.U` into an E pair (64),
  `MADD`; TriCore 1.6's `DIV`/`DIV.U` give quotient and remainder in an E
  pair and never trap.
- **64-bit add/subtract** are `ADDX`/`ADDC` and `SUBX`/`SUBC` through the
  PSW carry; 64-bit shifts use `DEXTR`, the funnel shift.
- **Memory:** the long-offset (BOL) loads and stores, a signed 16-bit
  offset from an address register. A load or store that irgen cannot
  promise is aligned (a packed member) goes byte by byte: TriCore traps a
  word access that is not halfword-aligned.
- **Calls:** `CALL` with a 24-bit displacement (`R_TRICORE_24REL`), `CALLI`
  through an address register; `RET`. Soft float and 64-bit division call
  lib/rt under libgcc's names.

## Relocations *(EABI numbering, unverified)*

| Type | Field | Value |
| --- | --- | --- |
| `R_TRICORE_32ABS` (2) | a data word | S + A |
| `R_TRICORE_24REL` (3) | `CALL`/`J` disp24 (halfwords) | (S + A - P) >> 1 |
| `R_TRICORE_HI` (6) | RLC const16 (`MOVH`, `MOVH.A`) | (S + A + 0x8000) >> 16 |
| `R_TRICORE_LO` (7) | RLC const16 (`ADDI`) | (S + A) & 0xffff |
| `R_TRICORE_LO2` (8) | BOL off16 (`LEA`, a load, a store) | (S + A) & 0xffff |

RELA, as every TriCore toolchain writes. `e_machine` is `EM_TRICORE` (44),
`e_flags` `EF_EABI_TRICORE_V1_6_1` *(unverified value)*.

## The board: QEMU's tricore_testboard

Read off `qemu-system-tricore -M tricore_testboard` (QEMU 11.1, `info
mtree`): 2 MiB of RAM at 0x80000000 ("ext_cram", where code goes), 4 MiB
at 0xa1000000 ("ext_dram": data, bss, stack), 48 KiB at 0xd0000000 and
0xd4000000, and a **test device at 0xf0000000**: a word written there ends
QEMU with that word as its exit status. `-kernel` loads an ELF and starts
at its entry. `-cpu tc27x` is a TriCore 1.6.1 core (the default, tc1796,
is 1.3 and lacks `DIV`).

- **CSAs** must lie in the first 4 MiB of a 256 MiB segment (a link word
  holds address bits 31:28 and 21:6), which rules out 0xa1000000; the
  harness puts them in the top half of the code RAM.
- **Output.** The board has no UART. The harness's `putchar` stores each
  byte to a word at the top of the internal data RAM (0xd000bff0) that
  nothing else uses, and a TCG plugin (tests/harness/tricore/putc.c,
  built with the host cc against QEMU's qemu-plugin.h) instruments only
  the byte stores and prints each one to that address, unbuffered.
- **Ending a run.** The harness prints `==EXIT n==` and then writes n to
  the test device, which exits QEMU at once; tests/harness/qrun.sh's
  `--until` and timeout bound a run that never gets there. An address
  QEMU has no memory at reads as zeros, which are NOPs, so a wild jump
  runs on until the timeout.
- **Traps** go to BTV (0 at reset, where there is no memory); the harness
  points it at a table of eight vectors, written as words at start-up,
  that load the class, the TIN (D15) and the trapping address (A11) into
  D4-D6 and jump to a C reporter, which ends the run without the sentinel.
- **Memory map of an image** (tests/harness/tricore/link.sh): the text at
  0x80000000; .data and .bss at 0xa1000000 (copied there by the startup),
  the stack growing down from 0xa1400000; the context-save areas in the
  top 512 KiB of the code RAM (8192 contexts: a link word can name only
  the first 4 MiB of a segment, which rules out 0xa1000000). Data beside
  code is slow under QEMU: a store to a page of translated code
  invalidates it, and one fuzz program ran ten times slower that way.

## The referee for encodings

`tools/tricorecheck` writes a program that executes every form the
encoder emits -- every register number in every field, both ends of every
immediate, branches and jumps at the ends of the displacements the board's
RAM holds -- and, for each, what its OPERANDS say QEMU must decode it to,
as TCG operations: ADD d3, d4, d5 is `add_i32 ?,d4,d5` then `mov_i32
d3,?`. QEMU runs it one instruction per translation block under `-d op`;
each branch is placed out of line with both its fall-through and its
target continuing the walk. The program then checks `tc_li` and the CSFR
numbers at run time and stops the board through the test device. Every
encoder range check is provoked too. The SYS forms that translate to no
operation (`NOP`, `ISYNC`, `DSYNC`) are checked only for not being an
illegal instruction.

## Status

Done (each test shown to fail against deliberate mutants, as the commits
say):

| Test | What it checks |
| --- | --- |
| `tests/golden/tricore-encoding.sh` | every encoder form against QEMU's translator (8717 instructions), `tc_li` and the CSFR numbers on the board, 39 range checks |
| `tests/golden/tricore-exec.sh` | `tests/exec/*.c` on the board at -O0, -O1, -O2 and -Os: 196 of 196 at every level, 13 of them judged against clang's MIPS32 status for an LP64 assumption, 18 not applicable |
| `tests/golden/tricore-abi.sh` | caller and callee in separate units at -O0/-O2, all four pairings, against the host's output |
| `tests/golden/tricore-asm.sh` | the inline-asm vocabulary on the board, and its refusals |
| `tests/golden/tricore-atomics.sh` | one- and two-byte atomics (the CMPSWAP.W loop on the word around them) on every lane of a word at every level, against the host |
| `tests/golden/tricore-refuse.sh` | the object header and relocations, the accepted and refused options, constructs and links |
| `tests/golden/libc-embedded.sh` | lib/libc's output on the board equals x86-64's at -O0, -O2, -Os |
| `tests/golden/debug-embedded.sh` | `-g`: the frame base (breg26, A10), address size, pointer DIEs |

The exec corpus also passes with the allocator's pool cut to two
registers (`EMBCC_RA_MAXPOOL=2`), which drives every spill path; a fuzz
campaign (tools of the other targets' campaigns: 440 gen2 and 100 gen
programs, each at -O0, -O2 and -Os, against the host's checksum) found no
miscompile; and the whole `make test` passes (514 of 514).

Known gaps, in the order they matter:

1. **The ABI is unverified.** The argument placement (especially the
   back-fill of a skipped register, structs by reference, unnamed
   variadic arguments on the stack), the 4-byte alignment of `long long`
   and `double`, the relocation numbers, `e_flags` and the predefined
   macros are the TriCore EABI and GCC for TriCore as remembered. The
   tests make the convention one convention (`tricore-abi.sh`); only a
   reference compiler (HighTec GCC, TASKING) can say it is THE
   convention, and a change made on both sides alike passes every test
   here.
2. **Code size.** lib/libc's text is 98874 bytes (MIPS32: about 137 KB
   with rodata). The 16-bit forms with implicit D15/A15 or offsets, and
   16-bit branches, are not used yet; pointers are never allocated to
   address registers, so an access through a pointer in a register costs
   a `mov.a` (2 bytes) each time.
3. **Refused by name:** jump tables (a dense switch is a compare tree),
   `.s` files and instructions in file-scope asm, naked and interrupt
   functions, atomics wider than a word, computed
   goto, `__builtin_frame_address`/`__builtin_return_address`, C++.
4. The TC3xx FPU is not used (soft float everywhere).
