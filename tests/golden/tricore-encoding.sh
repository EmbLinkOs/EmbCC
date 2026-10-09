#!/bin/sh
# EmbCC's TriCore encoder (src/arch/tricore/emit.c), against QEMU's
# TriCore translator.
#
# There is no TriCore assembler or disassembler on this machine -- LLVM has
# no TriCore target, and QEMU prints TriCore code under -d in_asm as raw
# bytes -- so the referee is the other half of QEMU: the decoder that turns
# each instruction it executes into TCG operations, which name the
# registers and constants it found in every field. tools/tricorecheck
# writes a program that executes every form the encoder can emit -- every
# register number in every field, both ends of every immediate, branches
# and jumps at the ends of the displacements the board's RAM can hold --
# and expectations written from each instruction's OPERANDS
# (`add_i32 ?,d4,d5` then `mov_i32 d3,?` for ADD d3, d4, d5). QEMU runs it
# one instruction per translation block under -d op, and the checker
# compares the two.
#
# The program then checks the encoder at run time, with the board as the
# second referee: each tc_li and tc_li_a sequence must build the value a
# table holds, and MTCR then MFCR of a core register must give it back.
# The board's test device ends the run with the program's status -- 0, or
# the number of the check that failed.
#
# Last, every encoder range check is provoked and must stop the process
# rather than truncate.
set -u
echo "TEST-MARKER tricore-encoding"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_TRICORE)"; exit 0; }

out=tests/golden/out/tricore-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/tricorecheck" \
   tools/tricorecheck/tricorecheck.c src/arch/tricore/emit.c src/arch/tricore/asm.c \
   src/arch/code.c src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "tricorecheck did not build"; exit 1; }

"$out/tricorecheck" --image "$out/walk.elf" || {
    echo "tricorecheck could not build its walk"; exit 1; }

# The walk ends by writing its status to the test device, which ends QEMU
# with it; the timeout bounds a walk that went astray (a jump into
# unmapped memory runs on through zeros, which are NOPs).
sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-20}" \
    "$QEMU" -M tricore_testboard -cpu tc27x -display none -monitor none \
    -kernel "$out/walk.elf" -d op -accel tcg,one-insn-per-tb=on \
    -D "$out/walk.log" > "$out/qemu.txt" 2>&1
st=$?

# The decoding first: when the walk went astray, the instruction that sent
# it there is usually one QEMU decoded as something else.
"$out/tricorecheck" --check "$out/walk.log" > "$out/check.txt"
ck=$?
if [ "$ck" != 0 ]; then
    head -60 "$out/check.txt"
    exit 1
fi
if [ "$st" != 0 ]; then
    echo "the walk did not finish cleanly on the board: status $st"
    echo "(a number is the run-time check that failed: a tc_li sequence or"
    echo "an MTCR/MFCR round trip; 137 is the timeout)"
    head -5 "$out/qemu.txt"
    exit 1
fi
n=$(cat "$out/check.txt")
[ "$n" -ge 8000 ] || {
    echo "the walk is only $n instructions -- it no longer sweeps the fields"
    exit 1; }

k=$("$out/tricorecheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/tricorecheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: tricore:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n TriCore instructions decode in QEMU as their operands say, every"
echo "register in every field and both ends of every immediate; the tc_li and"
echo "MTCR/MFCR checks pass on the board; and $k encoder range checks each"
echo "refuse rather than truncate"
