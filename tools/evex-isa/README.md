# tools/evex-isa: AVX-512 (EVEX) and the opmask instructions, learned

`src/arch/x86_64/evex.inc` -- the AVX-512 forms and the opmask
instructions EmbCC's x86-64 assembler (`src/arch/x86_64/att.c`) encodes
from a table -- is generated here from llvm-mc, as `tools/neon-isa` learns
NEON. x86's encodings are regular where it matters: this measures, per
form, what the prefix holds and where each operand goes, and the encoder
builds the EVEX or VEX prefix, the ModRM, SIB and displacement itself.

| step | script | writes | what it learns |
|---|---|---|---|
| 1 | `sweep.py` | `forms.json` | every EVEX form llvm-mc decodes: each opcode map, pp, W, vector length, b bit and opcode, with every ModRM /digit as a register and as `(%rax)`; each compare predicate (the vcmp/vpcmp aliases); gathers and scatters with a mask and a vector index. Each candidate is followed by fifteen nops and a ud2, so a short decode cannot swallow the next |
| 2 | `sweep_k.py` | `kforms.json` | the VEX forms of every v and k mnemonic: the opmask instructions, and the VEX encodings llvm-mc prefers when no operand needs EVEX |
| 3 | `roles.py` | `evex.json` | per form, assembled by llvm-mc with a distinct register in each operand: map, pp, W, L'L, b, opcode, a ModRM reg extension, which field (ModRM reg or rm, vvvv, aaa) holds each operand, an immediate the shape does not show (a predicate), the displacement scale (disp8*N, probed), and whether it takes a mask and {z} |
| 4 | `gen.py OUT` | `evex.inc` | the C table, a form's VEX encoding before its EVEX one |

Referees: `difftest.py EMBCC [N] [SEED]` -- random registers (0-31),
masks, {z}, broadcasts, rounding, memory with random base, index, scale
and displacements about the disp8*N boundary, gathers' vector indexes:
15,418 statements a run, no difference from llvm-mc.
`tools/asm-audit/audit.py x86v4` -- clang's `-march=x86-64-v4` libc and
runtime: 310/310 files, no difference. And
`tests/golden/x86-avx512-asm.sh`.
