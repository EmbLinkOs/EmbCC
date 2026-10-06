#!/bin/sh
# EmbCC's MIPS32r2 encoder (src/arch/mips/emit.c), against llvm-mc.
#
# tools/mipscheck prints every form the backend can emit -- each register
# number in each field it can occupy, the immediates at both ends of both
# 16-bit extensions, branch displacements at the ends of their reach --
# as an assembly line and the word emit.c produced for it, both from one
# call. llvm-mc -show-encoding encodes the same lines, and the two words
# are compared line by line, so a mismatch names its instruction.
#
# The mistakes this machine invites are VALID different instructions: an
# andi handed a sign-extended field, sllv with the value and the amount in
# each other's fields, an ext whose size field holds the last bit (that is
# ins's encoding), a break code in the low half of its field. Each of those
# assembles and runs; only a referee sees it.
#
# Then mips_li's sequences are executed in mipscheck's own small
# interpreter, and every encoder range check is provoked and must stop the
# process rather than truncate.
set -u
echo "TEST-MARKER mips-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$MC" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc not found (set EMBCC_LLVM_MC)"; exit 0; }
"$MC" -triple=mipsel-unknown-elf -mcpu=mips32r2 /dev/null -o /dev/null \
    2>/dev/null || { echo "SKIP: this llvm-mc has no MIPS target"; exit 0; }

out=tests/golden/out/mips-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/mipscheck" \
   tools/mipscheck/mipscheck.c src/arch/mips/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "mipscheck did not build"; exit 1; }

"$out/mipscheck" --vocab > "$out/vocab.txt" || {
    echo "mipscheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 600 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

{ printf '.set noreorder\n.set noat\n.set nomacro\n'
  cut -d'|' -f1 "$out/vocab.txt"; } > "$out/v.s"
cut -d'|' -f2 "$out/vocab.txt" > "$out/ours.txt"
"$MC" -triple=mipsel-unknown-elf -mcpu=mips32r2 -show-encoding "$out/v.s" \
    > "$out/mc.txt" 2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an entry claims an instruction"
    echo "that does not exist:"
    head -6 "$out/mc.err"; exit 1; }
sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\4\3\2\1/p' \
    "$out/mc.txt" > "$out/ref.txt"
m=$(wc -l < "$out/ref.txt" | tr -d ' ')
[ "$m" = "$n" ] || {
    echo "llvm-mc encoded $m of the $n lines -- a line expanded into a"
    echo "macro or was not one instruction:"
    grep -v '^	\.set' "$out/mc.txt" | grep -v 'encoding:' | head -6; exit 1; }
cut -d'|' -f1 "$out/vocab.txt" > "$out/text.txt"
paste -d'|' "$out/text.txt" "$out/ours.txt" "$out/ref.txt" |
    awk -F'|' '$2 != $3 { print "  " $1 ": ours " $2 ", llvm-mc " $3 }' \
    > "$out/diff.txt"
if [ -s "$out/diff.txt" ]; then
    echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n encodings differ from llvm-mc's:"
    head -20 "$out/diff.txt"
    exit 1
fi

"$out/mipscheck" --li > "$out/li.txt" || {
    echo "mips_li computes the wrong value:"; cat "$out/li.txt"; exit 1; }

k=$("$out/mipscheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/mipscheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: mips:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n MIPS32r2 encodings agree with llvm-mc, every register in every"
echo "field and both ends of every immediate; $(cat "$out/li.txt");"
echo "and $k encoder range checks each refuse rather than truncate"
