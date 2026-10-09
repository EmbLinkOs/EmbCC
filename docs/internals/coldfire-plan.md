# ColdFire (m68k-none-elf): the plan

The target is Motorola/NXP ColdFire, the 68000's embedded descendant, as
in the MCF5208: 32-bit, big-endian, bare metal, soft float --
`m68k-none-elf` (also `m68k-unknown-elf`, `m68k-elf`, `m68k`) with
ColdFire semantics, `-mcpu=5208` by default. The instruction set emitted
is ISA_A with the hardware divide, which every ColdFire core since the
V2 MCF5206e executes; the 68000/68020 family proper is not a target
(`-mcpu=68000`, `-m68020` and friends are refused by name).

**Where the facts come from.** There is no m68k compiler, assembler or
disassembler on this machine apart from QEMU: LLVM 23 here is built
without its experimental M68k target (`llvm-mc --triple=m68k` and
`clang --target=m68k-unknown-elf` both say so), and there is no
m68k-elf-gcc. So, in order of trust:

1. **QEMU 11's m68k disassembler and board** (`qemu-system-m68k -M
   mcf5208evb -cpu m5208`), for every instruction encoding
   (tests/golden/coldfire-encoding.sh) and for what the code computes
   (the exec corpus on the board).
2. **The ColdFire Family Programmer's Reference Manual** (Freescale,
   CFPRM rev. 3) as I know it, for which addressing modes each
   instruction takes on ColdFire -- QEMU's decoder is more permissive
   than the silicon (it executes `add.b`, which ColdFire lacks), so the
   encoder's own mode checks are what keep the output ColdFire's.
3. **GCC's m68k port** (gcc/config/m68k: m68k.h, m68k.c, m68kelf.h) **as
   remembered, not verified** -- the calling convention, the data layout,
   the predefined macros. Every fact from here is marked *(GCC,
   unverified)*. The tests keep them self-consistent (EmbCC calling
   EmbCC); a real m68k-elf-gcc is the first thing to check them against.

## Registers

Two files of eight 32-bit registers, which is the hard part for a C
compiler:

- **d0-d7**, data registers: all arithmetic, logic, shifts, multiply and
  divide, and the only home of a value a shift or a multiply writes.
- **a0-a7**, address registers: every memory access takes its base (and
  may take its index) from one; they also add, subtract, compare and
  move (`adda`, `suba`, `cmpa`, `movea`, `lea`). a6 is the frame pointer
  (`link`/`unlk`), a7 the stack pointer.

DWARF numbers them d0-d7 = 0-7, a0-a7 = 8-15, and so does the encoder.

*(GCC, unverified)*: d0, d1, a0, a1 are call-clobbered; d2-d7 and a2-a6
are callee-saved; a6 is the frame pointer.

The backend keeps d0, d1, a0 and a1 as its scratch (and the return
registers); the allocator's data class is d2-d7, its address class a2-a5
(a pointer that is only ever added to, compared, copied and dereferenced
lives in one and is used as a base directly). Every function links a6:
its slots are a6-relative whatever the stack pointer does under an
alloca, and a debugger's frame chain is there.

## The calling convention *(GCC, unverified)*

The m68k SVR4 convention as GCC's m68k-elf implements it:

- **Every argument is on the stack**, pushed right to left so the first
  is at the lowest address: at 4(sp) on entry, 8(a6) after `link`. Each
  takes a whole number of 4-byte words (PARM_BOUNDARY 32):
  - a scalar of 4 bytes or fewer one word, PROMOTED to int: big-endian,
    a `char` argument's byte is the word's last (11(a6) for the first);
  - a `long long` or (soft) `double` two words, high word first, only
    4-aligned;
  - a structure or union BY VALUE, its size rounded up to a word; one
    smaller than a word is right-justified in it (GCC's default padding
    for a big-endian target, PAD_DOWNWARD below PARM_BOUNDARY), a larger
    one left-justified with its padding after it.
- **The caller pops** its arguments. EmbCC reserves the widest call's
  argument block once at the bottom of the frame and stores each
  argument into it, rather than pushing and popping at each call; the
  layout at the call is the same.
- **Variadic functions** take their unnamed arguments exactly as named
  ones (a `float` as a `double`), so `va_list` is a `char *` walking the
  caller's argument block.
- **Results:** an integer of 4 bytes or fewer, and a `float`, in d0; a
  `long long` or `double` in d0:d1, HIGH word in d0. A pointer comes back
  in a0 AND d0 (the SVR4 m68k rule: GCC's callee copies a0 to d0 so a
  caller without a prototype finds it): EmbCC's callee writes both and
  its caller reads d0. A `_Complex float` comes back in d0 (real) and d1
  (imaginary) -- it is not an aggregate to GCC, so pcc struct return
  does not apply, and m68k_function_value gives its mode d0 on -- and a
  `_Complex double` in d0:d1 (real) and d2:d3 (imaginary). A function
  that returns one, or calls one that does, keeps d2 and d3 out of its
  register pool, so it never saves, restores or keeps a value in them
  across that call (uses_d2d3_result).
- **Every structure and union is returned through memory**
  (DEFAULT_PCC_STRUCT_RETURN): the caller passes the buffer's address in
  **a1**, not on the stack, and the callee returns it in d0 (and a0).
- **Stack:** grows down, 4-aligned at every call (ColdFire's
  PREFERRED_STACK_BOUNDARY; the 68000 needs only 2), no red zone.

## Data layout *(GCC, unverified)*

ILP32 and big-endian, with the m68k's peculiarity: **no type is aligned
beyond 2 bytes** (BIGGEST_ALIGNMENT is 16 bits unless `-malign-int`,
which no ColdFire `-mcpu` turns on). So `int`, `long`, pointers, `float`,
`long long` and `double` are 2-aligned, in structures and as objects:
`struct { char c; int i; }` is 6 bytes and `__BIGGEST_ALIGNMENT__` is 2.
ColdFire handles a misaligned access in hardware, so nothing in the code
generator depends on alignment. `long double` is `double` (8 bytes:
LONG_DOUBLE_TYPE_SIZE is 64 on ColdFire, 80 on the 68881 parts). Plain
`char` is signed; `size_t` is `unsigned int`, `ptrdiff_t` `int`;
`wchar_t` is a signed 32-bit integer (GCC spells it `long int`, EmbCC
`int`: the same size and sign, different only to C++'s mangling). An
unnamed bit-field does not raise a structure's alignment. There is no
`__int128`. `-malign-int` and `-mshort` are refused by name.

## Instruction selection

ColdFire is the 68000 with its rarely used corners cut off, and what is
left decides the code:

- **Integer operations are .l only**: add, sub, and, or, eor, cmp, neg,
  not; a narrow value is computed in 32 bits and extended where C says
  (ext.w, ext.l, extb.l; `and.l #0xff` to zero-extend). Byte and word
  survive in move, clr, tst and the multiplies.
- **Two operands**: `op.l <ea>,Dn` computes into a data register, and the
  source may be memory -- a frame slot is an operand, not a load. The
  immediate forms (`addi`, `subi`, `andi`, `ori`, `eori`, `cmpi`) write a
  data register only; `addq`/`subq` (1-8) and `moveq` (-128..127) are
  the short ones.
- **Shifts** are register-only, by 1-8 or by a register's count mod 64;
  there are no rotates.
- **Multiply**: `muls.l`/`mulu.l` give the low 32 bits of 32 x 32 (no
  64-bit product): a 64-bit multiply is a call (`__muldi3`), as is a
  64-bit divide. **Divide**: `divs.l`/`divu.l` and `rems.l`/`remu.l` (the
  MCF5208's hardware divide); a division by zero traps, which C leaves
  undefined.
- **Moves** may not combine two long effective addresses: a source with
  a 16-bit displacement cannot go to an indexed or absolute destination,
  and an indexed, absolute or immediate source only to a register or
  (An), (An)+, -(An).
- **Comparisons** set the condition codes; a 0/1 value is `scc` (0 or
  -1 in the low byte), `extb.l` and `neg.l`.
- **Branches** are `bcc.w` (+-32 KiB) inside a function, `.b` where it
  reaches. One that does not reach takes a long form with no relocation
  -- the inverse `bcc.b` over `lea (0,%pc),%a0; adda.l #d,%a0; jmp (%a0)`
  -- and the function is generated again until nothing new fails, then
  once more with every branch that fits 8 bits short (shrinking only
  brings code closer). Calls are `jsr` to an absolute address
  (`R_68K_32`), so every call reaches. A dense `switch` is a table of
  32-bit offsets from itself after a `lea (d16,%pc)`, so it needs no
  relocation either.
- **Atomics**: ISA_A has no compare-and-swap (`cas` is not ColdFire's),
  so an atomic read-modify-write of 1, 2 or 4 bytes is a plain one with
  interrupts masked -- `move.w %sr` saved, the mask raised to 7, `move.w`
  back -- which on a single core is atomic. Moving to and from `%sr` is
  supervisor-only, where a bare-metal ColdFire program runs; in user mode
  the first atomic traps (a privilege violation) rather than run
  unprotected. 8-byte atomics are refused by name. A fence is `nop`, which
  synchronises the ColdFire pipeline.
- **`alloca`** moves `%sp` down by the size plus 15 and hands out the
  first 16-aligned address above the argument area, the IR's promise;
  the frame is a6-relative, so nothing else moves.
- **Addresses** of globals, strings and functions are 32-bit absolute
  immediates: `move.l #sym,Dn` or `lea sym,An`, `R_68K_32`.
- Soft float through lib/rt under libgcc's names.

## Relocations

RELA, as every m68k toolchain writes:

| Type | Field | Value |
| --- | --- | --- |
| `R_68K_32` (1) | a 32-bit field: an absolute address in an instruction's extension words, or a data word | S + A |
| `R_68K_16` (2) | 16 bits | S + A |
| `R_68K_PC32` (4) | 32 bits | S + A - P |
| `R_68K_PC16` (5) | 16 bits | S + A - P |

`e_machine` is `EM_68K` (4), class 32, big-endian; `e_flags` is
`EF_M68K_CF_ISA_A` (0x02), binutils' value for an ISA_A object with the
hardware divide and no MAC or FPU.

## The board: QEMU's mcf5208evb

- 128 MiB of SDRAM at 0x40000000 and 16 KiB of SRAM at 0x80000000; the
  UARTs at 0xfc060000 (UART0, the first serial port), 0xfc064000 and
  0xfc068000. `-kernel` loads an ELF and starts at its entry, with no
  stack pointer and the vector base at 0. `-cpu m5208` is ISA_A+ with
  the hardware divide and no FPU, so a floating-point instruction traps.
- **Output**: a byte written to UART0's transmit buffer (offset 0x0c)
  is printed once the transmitter is enabled (the command register at
  offset 0x08, `TC_ENABLE`).
- **Ending a run**: the harness prints `==EXIT n ==` and the runner
  (tests/harness/qrun.sh --until) stops QEMU at it.
- **Exceptions**: the harness points VBR at a table whose every vector
  reports the vector number, the faulting PC and the format word, so a
  run that faults ends without the sentinel.

## The referee for encodings

`tools/cfcheck` prints every form the encoder can emit -- each register
in each field, every kind of effective address on each side of an
instruction that takes one, both ends of every displacement and
immediate, every condition, branches at the ends of their reach -- as the
line QEMU's disassembler prints for it, next to the bytes emit.c made.
tests/golden/coldfire-encoding.sh loads the bytes into the board with the
CPU stopped and has QEMU's monitor disassemble them (`xp/Ni`), and the
texts are compared line by line: 3963 instructions. Then every encoder
check (38: a moveq out of range, a move between two long effective
addresses, `lea` of a data register, a predecrement `movem`, `eor <ea>,Dn`,
an address register under `and`, addq #0 and #9, a shift by 9, `mul.l`
of an absolute address, `div.l` of an immediate, a branch beyond 32 KiB,
...) is provoked and must stop the process. Shown to fail against five
mutants: addx's fields exchanged, the index scale's two values swapped,
lsl encoded as asl, `op.l <ea>,Dn` given the byte opmode, and the addq
range check widened.

## Status

Done -- each test shown to fail against a deliberate mutant, as the
commits say:

| Test | What it checks |
| --- | --- |
| `tests/golden/coldfire-encoding.sh` | every encoder form against QEMU's m68k disassembler (3979 instructions), and 39 encoder checks |
| `tests/golden/coldfire-exec.sh` | `tests/exec/*.c` on the mcf5208evb at -O0, -O1, -O2 and -Os: 200 of 200 at every level, 22 of them judged against clang's big-endian MIPS32 status for an LP64 or little-endian assumption, 1 against the value the m68k's 2-byte alignment gives, 16 not applicable |
| `tests/golden/coldfire-abi.sh` | caller and callee in separate units, -O0/-O2 in all four pairings, against the host's output: the shared embedded programs and the m68k-specific ones |
| `tests/golden/coldfire-refuse.sh` | the triples, the object header and relocations, the accepted and refused options and constructs, EmbLD's refusals |
| `tests/golden/coldfire-asm.sh` | the assembler: 4137 statements in every spelling read back by QEMU's m68k disassembler, the same bytes from a .s file, FreeRTOS's ColdFire V2 portasm.S, the symbol forms' relocations, inline asm, a naked function, a block and a .S file on the board at -O0..-Os against a host model, 61 statements, 11 templates and 5 files refused by name |
| `tests/golden/libc-embedded.sh` | lib/libc on the board equals x86-64's at -O0, -O2, -Os |
| `tests/golden/debug-embedded.sh` | `-g`: `llvm-dwarfdump --verify`, the frame base (breg14, a6), address size, pointer DIEs |

The exec corpus also passes with the allocator's pool cut to one
register (`EMBCC_RA_MAXPOOL=1`, every spill path) and with the address
class off (`EMBCC_CF_NOAREG=1`).

Known gaps, in the order they matter:

1. **The ABI is unverified** against a real m68k compiler: the argument
   padding of small composites, the 2-byte alignment, struct return
   through a1, `_Complex` results in d0-d3, the predefined macros
   (written by hand) and `wchar_t`'s spelling are GCC's m68k port as
   remembered. `coldfire-abi.sh` makes the convention one convention;
   only m68k-elf-gcc can say it is GCC's.
2. **Refused by name:** a frame beyond 32 KiB, unwind tables, C++ (as on
   every ILP32 target), and a scalar local aligned beyond the 4-byte
   stack. (Inline asm, file-scope asm and `.s` files were, until the
   assembler below; 8-byte atomics are now libatomic calls.)
3. **Code size**: every function links a6 and a 64-bit value always lives
   in its frame slot (there is no pair allocation); constants and
   addresses are 6-byte operands where GCC would use shorter forms; a
   64-bit shift by most constants is a call.
4. ISA_A+ and ISA_B forms (`mvs`/`mvz`, `mov3q`, `byterev`, `cmp.b/.w`)
   are not used, so the code runs on every ColdFire core with a divider.

## The assembler

src/arch/coldfire/asm.c parses GNU as's Motorola syntax (the `%` optional,
either case, MIT's `An@(d)` too) and encodes through emit.c's encoders,
with new ones for what only an assembler writes: `rte`, `stop`, `tpf`, the
moves of `%sr` (from an immediate), `%ccr` and `%usp`, `movec`, the bit
instructions and `bsr.s`. It serves inline asm (coldfire/irgen.c
substitutes `%d2`, `%a2`, `#5`, `(%a2)`, as GCC's m68k port prints them),
file-scope blocks and naked functions, and `.s`/`.S` files through
src/as/gas.c (`|` comments, `.align` in bytes, `.word` two bytes, nop
padding, `.s`/`.w` branch relaxation). The MCF5208 -- QEMU's m5208 --
traps on ISA_B's `mvs`/`mvz` and on 32-bit branches, so those are refused,
and a `bra`/`bsr` to a symbol defined elsewhere is a `jmp`/`jsr` to its
address. QEMU's disassembler is the referee (there is no m68k assembler
here): every statement of the vocabulary reads back as written, and a
program on the board matches a host model of the same computations.
Operands avoid a1 (the lowering's own), a6 and a7; a callee-saved register
an asm changes is saved by the prologue.
