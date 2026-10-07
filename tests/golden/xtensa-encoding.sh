#!/bin/sh
# EmbCC's Xtensa encoder (src/arch/xtensa/emit.c), refereed by QEMU's own
# disassembler for the de212 core (an LX6, the ESP32's generation).
#
# There is no Xtensa assembler here -- no llvm-mc target, no binutils --
# so the check runs the other way round from mips-encoding.sh: tools/
# xtensacheck prints every form the backend can emit (every register in
# every field, both ends of every immediate, each of the sixteen constants
# the immediate branches can name, branch, call and l32r targets at the
# ends of their reach and at every alignment phase) as the line QEMU
# prints for it, next to the three bytes emit.c produced, from one call.
# The bytes go into the de212's memory with the CPU stopped (-S), QEMU's
# monitor disassembles them (`xp/Ni`), and the two texts are compared line
# by line. The disassembler is generated from the core's ISA description,
# so an instruction the core lacks, a field in the wrong place or a wrong
# scale or bias reads back as a different line -- and one wrong length
# would shift every line after it.
#
# Then xt_li_inline's and xt_addi_any's sequences are executed in
# xtensacheck's own small interpreter, and every encoder range check is
# provoked and must stop the process rather than truncate.
set -u
echo "TEST-MARKER xtensa-encoding"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_XTENSA:-qemu-system-xtensa}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_XTENSA)"; exit 0; }

out=tests/golden/out/xtensa-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/xtensacheck" \
   tools/xtensacheck/xtensacheck.c src/arch/xtensa/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "xtensacheck did not build"; exit 1; }

BASE=0x60100000
"$out/xtensacheck" --vocab $BASE > "$out/vocab.txt" || {
    echo "xtensacheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 1200 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

cut -d'|' -f1 "$out/vocab.txt" > "$out/want.txt"
cut -d'|' -f2 "$out/vocab.txt" | tr -d '\n' |
    perl -ne 'print pack("H*", $_)' > "$out/blob.bin"
sz=$(wc -c < "$out/blob.bin" | tr -d ' ')
[ "$sz" = $((n * 3)) ] || {
    echo "the blob is $sz bytes for $n three-byte instructions"; exit 1; }

# The monitor on stdin: disassemble, then quit. -S: the CPU never runs.
printf 'xp/%di %s\nquit\n' "$n" "$BASE" |
    "$QEMU" -M sim -cpu de212 -S -display none -monitor stdio \
        -device loader,file="$out/blob.bin",addr=$BASE > "$out/qemu.raw" 2>&1
tr -d '\r' < "$out/qemu.raw" |
    sed -n 's/^0x[0-9a-f]*:  //p' > "$out/got.txt"
m=$(wc -l < "$out/got.txt" | tr -d ' ')
[ "$m" = "$n" ] || {
    echo "QEMU disassembled $m lines of the $n -- an instruction's length"
    echo "is not what the encoder thinks:"
    head -5 "$out/qemu.raw"; exit 1; }
paste -d'|' "$out/want.txt" "$out/got.txt" |
    awk -F'|' '$1 != $2 { print "  want: " $1 "   QEMU: " $2 }' \
    > "$out/diff.txt"
if [ -s "$out/diff.txt" ]; then
    echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n encodings decode to something else:"
    head -20 "$out/diff.txt"
    exit 1
fi

"$out/xtensacheck" --li > "$out/li.txt" || {
    echo "the constant sequences compute the wrong value:"; cat "$out/li.txt"
    exit 1; }

k=$("$out/xtensacheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/xtensacheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: xtensa:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n Xtensa encodings decode as intended on QEMU's de212, every"
echo "register in every field and both ends of every immediate; $(cat "$out/li.txt");"
echo "and $k encoder range checks each refuse rather than truncate"
