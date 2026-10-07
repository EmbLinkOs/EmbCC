#!/bin/sh
# EmbCC's SPARC V8 encoder (src/arch/sparc/emit.c), against llvm-mc.
#
# tools/sparccheck prints every form the backend can emit -- each register
# class in each field it can occupy, the signed 13-bit immediates at both
# ends, every condition with and without the annul bit, branch and call
# displacements at the ends of their reach -- as an assembly line and the
# word emit.c produced for it, both from one call. llvm-mc assembles the
# same lines into an object (a branch written `.+N` is resolved in the
# section, so its displacement is compared like any other field), and the
# object's .text is compared word by word, so a mismatch names its line.
#
# The mistakes this machine invites are VALID different instructions: rs1
# and rd exchanged, an op3 one off (addx for add, subx for sub), a branch
# displacement counted from the delay slot as MIPS counts it, a casa's ASI
# in the wrong bits. Each assembles and runs; only a referee sees it.
#
# Then sparc_li's sequences are executed in sparccheck's own small
# interpreter, and every encoder range check is provoked and must stop the
# process rather than truncate.
set -u
echo "TEST-MARKER sparc-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
command -v "$MC" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc not found (set EMBCC_LLVM_MC)"; exit 0; }
command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-objcopy not found (set EMBCC_LLVM_OBJCOPY)"; exit 0; }
"$MC" -triple=sparc -mcpu=leon3 /dev/null -o /dev/null 2>/dev/null || {
    echo "SKIP: this llvm-mc has no SPARC target"; exit 0; }

out=tests/golden/out/sparc-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/sparccheck" \
   tools/sparccheck/sparccheck.c src/arch/sparc/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "sparccheck did not build"; exit 1; }

"$out/sparccheck" --vocab > "$out/vocab.txt" || {
    echo "sparccheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 1000 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

cut -d'|' -f1 "$out/vocab.txt" > "$out/v.s"
cut -d'|' -f2 "$out/vocab.txt" > "$out/ours.txt"
"$MC" -triple=sparc -mcpu=leon3 -filetype=obj "$out/v.s" -o "$out/v.o" \
    2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an entry claims an instruction"
    echo "that does not exist:"
    head -6 "$out/mc.err"; exit 1; }
"$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.bin" || {
    echo "llvm-objcopy could not extract .text"; exit 1; }
od -An -v -tx1 "$out/v.bin" | tr -s ' \n' '  ' | tr ' ' '\n' | sed '/^$/d' |
    paste -d '\0' - - - - > "$out/ref.txt"
m=$(wc -l < "$out/ref.txt" | tr -d ' ')
[ "$m" = "$n" ] || {
    echo "llvm-mc made $m words of the $n lines -- a line expanded into a"
    echo "macro or was not one instruction"; exit 1; }
paste -d'|' "$out/v.s" "$out/ours.txt" "$out/ref.txt" |
    awk -F'|' '$2 != $3 { print "  " $1 ": ours " $2 ", llvm-mc " $3 }' \
    > "$out/diff.txt"
if [ -s "$out/diff.txt" ]; then
    echo "$(wc -l < "$out/diff.txt" | tr -d ' ') of $n encodings differ from llvm-mc's:"
    head -20 "$out/diff.txt"
    exit 1
fi

"$out/sparccheck" --li > "$out/li.txt" || {
    echo "sparc_li computes the wrong value:"; cat "$out/li.txt"; exit 1; }

k=$("$out/sparccheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/sparccheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: sparc:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n SPARC V8 encodings agree with llvm-mc byte for byte, every"
echo "register class in every field and both ends of every immediate;"
echo "$(cat "$out/li.txt"); and $k encoder range checks each refuse rather"
echo "than truncate"
