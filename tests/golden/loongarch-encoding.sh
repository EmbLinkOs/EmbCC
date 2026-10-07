#!/bin/sh
# EmbCC's LoongArch64 encoder (src/arch/loongarch/emit.c), against llvm-mc.
#
# tools/lacheck prints every form the backend can emit -- each register
# number in each field it can occupy, the immediates at both ends of their
# extension, branch offsets at the ends of their reach -- as an assembly
# line and the word emit.c produced for it, both from one call. llvm-mc
# -show-encoding encodes the same lines, and the two words are compared
# line by line, so a mismatch names its instruction.
#
# The mistakes this machine invites are VALID different instructions: an
# ori handed a sign-extended field, a branch with rj and rd in each other's
# places, a bstrpick whose msb and lsb trade fields, an alsl whose shift
# field holds sa where the ISA wants sa - 1. Each of those assembles and
# runs; only a referee sees it.
#
# Then every la_li sequence is compared with llvm-mc's own expansion of
# `li.d` for the same value (LLVM's LoongArchMatInt is the reference for
# which instructions to use), executed in lacheck's small interpreter, and
# every encoder range check is provoked and must stop the process rather
# than truncate.
set -u
echo "TEST-MARKER loongarch-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$MC" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc not found (set EMBCC_LLVM_MC)"; exit 0; }
"$MC" -triple=loongarch64 /dev/null -o /dev/null 2>/dev/null || {
    echo "SKIP: this llvm-mc has no LoongArch target"; exit 0; }

out=tests/golden/out/loongarch-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/lacheck" \
   tools/lacheck/lacheck.c src/arch/loongarch/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "lacheck did not build"; exit 1; }

"$out/lacheck" --vocab > "$out/vocab.txt" || {
    echo "lacheck could not encode its own vocabulary"; exit 1; }
n=$(wc -l < "$out/vocab.txt" | tr -d ' ')
[ "$n" -ge 1500 ] || {
    echo "the vocabulary is only $n instructions -- it no longer sweeps the
fields"; exit 1; }

cut -d'|' -f1 "$out/vocab.txt" > "$out/v.s"
cut -d'|' -f2 "$out/vocab.txt" > "$out/ours.txt"
"$MC" -triple=loongarch64 -show-encoding "$out/v.s" \
    > "$out/mc.txt" 2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an entry claims an instruction"
    echo "that does not exist:"
    head -6 "$out/mc.err"; exit 1; }
sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\4\3\2\1/p' \
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

# la_li against llvm-mc's li.d, one value at a time: each li.d is followed
# by a marker instruction li.d never expands to, which separates the
# groups in llvm-mc's output.
"$out/lacheck" --li > "$out/li.txt" || { echo "lacheck --li failed"; exit 1; }
nli=$(wc -l < "$out/li.txt" | tr -d ' ')
awk -F'|' '{ print $1; print "break 32767" }' "$out/li.txt" > "$out/li.s"
"$MC" -triple=loongarch64 -show-encoding "$out/li.s" > "$out/limc.txt" \
    2> "$out/limc.err" || {
    echo "llvm-mc rejected a li.d:"; head -4 "$out/limc.err"; exit 1; }
sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\4\3\2\1/p' \
    "$out/limc.txt" |
    awk '$1 == "002a7fff" { print l; l = ""; next }
         { l = (l == "" ? $1 : l " " $1) }' > "$out/liref.txt"
cut -d'|' -f1 "$out/li.txt" > "$out/litext.txt"
cut -d'|' -f2 "$out/li.txt" > "$out/liours.txt"
paste -d'|' "$out/litext.txt" "$out/liours.txt" "$out/liref.txt" |
    awk -F'|' '$2 != $3 { print "  " $1 ": ours " $2 ", llvm-mc " $3 }' \
    > "$out/lidiff.txt"
[ "$(wc -l < "$out/liref.txt" | tr -d ' ')" = "$nli" ] || {
    echo "llvm-mc expanded a different number of li.d than were asked"; exit 1; }
if [ -s "$out/lidiff.txt" ]; then
    echo "$(wc -l < "$out/lidiff.txt" | tr -d ' ') of $nli constants are built differently from llvm-mc's li.d:"
    head -10 "$out/lidiff.txt"
    exit 1
fi

"$out/lacheck" --run-li > "$out/runli.txt" || {
    echo "la_li computes the wrong value:"; cat "$out/runli.txt"; exit 1; }

k=$("$out/lacheck" --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/lacheck" --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: loongarch:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n LoongArch64 encodings agree with llvm-mc, every register in"
echo "every field and both ends of every immediate; $nli constants are built"
echo "as llvm-mc's li.d builds them and $(cat "$out/runli.txt");"
echo "and $k encoder range checks each refuse rather than truncate"
