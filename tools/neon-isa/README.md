# tools/neon-isa: A32 Advanced SIMD (NEON), learned

`src/arch/thumb/neon.inc` -- the A32 NEON and VFP forms EmbCC's ARM
assembler encodes from a table (`src/arch/thumb/neon.c`) -- is generated
here from what llvm-mc does, as `tools/tricore-isa` generates TriCore's from
GNU binutils: llvm-mc's disassembler says what a word means, its assembler
which word a statement is. No opcode table is copied. You need `llvm-mc`
and Python 3; the data files are written where the scripts run and are not
committed.

| step | script | writes | what it learns |
|---|---|---|---|
| 1 | `sweep.py` | `groups.json` | every word of 0xF2000000-0xF4FFFFFF and of the cp10/cp11 transfers decoded; a random 100 examples of each (mnemonic, shape) |
| 2 | `learn.py` | `isa.json` | each example's bits flipped: opcode (another mnemonic or shape, or an operand moved by other than a power of two -- an immediate's cmode), operand, reserved; each operand bit's weight from the cleared word |
| 3 | `constrain.py` | `isa2.json` | the values llvm-mc's assembler takes in each field (lists that stay within d0-d31, post-index registers other than sp and pc) |
| 4 | `check.py` | `rank.json` | random fillings decoded back (the round trip), and how often llvm-mc writes each form |
| 5 | `rank.py` | `order.json` | between two forms that both hold a value (cmode classes at zero), the one llvm-mc writes |
| 6 | `synonyms.py` | `synonyms.json` | the type spellings llvm-mc takes for each other (`vadd.s32` is `vadd.i32`), checked at two fillings |
| 7 | `gen.py OUT` | `neon.inc` | the C tables |

`shapes.py` reduces operand text to a shape and values -- a register list
is one operand (its first register; the count, spacing and lanes in the
shape) -- and `oracle.py` drives llvm-mc. Two things are not in the table:
the VFP loads and stores of many registers, vldr/vstr (the hand-written
encoders have them), and `vext`'s immediate, which llvm-mc prints in
elements of the largest size dividing the byte offset (neon.c rescales).

Referees: `difftest.py EMBCC [N] [SEED] [syn]` -- random statements of
every form, with the synonym spellings when `syn` is given, assembled by
llvm-mc and EmbCC: 24,232 statements a run, no difference.
`tools/asm-audit/audit.py a7neon` -- clang's NEON-vectorized libc and
runtime for Cortex-A: 306/306 files, no difference. And
`tests/golden/arm-a32-neon.sh`.
