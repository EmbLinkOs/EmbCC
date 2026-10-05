#!/bin/sh
# ARMv7-M inline asm whose operands are values in registers.
#
# An asm output used to be written through its lvalue's address, so a
# local an asm writes was address-taken and lived in memory; every input
# was read from a stack slot; a value live across any asm was kept out of
# the registers; and a function containing one was never inlined. So
# every CMSIS intrinsic and every FreeRTOS critical section was a call
# to a function with a frame -- FreeRTOS's ulPortRaiseBASEPRI pushed four
# registers and went through a slot for a three-instruction body.
#
# Now an "=r" output is the asm's value (and a second one a continuation
# right after it), inputs are moved into their registers as one parallel
# move, an asm is a call to the allocator (it can touch only r0-r3, r12
# and lr), and the inliner copies it. Checked here:
#
#   1. the shape: CMSIS- and FreeRTOS-style always_inline intrinsics
#      inline into their callers, which then touch no stack at all;
#   2. the values, run on QEMU at every level: narrow outputs extended as
#      their type, two outputs, crossed inputs and outputs, outputs to a
#      global and an array element, "+r", and values live across an asm
#      that writes r0-r3 and r12 -- each against the same thing in C.
set -u
echo "TEST-MARKER thumb-asm-values"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
d=tests/golden/thumb-asm-values
out=tests/golden/out/thumb-asm-values
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v llvm-objdump >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }
T=thumbv7m-none-eabi

# ---- 1. the shape --------------------------------------------------------
for O in -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c "$d/intrin.c" -o "$out/intrin.o" ||
        fail "$O: intrin.c"
    llvm-objdump -d --no-show-raw-insn "$out/intrin.o" > "$out/intrin$O.dis"
    for f in bump bump_primask raised; do
        sed -n "/<$f>:/,/^\$/p" "$out/intrin$O.dis" > "$out/$f.dis"
        [ -s "$out/$f.dis" ] || { cat "$out/intrin$O.dis"; fail "$O: no $f"; }
        if grep -Eq 'bl	|sp' "$out/$f.dis"; then
            cat "$out/$f.dis"
            fail "$O: $f calls an intrinsic or touches the stack"
        fi
    done
    grep -q 'mrs	r[0-9]*, basepri' "$out/bump.dis" &&
    grep -q 'msr	basepri, r' "$out/bump.dis" ||
        { cat "$out/bump.dis"; fail "$O: bump's critical section is not inline"; }
    n=$(grep -c . "$out/raised.dis")
    [ "$n" -le 8 ] || { cat "$out/raised.dis"; fail "$O: raised is $n lines"; }
done
echo "thumb-asm-values: intrinsics inline, and their callers touch no stack"

# ---- 2. the values -------------------------------------------------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the run: no $QEMU"; exit 0; }
H=$out/h; mkdir -p "$H"
for f in boot io; do
    "$EMBCC" --target=$T -c tests/harness/thumb/$f.c -o "$H/$f.o" || exit 1
done
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c "$d/run.c" -o "$out/run.o" || fail "$O: run.c"
    EMBCC_THUMB_HARNESS="$H" sh tests/harness/thumb/link.sh "$out/run.elf" \
        "$out/run.o" || fail "$O: link"
    sh tests/harness/thumb/run.sh "$out/run.elf" > "$out/run$O.txt" 2>&1
    tr -d '\r' < "$out/run$O.txt" | sed -n '1,/==END==/p' > "$out/cut"
    printf 'all ok\n==END==\n' > "$out/want"
    cmp -s "$out/cut" "$out/want" || { cat "$out/run$O.txt"; fail "$O: wrong values"; }
    # and the pool squeezed to two registers, so the spill paths run
    EMBCC_RA_MAXPOOL=2 "$EMBCC" --target=$T $O -c "$d/run.c" -o "$out/run2.o" ||
        fail "$O: run.c with two registers"
    EMBCC_THUMB_HARNESS="$H" sh tests/harness/thumb/link.sh "$out/run2.elf" \
        "$out/run2.o" || fail "$O: link (two registers)"
    sh tests/harness/thumb/run.sh "$out/run2.elf" > "$out/run2$O.txt" 2>&1
    tr -d '\r' < "$out/run2$O.txt" | sed -n '1,/==END==/p' > "$out/cut"
    cmp -s "$out/cut" "$out/want" ||
        { cat "$out/run2$O.txt"; fail "$O: wrong values with two registers"; }
done
echo "thumb-asm-values: every output, input and crossing value is right at -O0, -O1, -O2 and -Os"
