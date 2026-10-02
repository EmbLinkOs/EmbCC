#!/bin/sh
# Every Thumb-2 encoding src/arch/thumb/emit.c produces, disassembled and
# compared against what it was supposed to be (D-015).
#
# tools/thumbcheck emits one instruction per call and prints the intended
# disassembly beside it; this assembles the raw bytes into an object and
# lets llvm-objdump say what they actually are. A backend that encodes its
# own instructions has no assembler to catch a wrong bit, so this is the
# only thing standing between a typo and a silently different instruction.
#
# Skipped where llvm is not installed — the encoder is still exercised by
# every thumb test that runs code, but this is the one that reads the bits.
set -u
echo "TEST-MARKER thumb-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$MC" >/dev/null 2>&1 && command -v "$OD" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objdump not found (set EMBCC_LLVM_MC)"; exit 0; }

out=tests/golden/out/thumb-encoding
rm -rf "$out"; mkdir -p "$out"

cc -std=c99 -Wall -Wextra -o "$out/thumbcheck" \
   tools/thumbcheck/thumbcheck.c src/arch/thumb/emit.c \
   src/arch/code.c src/driver/util.c || {
    echo "thumbcheck did not build"; exit 1; }

check_one() {
    "$out/thumbcheck" $1 > "$out/bytes.bin" 2> "$out/want.txt" || {
        echo "thumbcheck exited nonzero"; exit 1; }

# The bytes, wrapped in just enough assembly to be a Thumb .text section.
{
    echo '	.syntax unified'
    echo '	.thumb'
    echo '	.section .text'
    echo '	.thumb_func'
    od -An -v -tx1 "$out/bytes.bin" | tr -s ' ' '\n' | sed '/^$/d' |
        sed 's/^/	.byte 0x/'
} > "$out/blob.s"

"$MC" -triple=thumbv7m-none-eabi -filetype=obj "$out/blob.s" -o "$out/blob.o" || {
    echo "llvm-mc could not assemble the emitted bytes"; exit 1; }

# llvm-objdump's own format, reduced to the instruction: drop the address
# and the raw bytes, drop its trailing "@ imm = ..." note and the <symbol>
# annotations on branch targets, and print immediates in decimal so the
# expectations beside each encoder stay readable.
"$OD" -d --triple=thumbv7m "$out/blob.o" |
    sed -n 's/^[ 	]*[0-9a-f]\{1,\}:[ 	]*//p' |
    sed 's/^\(\([0-9a-f][0-9a-f][0-9a-f][0-9a-f] \)\{1,2\}\)[ 	]*//' |
    sed 's/[ 	]*@.*$//' |
    sed 's/ <[^>]*>//g' |
    sed 's/[ 	]*$//' |
    awk '{ while (match($0, /#0x[0-9a-fA-F]+/)) {
               h = substr($0, RSTART + 3, RLENGTH - 3)
               v = 0
               for (i = 1; i <= length(h); i++) {
                   ch = tolower(substr(h, i, 1))
                   v = v * 16 + index("0123456789abcdef", ch) - 1
               }
               $0 = substr($0, 1, RSTART - 1) "#" v substr($0, RSTART + RLENGTH)
           }
           print }' > "$out/got.txt"

    if ! diff -u "$out/want.txt" "$out/got.txt" > "$out/diff.txt"; then
        echo "$2 — an encoding is not the instruction it was meant to be:"
        head -40 "$out/diff.txt"
        exit 1
    fi
    echo "$(wc -l < "$out/want.txt" | tr -d ' ') $2"
}

check_one "" "Thumb-2 encodings disassemble as intended"
# Every value the 12-bit modified immediate can hold. t_imm_ok() decides
# by searching that space, so a wrong bit would not fail the search — it
# would emit a field meaning a different number, which only this catches.
check_one --immediates "modified immediates hold the value asked for"
