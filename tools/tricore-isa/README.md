# tools/tricore-isa: the TriCore instruction set, learned

`src/arch/tricore/isa.inc` -- every form of TC1.6.2 that EmbCC's TriCore
assembler (`src/arch/tricore/asm.c`) encodes -- is generated here, from what
GNU binutils for TriCore *do*, used as oracles: `tricore-elf-objdump` says
what a word means, `tricore-elf-as -mtc162` says what a statement is. No
opcode table was copied from a manual or another assembler; the table is
measured, and the measurements are rerun whenever the scripts change.

You need tricore-elf-as, -objdump, -nm, -objcopy and -readelf in
`~/cross/tricore/bin` and Python 3. The scripts write their data files
(`*.dis`, `*.json`, `groups.txt`) in the directory they run in; none is
committed -- they regenerate in about ten minutes, plus the sweeps.

## The pipeline

| step | script | reads | writes | what it learns |
|---|---|---|---|---|
| 1 | `sweep.py` | -- | `h16.dis`, `w32*.dis` | every 16-bit word and 15 million random 32-bit ones, decoded |
| 2 | `discover.py` | `isa3.json` | `disc.dis`; with `DISC_ALL=1`, `disc2.dis` | the mnemonics GNU as knows (each word of its binary it does not answer `Unknown instruction` to), each offered every known operand shape: forms the sweeps missed (`clz`, `dsync`, the `[A+r]` loads) |
| 3 | `collect.py` | the `.dis` files | `groups.txt` | examples grouped by mnemonic, operand shape (`D,[A]N`) and length |
| 4 | `learn2.py` (`learn.py`) | `groups.txt` | `isa.json` | per example, each bit flipped: an opcode bit changes the mnemonic or shape, an operand bit one operand by a power of two, a reserved bit nothing; then each operand bit's weight from the all-clear word. Learned forms' examples are removed and the rest relearned until none is left |
| 5 | `post.py` | `isa.json` | `isa2.json` | weights that are not powers of two dropped; each form's opcode bits and field mask |
| 6 | `check.py` | `isa2.json` | -- | random fillings of every form encoded and decoded back: the table against objdump (only `nop` differs: objdump folds zero words) |
| 7 | `constrain.py` | `isa2.json` | `isa3.json` | which values GNU as takes in each field: an even register for a pair, a shift of only 0 or 1 |
| 8 | `relocs.py` | `isa3.json` | `relocs.json` | a symbol (`sym`, `hi:`, `lo:`, `sm:`, `up:`, a label of another section) in every numeric field: the relocation GNU writes, where, and in which form |
| 9 | `rank.py` | `isa3.json` | `rank.json` | between two forms of one length that both fit, the one GNU takes |
| 10 | `narrow.py` | `isa3.json` | `narrow.json` | when `add d1, d1, d2` is written as the 2-operand 16-bit `add d1, d2` (and when not: `sub d1, d2, d1`) |
| 11 | `reserved.py` | `isa3.json` | `gnures.json` | what GNU writes in reserved bits (updfl's register, twice) |
| 12 | `gen.py OUT` | all of it | `isa.inc` | the C tables |

`learn.py` holds the oracle (`disasm`: each word followed by a 16-bit nop,
so an invalid one cannot swallow the next) and the operand reading shared
by the others; `formsel.py` the field solver (`fit`).

## The referees

* `difftest.py EMBCC [N] [SEED]` -- random encodings of every form decoded
  to statements, assembled by GNU as and by `embcc -c`, compared byte for
  byte. `DT_EQ=1` adds each statement with its first register repeated and
  with d15, a15 and sp in every place: the cases where GNU writes a shorter
  form (`sel d1, d15, d9, d1` is `cmov d1, d15, d9`; `lea sp, [sp]-8` is
  `sub.a sp, 8`).
* `symtest.py EMBCC` -- every symbol statement GNU takes, both assemblers:
  bytes and relocations (offset, type, symbol, addend).
* `linktest.py EMBLD` -- every symbol statement GNU takes, its symbol given
  a value its field holds, linked by tricore-elf-ld and by embld with one
  script: the same bytes. `LT_AS=EMBCC` has EmbCC assemble the object,
  and also compares the image with GNU as's.
* `tools/asm-audit/audit.py tricore` -- GCC's TriCore output for EmbCC's
  libc and runtime, both assemblers, disassembly compared.

At the time of writing: 47,478 random statements, 1,281 symbol
statements, 748 linked relocations (734 from EmbCC's objects) and the
114 files of the audit, with no difference.

## What GNU does that the table cannot say

`asm.c` holds these by hand, each measured: a branch to a label is always
its 32-bit form (`jz d2, L` is `jeq d2, 0, L`); one out of reach is the
inverse over a `j` (or, for `loop`, `jned` and `jnei`, a branch to a `j`
past a 16-bit `j` over it); `[A]0` takes the form without an offset only
when that one is shorter; `[A+]4` of a word is `[A+]`; `.code16` and
`.code32` force the next instruction's width.
