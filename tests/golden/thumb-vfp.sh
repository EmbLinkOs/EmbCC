#!/bin/sh
# EmbCC's VFP vocabulary, against llvm-mc.
#
# tools/vfpcheck generates one assembly line per instruction form from
# the SAME table that emits the bytes, so a form added to emit.c cannot
# escape the referee. llvm-mc assembles the lines; the bytes must be
# identical.
#
# A hand-written encoder with no referee is a guess, and VFP is the worst
# place for one, because its wrong answers are all VALID instructions:
#
#  - the register-number split is OPPOSITE for the two widths. A single
#    s<N> contributes N>>1 to the field and N&1 to a flag; a double d<N>
#    contributes N&0xf and N>>4. Encoding a double the single way names a
#    different register.
#  - the conversions' two operands have DIFFERENT widths, because the
#    integer side of a vcvt always lives in a single register. One width
#    flag for both looked right and named two wrong registers.
#  - a wrong opcode group is invisible whenever the destination register
#    is odd, because the D flag then sets the same bit. The first draft
#    had vcvt in group 0xF0 instead of 0xB0 and passed every check that
#    happened to use an odd register.
#
# None of those three is a malformed encoding. Only a byte comparison
# against something that already gets it right finds them.
#
# The vocabulary is assembled twice: for the Cortex-M4F's unit (+vfp4) and
# for the Cortex-M7's (+fp-armv8d16, FPv5-D16), which is where the .f64
# arithmetic, the conversions between the widths and the D-named vpush are
# actually executed. Every form must be the same bytes under both.
set -u
echo "TEST-MARKER thumb-vfp"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/thumb-vfp
rm -rf "$out"; mkdir -p "$out"

command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objcopy not found"; exit 0; }

cc -std=c99 -Wall -Wextra -o "$out/vfpcheck" \
   tools/vfpcheck/vfpcheck.c src/arch/thumb/emit.c src/arch/code.c \
   src/arch/target.c src/driver/util.c src/driver/diag.c \
   src/platform/platform_common.c src/platform/platform_posix.c src/sema/type.c src/sema/ldfloat.c || {
    echo "vfpcheck did not build"; exit 1; }

"$out/vfpcheck" --list > "$out/v.s" || {
    echo "vfpcheck could not list the vocabulary"; exit 1; }
"$out/vfpcheck" bytes > "$out/v.bin" || {
    echo "vfpcheck could not encode its own vocabulary"; exit 1; }

n=$(wc -l < "$out/v.s" | tr -d ' ')
[ "$n" -ge 100 ] || {
    echo "the vocabulary is only $n instructions -- it no longer covers
both widths and both register parities"; exit 1; }

for attr in +vfp4 +fp-armv8d16; do
rm -f "$out/v.o" "$out/v.ref"
"$MC" -triple=thumbv7em-none-eabihf -mattr=$attr -filetype=obj "$out/v.s" \
    -o "$out/v.o" 2> "$out/mc.err" || {
    echo "llvm-mc ($attr) rejected the vocabulary -- an entry claims an"
    echo "        instruction that does not exist:"
    head -4 "$out/mc.err"; exit 1; }
"$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref" 2>/dev/null

cmp -s "$out/v.bin" "$out/v.ref" || {
    echo "an encoding differs from llvm-mc's ($attr):"
    # Name the instruction rather than the byte offset: every form here
    # is four bytes, so the offset divides straight into a line number,
    # and "vcvt.f64.s32 d0, s1" is the thing to go and look at.
    cmp -l "$out/v.bin" "$out/v.ref" 2>/dev/null | head -4 |
    while read -r off ours theirs; do
        ln=$(( (off - 1) / 4 + 1 ))
        printf '  %-28s ours %s, llvm-mc %s\n' \
               "$(sed -n "${ln}p" "$out/v.s")" "$ours" "$theirs"
    done
    exit 1; }
done
echo "all $n VFP instructions encode as llvm-mc does, at both widths and
both register parities, for the Cortex-M4F's unit and the Cortex-M7's"
