#!/bin/sh
# ARMv6-M inline asm whose operands are values in registers: what
# thumb-asm-values.sh checks on ARMv7-M, for src/arch/thumb/v6m.c's
# gen_asm on a Cortex-M0 (QEMU's micro:bit, tests/harness/thumb-m0).
#
#   1. the shape: CMSIS-style always_inline PRIMASK intrinsics inline into
#      their callers, which then make no call and touch no stack;
#   2. the values, run at -O0, -O1, -O2 and -Os, and again with the pool
#      squeezed to two registers and to none, so the spill paths and the
#      operands in memory run: narrow outputs,
#      two outputs (a continuation), crossed and rotated inputs, outputs
#      to memory, "+r", a float output stored through its address, values
#      live across an asm that writes r0-r3 and r12 or r0 alone, templates
#      that call, and operands in r12 -- an input, a value output and a
#      "+" output -- which ARMv6-M can reach only with MOV and ADD.
set -u
echo "TEST-MARKER thumbv6m-asm-values"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
d=tests/golden/thumbv6m-asm-values
out=tests/golden/out/thumbv6m-asm-values
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v llvm-objdump >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }
T=thumbv6m-none-eabi

# ---- 1. the shape --------------------------------------------------------
for O in -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c "$d/intrin.c" -o "$out/intrin.o" ||
        fail "$O: intrin.c"
    llvm-objdump -d --no-show-raw-insn "$out/intrin.o" > "$out/intrin$O.dis"
    for f in bump_primask primask; do
        sed -n "/<$f>:/,/^\$/p" "$out/intrin$O.dis" > "$out/$f.dis"
        [ -s "$out/$f.dis" ] || { cat "$out/intrin$O.dis"; fail "$O: no $f"; }
        if grep -Eq 'bl	|sp' "$out/$f.dis"; then
            cat "$out/$f.dis"
            fail "$O: $f calls an intrinsic or touches the stack"
        fi
    done
    grep -q 'mrs	r[0-9]*, primask' "$out/bump_primask.dis" &&
    grep -q 'msr	primask, r' "$out/bump_primask.dis" ||
        { cat "$out/bump_primask.dis"; fail "$O: the critical section is not inline"; }
    # mrs, bx lr: a leaf that saves nothing
    n=$(grep -c '^ *[0-9a-f]*:' "$out/primask.dis")
    grep -q push "$out/primask.dis" || [ "$n" -gt 3 ] &&
        { cat "$out/primask.dis"; fail "$O: primask is not mrs and bx lr"; }
done
echo "thumbv6m-asm-values: intrinsics inline, and their callers touch no stack"

# ---- 2. the values -------------------------------------------------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the run: no $QEMU"; exit 0; }
"$QEMU" -M help 2>/dev/null | grep -q '^microbit' || {
    echo "SKIP the run: this $QEMU has no micro:bit (Cortex-M0) machine"; exit 0; }
H=$out/h; mkdir -p "$H"
# ARMv6-M divides by calling __aeabi_uidiv: the harness's putn needs librt
sh tools/build-rt.sh $T "$H" > "$H/build.log" 2>&1 || {
    echo "lib/rt does not build for $T:"; tail -3 "$H/build.log"; exit 1; }
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c tests/harness/thumb-m0/$f.c -o "$H/$f.o" ||
        exit 1
done
printf 'all ok\n==END==\n' > "$out/want"
# run TAG [VAR=VALUE] OPT: compile, link, run, compare
run() {
    tag=$1; O=$2; shift 2
    env "$@" "$EMBCC" --target=$T $O -c "$d/run.c" -o "$out/$tag.o" ||
        fail "$tag: run.c"
    EMBCC_THUMB_M0_HARNESS="$H" sh tests/harness/thumb-m0/link.sh \
        "$out/$tag.elf" "$out/$tag.o" "$H/librt.a" || fail "$tag: link"
    sh tests/harness/thumb-m0/run.sh "$out/$tag.elf" > "$out/$tag.txt" 2>&1
    tr -d '\r' < "$out/$tag.txt" | sed -n '1,/==END==/p' > "$out/cut"
    cmp -s "$out/cut" "$out/want" || { cat "$out/$tag.txt"; fail "$tag: wrong values"; }
}
for O in -O0 -O1 -O2 -Os; do
    run "run$O" $O EMBCC_UNUSED=
    run "run2$O" $O EMBCC_RA_MAXPOOL=2
    run "run0$O" $O EMBCC_RA_MAXPOOL=0      # every value in memory
done
echo "thumbv6m-asm-values: every output, input and crossing value is right at -O0, -O1, -O2 and -Os"
