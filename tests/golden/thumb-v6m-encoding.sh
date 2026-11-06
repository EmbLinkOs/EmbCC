#!/bin/sh
# The ARMv6-M (Cortex-M0) vocabulary of src/arch/thumb/emit.c, against
# llvm-mc's thumbv6m encoder, byte for byte.
#
# tools/t1check sweeps every t1_* encoder -- and every ARMv7-M encoder an
# ARMv6-M code generator uses as it is -- across its operands, writing the
# bytes it produced and the assembly each instruction is meant to be.
# llvm-mc assembles the assembly for thumbv6m-none-eabi, so:
#
#  - an instruction ARMv6-M does not have is a line llvm-mc rejects. On a
#    Cortex-M0 that instruction is a HardFault, and the ARMv7-M encoders
#    widen to one whenever the operands do not fit sixteen bits;
#  - an encoding that is a DIFFERENT ARMv6-M instruction is a byte that
#    differs. That one runs, which is why only a comparison finds it.
#
# `t1check --refuse` checks that each encoder with an immediate field
# answers 0 and writes nothing for an operand just outside the field,
# rather than encoding the bits that are left.
set -u
echo "TEST-MARKER thumb-v6m-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/thumb-v6m-encoding
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objcopy not found (set EMBCC_LLVM_MC)"; exit 0; }

cc -std=c99 -Wall -Wextra -Werror -o "$out/t1check" \
   tools/t1check/t1check.c src/arch/thumb/emit.c src/arch/thumb/a32.c src/arch/code.c \
   src/driver/util.c || {
    echo "t1check did not build"; exit 1; }

"$out/t1check" > "$out/v.bin" 2> "$out/v.s" || {
    echo "t1check stopped:"; tail -2 "$out/v.s"; exit 1; }
"$out/t1check" --refuse > "$out/refuse.txt" 2>&1 || {
    echo "an encoder encoded an operand outside its field:"
    cat "$out/refuse.txt"; exit 1; }

n=$(grep -c '@ 0x' "$out/v.s")
enc=$(sed -n 's/.*@ 0x[0-9a-f]* //p' "$out/v.s" | sort -u | wc -l | tr -d ' ')
# A floor, so that a sweep which stopped covering something says so.
[ "$n" -ge 60000 ] && [ "$enc" -ge 45 ] || {
    echo "the sweep is $n instructions from $enc encoders -- it no longer"
    echo "covers the whole ARMv6-M vocabulary"; exit 1; }
# ...and every t1_* encoder emit.h declares, so that one added to emit.c
# without being added to the sweep is unchecked by name, not silently.
missing=
for f in $(sed -n 's/^[a-z].* \(t1_[a-z0-9_]*\)(.*/\1/p' src/arch/thumb/emit.h); do
    grep -q "@ 0x[0-9a-f]* $f\$" "$out/v.s" || missing="$missing $f"
done
[ -z "$missing" ] || {
    echo "declared in emit.h but not swept by tools/t1check:$missing"; exit 1; }

"$MC" -triple=thumbv6m-none-eabi -mcpu=cortex-m0 -filetype=obj "$out/v.s" \
    -o "$out/v.o" 2> "$out/mc.err" || {
    echo "llvm-mc rejected the vocabulary -- an encoder claims an instruction"
    echo "ARMv6-M does not have:"
    head -6 "$out/mc.err"; exit 1; }
[ -s "$out/mc.err" ] && {
    echo "llvm-mc warned about the vocabulary:"; head -6 "$out/mc.err"; exit 1; }
"$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" 2>/dev/null

cmp -s "$out/v.bin" "$out/v.ref" || {
    echo "an encoding differs from llvm-mc's:"
    # By instruction, not by byte: each listing line carries the offset
    # it was emitted at, and the line to look at is the last one at or
    # before the first differing byte.
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
echo "all $n instructions from $enc encoders encode as llvm-mc's thumbv6m does"
echo "$(cat "$out/refuse.txt")"

# literal loads to labels not placed yet (from GCC-like code, reduced):
# pass one's `.+0` must not make one the wide form, which this core lacks
# (they were refused: "ldr r1, [pc, #22]" ... a 32-bit Thumb-2 form)
printf ' .syntax unified\n .thumb\n .text\n adds r7, r6, r7\n movs r0, #0\n add sp, #20\n pop {r4, r5, r6, r7, pc}\n ldr r1, .LCPI2_6\n stm r3!, {r1, r4}\n ldr r5, .LCPI2_9\n adds r5, r6, r5\n mov r1, r6\n movs r6, #65\n lsls r6, r6, #4\n ldr r6, [r1, r6]\n str r6, [r0]\n str r5, [r0, #4]\n str r2, [r0, #8]\n ldr r1, [sp]\n.LCPI2_6:\n.LCPI2_9:\n' \
    > "$out/lit.s"
"${EMBCC:-./embcc}" --target=thumbv6m-none-eabi -c "$out/lit.s" -o "$out/lit.o" 2> "$out/lit.err" || {
    echo "a forward literal load was refused:"; cat "$out/lit.err"; exit 1; }
"$MC" -triple=thumbv6m-none-eabi -filetype=obj "$out/lit.s" -o "$out/lit.ref.o"
for f in lit lit.ref; do
    ${EMBCC_LLVM_OBJDUMP:-llvm-objdump} -d "$out/$f.o" | grep -E '^ +[0-9a-f]+:' > "$out/$f.txt"
done
cmp -s "$out/lit.txt" "$out/lit.ref.txt" || {
    echo "forward literal loads encode otherwise than llvm-mc's:"
    diff "$out/lit.txt" "$out/lit.ref.txt"; exit 1; }
echo "forward literal loads take the 16-bit form, as llvm-mc's do"
