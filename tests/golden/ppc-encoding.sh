#!/bin/sh
# EmbCC's PowerPC encoder (src/arch/ppc/emit.c), against llvm-mc.
#
# tools/ppccheck prints every form the backend can emit -- each register
# number in each field it can occupy, the immediates at both ends of both
# 16-bit extensions, every branch condition and CR field with displacements
# at the ends of their reach -- as an assembly line and the four bytes
# emit.c stored for it, both from one call. llvm-mc -show-encoding encodes
# the same lines for powerpc-none-eabi, and the bytes are compared in
# memory order (big-endian) line by line, so a mismatch names its
# instruction.
#
# The mistakes this machine invites are VALID different instructions: a
# logical form with its source and destination fields exchanged, subf with
# its operands the wrong way round, an ori given a sign-extended field, an
# SPR number whose two halves are not swapped. Each assembles and runs;
# only a referee sees it.
#
# Then ppc_li's sequences are executed in ppccheck's own small interpreter,
# and every encoder range check is provoked and must stop the process
# rather than truncate.
set -u
echo "TEST-MARKER ppc-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$MC" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc not found (set EMBCC_LLVM_MC)"; exit 0; }
"$MC" -triple=powerpc-none-eabi /dev/null -o /dev/null 2>/dev/null || {
    echo "SKIP: this llvm-mc has no PowerPC target"; exit 0; }

out=tests/golden/out/ppc-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/ppccheck" \
   tools/ppccheck/ppccheck.c src/arch/ppc/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "ppccheck did not build"; exit 1; }

"$out/ppccheck" --vocab > "$out/vocab.txt" || {
    echo "ppccheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 900 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

cut -d'|' -f1 "$out/vocab.txt" > "$out/v.s"
cut -d'|' -f2 "$out/vocab.txt" > "$out/ours.txt"
"$MC" -triple=powerpc-none-eabi -mcpu=e500 -show-encoding "$out/v.s" \
    > "$out/mc.txt" 2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an entry claims an instruction"
    echo "that does not exist:"
    head -6 "$out/mc.err"; exit 1; }
sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\1\2\3\4/p' \
    "$out/mc.txt" > "$out/ref.txt"
m=$(wc -l < "$out/ref.txt" | tr -d ' ')
[ "$m" = "$n" ] || {
    echo "llvm-mc encoded $m of the $n lines -- a line expanded into a"
    echo "macro or was not one instruction"; exit 1; }
paste -d'|' "$out/v.s" "$out/ours.txt" "$out/ref.txt" |
    awk -F'|' '$2 != $3 { print "  " $1 ": ours " $2 ", llvm-mc " $3 }' \
    > "$out/diff.txt"
if [ -s "$out/diff.txt" ]; then
    echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n encodings differ from llvm-mc's:"
    head -20 "$out/diff.txt"
    exit 1
fi

"$out/ppccheck" --li > "$out/li.txt" || {
    echo "ppc_li computes the wrong value:"; cat "$out/li.txt"; exit 1; }

k=$("$out/ppccheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/ppccheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: ppc:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n PowerPC encodings agree with llvm-mc byte for byte, every"
echo "register in every field and both ends of every immediate;"
echo "$(cat "$out/li.txt"); and $k encoder range checks each refuse rather"
echo "than truncate"
