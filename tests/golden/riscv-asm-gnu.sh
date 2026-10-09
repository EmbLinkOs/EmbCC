#!/bin/sh
# RISC-V assembly as GNU as reads it, as FreeRTOS's RISC-V port writes it.
#
# portASM.S (with portContext.h) needed five things this assembler and
# driver did not do:
#   - a .S file's #include searches the -I directories (its chip-specific
#     header is named by the build); the driver gave the preprocessor none;
#   - `.extern sym`, which GNU as accepts and ignores;
#   - operands with expressions and spaces: `addi sp, sp, -( 31 * 4 )`,
#     `lw x1, 1 * 4( sp )`, `0( sp )`;
#   - `lw rd, sym`, a load from a symbol: auipc rd, %pcrel_hi(sym) and
#     lw rd, %pcrel_lo(label)(rd);
#   - `csrc mstatus, 8`: a CSR operation on an immediate is the csrci form.
# The assembled bytes are compared with llvm-mc's for the same text, and
# the load from a symbol's pair and relocations with clang's at RV32 and
# RV64 (without the C extension: this assembler does not compress a .S).
set -u
echo "TEST-MARKER riscv-asm-gnu"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
command -v "$MC" >/dev/null 2>&1 || { echo "SKIP: no $MC"; exit 0; }
out=tests/golden/out/riscv-asm-gnu
rm -rf "$out"; mkdir -p "$out/inc"
fail() { echo "FAIL: $*"; exit 1; }
cat > "$out/inc/frame.h" <<'EOF'
#define WORD 4
#define CONTEXT ( 31 * WORD )
EOF
cat > "$out/body.s" <<'EOF'
addi sp, sp, -( CONTEXT )
sw x1, 1 * WORD( sp )
sw x5, ( 2 * WORD )( sp )
lw x6, 0( sp )
lw x7, (CONTEXT - WORD)(sp)
andi t0, t0, ~( 0xf )
li t1, ( 1 << 11 ) | 5
csrc mstatus, 8
csrs mstatus, 0x8
csrw mie, 0
csrrs a0, mstatus, 8
csrrw a1, mscratch, 31
addi sp, sp, CONTEXT
EOF
{ printf '%s\n' '#include "frame.h"' '.extern counter' '.text' '.globl f' 'f:'
  cat "$out/body.s"; } > "$out/f.S"
"$EMBCC" --target=riscv32-unknown-elf -I"$out/inc" -c "$out/f.S" -o "$out/f.o" ||
    fail "the .S did not assemble (its -I header, .extern, or a form)"
# llvm-mc gets the same text with the macros expanded by hand
sed -e 's/CONTEXT/( 31 * 4 )/g; s/WORD/4/g' "$out/body.s" > "$out/ref.s"
"$MC" -triple=riscv32 -mattr=-c -show-encoding "$out/ref.s" 2>&1 |
    sed -n 's/.*encoding: \[\(.*\)\]/\1/p' | tr -d ' ,' |
    sed 's/0x//g' | awk '{ s = ""; for (i = length($0) - 1; i >= 1; i -= 2) s = s substr($0, i, 2); print s }' \
    > "$out/want.hex"
"$OBJDUMP" -d "$out/f.o" | sed -n '/<f>:/,$p' |
    awk -F'\t' 'NF >= 3 { sub(/^ *[0-9a-f]+: */, "", $1); gsub(/ /, "", $1); print $1 }' \
    > "$out/got.hex"
[ -s "$out/want.hex" ] || fail "llvm-mc assembled nothing"
# `li` with a value addi holds is one instruction for both
diff "$out/want.hex" "$out/got.hex" ||
    fail "a GNU form assembled differently from llvm-mc"

# lw rd, sym: the pair and its two relocations, as clang makes them
cat > "$out/ld.S" <<'EOF'
.extern pxCurrentTCB
.text
.globl g
g:
    lw  sp, pxCurrentTCB
    lw  t0, 0( sp )
    lbu a0, flag
    ret
EOF
for xl in 32 64; do
    "$EMBCC" --target=riscv$xl-unknown-elf -c "$out/ld.S" -o "$out/ld$xl.o" ||
        fail "rv$xl: lw rd, sym"
    clang --target=riscv$xl-unknown-elf -march=rv${xl}ima -mno-relax \
        -c "$out/ld.S" -o "$out/ldc$xl.o" 2>/dev/null ||
        { echo "SKIP: clang for riscv$xl"; continue; }
    for o in ld ldc; do
        "$OBJDUMP" -dr --no-show-raw-insn "$out/$o$xl.o" |
            sed -n '/<g>:/,$p' | sed -E 's/^ *[0-9a-f]+:\s*//; s/<[^>]*>//; s/_PLT//' |
            grep -v '^$' > "$out/$o$xl.dis"
    done
    diff "$out/ldc$xl.dis" "$out/ld$xl.dis" ||
        fail "rv$xl: a load from a symbol differs from clang's"
done
# An RTOS port's floating-point context: saving and restoring fs0-fs11
# and fcsr, as FreeRTOS's RISC-V port does with an FPU. The float
# registers and rounding modes are operand WORDS here, not symbols.
cat > "$out/fp.S" <<'FPEOF'
.text
.globl save_fpu
save_fpu:
    fsd fs0, 0( a0 )
    fsd fs11, 11 * 8( a0 )
    frcsr t0
    sw t0, 96(a0)
    fld fs0, 0(a1)
    fscsr t0
    fmadd.d fa0, fa1, fa2, fa3, rtz
    fcvt.w.d a0, fa0, rtz
    ret
FPEOF
"$EMBCC" --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -c "$out/fp.S" \
    -o "$out/fp.o" || fail "an FPU context save did not assemble"
"$MC" -triple=riscv64 -mattr=+m,+f,+d -filetype=obj "$out/fp.S" -o "$out/fpm.o" ||
    fail "llvm-mc rejected fp.S"
for o in fp fpm; do
    "$OBJDUMP" -d --mattr=+f,+d "$out/$o.o" | sed -n '/<save_fpu>:/,$p' |
        awk -F'\t' 'NF >= 3 { print $2, $3 }' > "$out/$o.dis"
done
[ -s "$out/fpm.dis" ] || fail "llvm-mc's fp.S has no code"
# The object says which ISA it is for (.riscv.attributes), as a compiled
# one does: a disassembler told nothing decodes no F instruction.
"$OBJDUMP" -d "$out/fp.o" | grep -q 'fsd	fs0' ||
    fail "fp.o carries no .riscv.attributes naming F and D (fsd is <unknown>)"
diff "$out/fpm.dis" "$out/fp.dis" || fail "an F/D form assembled differently from llvm-mc"
"$EMBCC" --target=riscv32-unknown-elf -c "$out/fp.S" -o "$out/fp32.o" 2> "$out/fp32.err" &&
    fail "fsd assembled for rv32imac, which has no D"
grep -q 'fsd needs the D extension' "$out/fp32.err" ||
    fail "the refusal of fsd without D does not say why: $(head -1 "$out/fp32.err")"

echo "riscv-asm-gnu: a .S with an -I header, .extern, expressions, spaced addresses, csr immediates, loads from symbols and an FPU context save assemble as llvm-mc and clang do"
