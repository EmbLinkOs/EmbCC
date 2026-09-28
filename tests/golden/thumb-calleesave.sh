#!/bin/sh
# Every EmbCC function preserves what AAPCS32 says a callee must: r4-r11
# and sp.
#
# EmbCC's own callers cannot check this -- they never keep a value in
# r9-r11, which the backend uses as fixed scratch -- so a function that
# forgot to save one would pass every other test here and then corrupt a
# caller compiled by GCC or clang. That became a live question when the
# prologue started saving only the scratch registers a body actually
# uses: a use that was not recorded is exactly that clobber. So an
# assembly probe (calleesave/probe.S) loads a known value into each of
# r4-r11, calls an EmbCC function, and reports which came back changed,
# for functions shaped to use every scratch path: a leaf, an argument
# cycle, 64-bit arithmetic, a frame past every short offset, values
# living across calls, a switch, division and soft floating point.
set -u
echo "TEST-MARKER thumb-calleesave"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
MC=${EMBCC_LLVM_MC:-llvm-mc}
command -v "$MC" >/dev/null 2>&1 || { echo "SKIP: no llvm-mc for the probe"; exit 0; }

T=thumbv7m-none-eabi
H=$EMBCC_ROOT/tests/harness/thumb
D=$EMBCC_ROOT/tests/golden/calleesave
out=tests/golden/out/thumb-calleesave
rm -rf "$out"; mkdir -p "$out"
EMBCC_THUMB_HARNESS=$PWD/$out; export EMBCC_THUMB_HARNESS
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
for f in softfp int64; do
    "$EMBCC" --target=$T -Os -c "lib/rt/$f.c" -o "$out/$f.o" || {
        echo "lib/rt/$f.c does not compile"; exit 1; }
done
"$MC" -triple=thumbv7m -filetype=obj "$D/probe.S" -o "$out/probe.o" || {
    echo "the probe does not assemble"; exit 1; }

cc -w -o "$out/host" "$D/main.c" "$D/funcs.c" "$D/host.c" "$H/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)
for opt in -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$D/funcs.c" -o "$out/funcs.o" &&
    "$EMBCC" --target=$T $opt -c "$D/main.c" -o "$out/main.o" || {
        echo "$opt: does not compile"; exit 1; }
    sh "$H/link.sh" "$out/t.elf" "$out/main.o" "$out/funcs.o" "$out/probe.o" \
        "$out/softfp.o" "$out/int64.o" > "$out/ln.log" 2>&1 || {
        echo "$opt: does not link"; head -3 "$out/ln.log"; exit 1; }
    got=$(sh "$H/run.sh" "$out/t.elf" 2>&1 | head -1)
    case $got in *CLOBBERED*)
        echo "$opt: a callee-saved register came back changed"
        echo "  (CLOBBERED <function> <mask: bit n = rn, 0x2000 = sp>)"
        echo "  $got"; exit 1;;
    esac
    [ "$got" = "$want" ] || {
        echo "$opt: disagrees with the host"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
done
echo "r4-r11 and sp survive every EmbCC function shape, called from assembly
that checks each one, at -O1, -O2 and -Os"
