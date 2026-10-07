#!/bin/sh
# The A32 (ARM state) vocabulary of src/arch/thumb/a32.c, against llvm-mc's
# armv7a encoder, word for word.
#
# tools/a32check drives every encoder the way the code generator does --
# through emit.h's t_* interface with t_isa_a32 set -- across its operands:
# every modified immediate with every data-processing operation, every
# addw value 0..4095 (one instruction or two), every load/store size at
# both ends of its offset field, every shift amount, every bit-field, the
# branches at both ends of their reach, every condition through the IT
# queue, the exclusives, the system instructions and the VFP forms. It
# writes the bytes it produced and the assembly each call is meant to be;
# llvm-mc assembles the assembly for armv7a-none-eabi, so
#
#  - an instruction ARMv7-A does not have is a line llvm-mc rejects;
#  - an encoding that is a DIFFERENT instruction is a word that differs.
#
# `a32check --refuse` checks the other half of the contract: each
# int-returning encoder answers 0 and writes nothing for an operand A32
# cannot say (an odd rotation, an ldrd of an odd register, a halfword offset
# past 255, a branch past 32 MB ...), rather than encoding what is left.
set -u
echo "TEST-MARKER arm-a32-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/arm-a32-encoding
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objcopy not found (set EMBCC_LLVM_MC)"; exit 0; }

cc -std=c99 -Wall -Wextra -Werror -o "$out/a32check" \
   tools/a32check/a32check.c src/arch/thumb/emit.c src/arch/thumb/a32.c \
   src/arch/code.c src/driver/util.c || {
    echo "a32check did not build"; exit 1; }

"$out/a32check" > "$out/v.bin" 2> "$out/v.s" || {
    echo "a32check stopped:"; tail -2 "$out/v.s"; exit 1; }
"$out/a32check" --refuse > "$out/refuse.txt" 2>&1 || {
    echo "an encoder encoded an operand A32 cannot say:"
    cat "$out/refuse.txt"; exit 1; }

n=$(grep -c '@ 0x' "$out/v.s")
enc=$(sed -n 's/.*@ 0x[0-9a-f]* //p' "$out/v.s" | sort -u | wc -l | tr -d ' ')
# A floor, so that a sweep which stopped covering something says so.
[ "$n" -ge 120000 ] && [ "$enc" -ge 50 ] || {
    echo "the sweep is $n instructions from $enc encoders -- it no longer"
    echo "covers the whole A32 vocabulary"; exit 1; }
# ...and every encoder a32.h declares (the predicates and the IT queue's
# bookkeeping excepted), so one added without being swept is named.
missing=
for f in $(sed -n 's/^[a-z].* \(a32_[a-z0-9_]*\)(.*/\1/p' src/arch/thumb/a32.h); do
    case $f in
    a32_imm_ok|a32_encode_imm|a32_ldst_reg_ok|a32_it_open|a32_it_reset) continue ;;
    esac
    grep -q "@ 0x[0-9a-f]* $f\$" "$out/v.s" || missing="$missing $f"
done
[ -z "$missing" ] || {
    echo "declared in a32.h but not swept by tools/a32check:$missing"; exit 1; }

# +hwdiv-arm for sdiv/udiv (a Cortex-A7/A15's; the code generator calls
# __aeabi_idiv instead), +vfp4 for the VFP forms.
"$MC" -triple=armv7a-none-eabi -mattr=+hwdiv-arm,+vfp4 -filetype=obj \
    "$out/v.s" -o "$out/v.o" 2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an encoder claims an instruction"
    echo "ARMv7-A does not have:"
    head -6 "$out/mc.err"; exit 1; }
[ -s "$out/mc.err" ] && {
    echo "llvm-mc warned about the vocabulary:"; head -6 "$out/mc.err"; exit 1; }
"$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" 2>/dev/null

cmp -s "$out/v.bin" "$out/v.ref" || {
    echo "an encoding differs from llvm-mc's:"
    cmp -l "$out/v.bin" "$out/v.ref" 2>/dev/null | head -4 |
    while read -r off ours theirs; do
        awk -v b=$((off - 1)) '
            /@ 0x/ { o = $0; sub(/.*@ 0x/, "", o); sub(/ .*/, "", o)
                     v = 0
                     for (i = 1; i <= length(o); i++)
                         v = v * 16 + index("0123456789abcdef",
                                            substr(o, i, 1)) - 1
                     if (v <= b) hit = $0 }
            END { sub(/^[ \t]+/, "", hit); print "  " hit }' "$out/v.s"
        echo "    byte $off: ours $ours, llvm-mc $theirs (octal)"
    done
    exit 1; }
echo "all $n instructions from $enc encoders encode as llvm-mc's armv7a does"
cat "$out/refuse.txt"
