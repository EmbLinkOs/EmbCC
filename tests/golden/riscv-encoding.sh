#!/bin/sh
# Every RISC-V encoder in src/arch/riscv/emit.c, round-tripped through a
# real disassembler (D-016).
#
# tools/riscvcheck emits one instruction per call and prints the intended
# disassembly beside it; this diffs the two. A backend that assembles its
# own instructions has no assembler to catch a wrong bit, and reading the
# encoding tables twice is not a substitute -- the Thumb encoder shipped
# three wrong bits past careful reading and this check found all three.
#
# Three things are checked, and they are different claims:
#   1. each encoding disassembles as the instruction it was meant to be;
#   2. rv_li's sequences COMPUTE the constant asked for, which disassembly
#      cannot say -- it is checked by executing them (riscvcheck --li*);
#   3. every range check in the encoder actually fires.
set -u
echo "TEST-MARKER riscv-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$MC" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc not found (set EMBCC_LLVM_MC)"; exit 0; }

out=tests/golden/out/riscv-encoding
rm -rf "$out"; mkdir -p "$out"

# src/arch/target.c joined the list when emit.c learned to compress:
# rv_w() asks target_xlen() which short form applies. sema/type.c and
# ldfloat.c come with target.c.
cc -std=c99 -Wall -Wextra -o "$out/riscvcheck" \
   tools/riscvcheck/riscvcheck.c src/arch/riscv/emit.c \
   src/arch/code.c src/arch/target.c src/driver/util.c src/driver/diag.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_posix.c || {
    echo "riscvcheck did not build"; exit 1; }

# 1. The encodings. llvm-mc --disassemble rather than llvm-objdump: an
#    object assembled from bare .byte carries no .riscv.attributes, and
#    without it objdump declines to decode and prints .word for every
#    instruction. Feeding it the bytes directly also means the branch
#    displacements come back as NUMBERS rather than as resolved addresses,
#    which is the more useful thing to check -- it is the value rv_patch_b
#    was asked for.
check_one() {
    "$out/riscvcheck" "--rv$1" > "$out/bytes.bin" 2> "$out/want.txt" || {
        echo "riscvcheck --rv$1 exited nonzero:"; tail -2 "$out/want.txt"
        exit 1; }
    od -An -v -tx1 "$out/bytes.bin" | tr -s ' ' '\n' | sed '/^$/d' |
        sed 's/^/0x/' | tr '\n' ' ' > "$out/bytes.hex"
    "$MC" -triple="riscv$1" -mattr=+m --disassemble < "$out/bytes.hex" \
        2> "$out/mc.err" | tr '\t' ' ' | tr -s ' ' |
        sed 's/^ //; s/ $//' > "$out/got.txt" || {
        echo "llvm-mc could not disassemble the emitted bytes:"
        head -5 "$out/mc.err"; exit 1; }
    if ! diff -u "$out/want.txt" "$out/got.txt" > "$out/diff.txt"; then
        echo "rv$1 — an encoding is not the instruction it was meant to be:"
        head -40 "$out/diff.txt"
        exit 1
    fi
    echo "$(wc -l < "$out/want.txt" | tr -d ' ') rv$1 encodings disassemble as intended"
}
check_one 32
check_one 64

# 2. rv_li, checked by running what it emits. Disassembly could only say
#    that each instruction is the one intended; this says the sequence
#    leaves the register holding the value that was asked for, over both
#    widths' edge cases and 20000 pseudo-random values each. It also
#    checks rv_li_len agrees with what was emitted, which is what the
#    codegen will size a branch from.
"$out/riscvcheck" --li32 || { echo "rv_li is wrong at RV32"; exit 1; }
"$out/riscvcheck" --li64 || { echo "rv_li is wrong at RV64"; exit 1; }

# 3. The refusals. Every range check guards a field narrower than the C
#    type its caller passes, so a truncated value would assemble into a
#    real instruction pointing somewhere else. A check nobody has seen
#    fire is not known to work.
n=$("$out/riscvcheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$n" ]; do
    if "$out/riscvcheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: riscv:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done
echo "$n encoder range checks each refuse rather than truncate"
