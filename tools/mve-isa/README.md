# tools/mve-isa: MVE (Helium), learned

`src/arch/thumb/mve.inc` -- the forms of ARMv8.1-M's vector extension
(MVE, Helium: Cortex-M55, M85) EmbCC's Thumb assembler encodes from a
table -- is generated here from what llvm-mc does, as `tools/neon-isa`
generates A32 NEON's: llvm-mc's disassembler says what a word means, its
assembler which word a statement is. No opcode table is copied. You need
`llvm-mc` and Python 3; the data files are written where the scripts run
(`~/EmbRef/mve-isa` by habit) and are not committed.

The steps are tools/neon-isa's (see its README); what is MVE's own:

| file | what differs |
|---|---|
| `oracle.py` | Thumb words: the first halfword in the high 16 bits, each halfword little-endian in memory. Each candidate is decoded in `[ ]` brackets of its own -- llvm-mc skips a single byte of an invalid Thumb word, which would shift every later word out of halfword alignment -- with a bracketed marker after it ending its segment |
| `sweep.py` | the Thumb coprocessor space MVE lives in, first halfwords `0xEC00`-`0xEFFF` and `0xFC00`-`0xFFFF` with every second halfword (134M words, two minutes on eight cores), keeping the groups that name a vector register |
| `shapes.py` | MVE's operands: a vector base `[q1, #8]`, a vector offset `[r0, q1, uxtw #2]`, a comparison's condition (`eq`, as `C`), `zr` |
| `constrain.py` | a form's base statement with a distinct register in each field (MVE refuses `Qd` equal to `Qn` in some), and an offset with only negative weights one step below zero (`#0` is the adding form's) |
| `complete.py` | run after `constrain.py`: a load or store the sample gave in one offset sign only gets its other sign's form -- the add bit (bit 23) set, the weights negated -- kept if llvm-mc assembles it alike |
| `gen.py` | writes `mve.inc`, which `neon.c` includes under `mve_` names: one encoder, two tables |
| `difftest.py` | the Cortex-M55 target, and Thumb words |

The order: `sweep.py`, `learn.py`, `constrain.py`, `complete.py`,
`check.py`, `rank.py`, `synonyms.py`, `gen.py ../../src/arch/thumb/mve.inc`.

The assembler's own code, not the table: a statement predicated in a VPT
block (`vaddt.i32`) is its base form's word; the reductions' `dav` names
(`vmlaldav` is `vmlalv`); `vpst` and `vpt` write their block's mask
(`vpt` is `vcmp` with the mask in the same bits); `vmov` of two lanes,
which names one register twice (the learner takes it for an opcode);
`vctp`,
the scalar shifts (`lsll`, `uqrshl`...) and `vmrs`/`vmsr` of `vpr` and
`p0` are hand-written beside ARMv8.1-M's other instructions.

Referees: `difftest.py EMBCC [N] [SEED]` -- random statements of every
form assembled by llvm-mc and EmbCC, word for word; and
`tests/golden/thumb-mve.sh`.
