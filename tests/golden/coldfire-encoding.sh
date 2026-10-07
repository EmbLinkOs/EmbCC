#!/bin/sh
# EmbCC's ColdFire encoder (src/arch/coldfire/emit.c), refereed by QEMU's
# m68k disassembler (binutils').
#
# There is no m68k assembler here -- no llvm-mc target, no binutils -- so
# the check runs the other way round from mips-encoding.sh: tools/cfcheck
# prints every form the backend can emit (each register in each field,
# every kind of effective address on each side of an instruction that has
# one, both ends of every displacement and immediate, every condition,
# branches at the ends of their reach) as the line QEMU prints for it,
# next to the bytes emit.c produced, from one call. The bytes are loaded
# into QEMU's mcf5208evb with the CPU stopped (-S), the monitor
# disassembles them (`xp/Ni`, its commands read from a file through a
# chardev, because tests/harness/qrun.sh gives QEMU no stdin), and the two
# texts are compared line by line. A field in the wrong place reads back
# as another operand; a wrong length shifts every line after it.
#
# The disassembler decodes the whole 68000 family, so it says what an
# encoding MEANS, not whether a ColdFire core has it: that the forms are
# ColdFire's is the encoder's own checks (each refused below) and the
# exec corpus on the board.
#
# Then every encoder check is provoked and must stop the process rather
# than emit a 68000 form or truncate a field.
set -u
echo "TEST-MARKER coldfire-encoding"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_M68K:-qemu-system-m68k}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_M68K)"; exit 0; }

out=tests/golden/out/coldfire-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/cfcheck" \
   tools/cfcheck/cfcheck.c src/arch/coldfire/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "cfcheck did not build"; exit 1; }

BASE=0x40000000
"$out/cfcheck" --vocab $BASE > "$out/vocab.txt" || {
    echo "cfcheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 3000 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

cut -d'|' -f1 "$out/vocab.txt" > "$out/want.txt"
cut -d'|' -f2 "$out/vocab.txt" | tr -d '\n' |
    perl -ne 'print pack("H*", $_)' > "$out/blob.bin"

# The monitor's commands from a file; -S: the CPU never runs. The blob is
# the "kernel": a raw image, loaded at the start of SDRAM.
printf 'xp/%di %s\nquit\n' "$n" "$BASE" > "$out/cmds.txt"
TMPDIR=${TMPDIR:-/tmp} sh tests/harness/qrun.sh 60 "$QEMU" \
    -M mcf5208evb -cpu m5208 -S -display none -serial null \
    -chardev file,id=mon,path="$out/qemu.raw",input-path="$out/cmds.txt" \
    -mon chardev=mon -kernel "$out/blob.bin" 2>"$out/qemu.err"
tr -d '\r' < "$out/qemu.raw" |
    sed -n 's/^0x[0-9a-f]*:  //p' > "$out/got.txt"
m=$(wc -l < "$out/got.txt" | tr -d ' ')
[ "$m" = "$n" ] || {
    echo "QEMU disassembled $m lines of the $n -- an instruction's length"
    echo "is not what the encoder thinks, or QEMU did not finish:"
    head -5 "$out/qemu.err"; exit 1; }
paste -d'|' "$out/want.txt" "$out/got.txt" |
    awk -F'|' '$1 != $2 { print "  want: " $1 "   QEMU: " $2 }' \
    > "$out/diff.txt"
if [ -s "$out/diff.txt" ]; then
    echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n encodings decode to something else:"
    head -20 "$out/diff.txt"
    exit 1
fi

k=$("$out/cfcheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/cfcheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: coldfire:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n ColdFire encodings decode as intended by QEMU's m68k"
echo "disassembler, every register in every field and both ends of every"
echo "displacement and immediate; and $k encoder checks each refuse"
