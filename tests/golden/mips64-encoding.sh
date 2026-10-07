#!/bin/sh
# EmbCC's MIPS64r2 encoder (src/arch/mips/emit.c with mips_set_64), against
# llvm-mc for mips64el and mips64.
#
# tools/mipscheck --64 prints the whole MIPS32r2 vocabulary (which MIPS64
# keeps) and then the doubleword forms -- daddu..drotrv with every
# register in every field, daddiu at both ends of its field, the
# doubleword shifts at amounts below 32, from 32 (the *32 encodings) and
# 63, dmult..ddivu, dext/dins at the edges of their three encodings each
# (dext, dextm, dextu; dins, dinsm, dinsu), dsbh/dshd, dclz/dclo,
# dmfc0/dmtc0 and the doubleword memory forms (ld, sd, lwu, ldl/ldr,
# sdl/sdr, lld/scd) -- as an assembly line and the word emit.c produced,
# both from one call. llvm-mc -show-encoding encodes the same lines, in
# both byte orders, and the words are compared line by line.
#
# The mistakes a doubleword encoder invites are again VALID different
# instructions: dsll by 40 as dsll with 8 in the field (that is a shift by
# 8), dextm's size field holding size-1 instead of size-33, dinsu's
# position not less 32. Each assembles and runs; only a referee sees it.
#
# Then mips_li64's sequences are executed in mipscheck's 64-bit
# interpreter -- every value computed, none longer than six instructions
# -- and every encoder range check, including every doubleword
# instruction asked for with the 64-bit switch OFF (a MIPS32 object must
# never carry one), must stop the process rather than truncate.
set -u
echo "TEST-MARKER mips64-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$MC" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc not found (set EMBCC_LLVM_MC)"; exit 0; }
"$MC" -triple=mips64el-unknown-elf -mcpu=mips64r2 /dev/null -o /dev/null \
    2>/dev/null || { echo "SKIP: this llvm-mc has no MIPS64 target"; exit 0; }

out=tests/golden/out/mips64-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/mipscheck" \
   tools/mipscheck/mipscheck.c src/arch/mips/emit.c src/arch/code.c \
   src/driver/util.c src/driver/diag.c src/arch/target.c \
   src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "mipscheck did not build"; exit 1; }

check() {                       # check BE-FLAG TRIPLE NAME
    "$out/mipscheck" $1 --64 --vocab > "$out/vocab$3.txt" || {
        echo "mipscheck --64 could not encode its own vocabulary"; exit 1; }
    n=$(wc -l < "$out/vocab$3.txt" | tr -d ' ')
    [ "$n" -ge 1400 ] || {
        echo "the vocabulary is only $n instructions -- it no longer sweeps"
        echo "the fields"; exit 1; }
    { printf '.set noreorder\n.set noat\n.set nomacro\n'
      cut -d'|' -f1 "$out/vocab$3.txt"; } > "$out/v$3.s"
    cut -d'|' -f2 "$out/vocab$3.txt" > "$out/ours$3.txt"
    cut -d'|' -f1 "$out/vocab$3.txt" > "$out/text$3.txt"
    "$MC" -triple=$2 -mcpu=mips64r2 -show-encoding "$out/v$3.s" \
        > "$out/mc$3.txt" 2> "$out/mc$3.err" || {
        echo "llvm-mc ($2) rejected the vocabulary -- an entry claims an"
        echo "instruction that does not exist:"
        grep error "$out/mc$3.err" | head -6; exit 1; }
    if [ -n "$1" ]; then        # big-endian: the bytes in memory order
        sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\1\2\3\4/p' \
            "$out/mc$3.txt" > "$out/ref$3.txt"
    else
        sed -n 's/.*# encoding: \[0x\(..\),0x\(..\),0x\(..\),0x\(..\)\].*/\4\3\2\1/p' \
            "$out/mc$3.txt" > "$out/ref$3.txt"
    fi
    m=$(wc -l < "$out/ref$3.txt" | tr -d ' ')
    [ "$m" = "$n" ] || {
        echo "llvm-mc ($2) encoded $m of the $n lines -- a line expanded"
        echo "into a macro or was not one instruction"; exit 1; }
    paste -d'|' "$out/text$3.txt" "$out/ours$3.txt" "$out/ref$3.txt" |
        awk -F'|' '$2 != $3 { print "  " $1 ": ours " $2 ", llvm-mc " $3 }' \
        > "$out/diff$3.txt"
    if [ -s "$out/diff$3.txt" ]; then
        echo "$(wc -l < "$out/diff$3.txt" | tr -d ' ') of $n encodings ($2) differ from llvm-mc's:"
        head -20 "$out/diff$3.txt"
        exit 1
    fi
}
check "" mips64el-unknown-elf el
check --be mips64-unknown-elf be

"$out/mipscheck" --64 --li > "$out/li.txt" || {
    echo "mips_li64 computes the wrong value:"; cat "$out/li.txt"; exit 1; }
"$out/mipscheck" --be --64 --li > "$out/li-be.txt" || {
    echo "mips_li64 computes the wrong value big-endian:"
    cat "$out/li-be.txt"; exit 1; }

k=$("$out/mipscheck" --64 --refuse list) || { echo "--refuse list failed"; exit 1; }
i=0
while [ "$i" -lt "$k" ]; do
    if "$out/mipscheck" --64 --refuse "$i" > "$out/refuse.txt" 2>&1; then
        echo "refusal $i did not fire:"; cat "$out/refuse.txt"; exit 1
    fi
    grep -q "internal error: mips:" "$out/refuse.txt" || {
        echo "refusal $i failed for the wrong reason:"
        cat "$out/refuse.txt"; exit 1; }
    i=$((i + 1))
done

echo "all $n MIPS64r2 encodings agree with llvm-mc in both byte orders,"
echo "every register in every field and both ends of every immediate;"
echo "$(cat "$out/li.txt"); and $k encoder range checks each refuse"
echo "rather than truncate"
