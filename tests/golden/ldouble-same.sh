#!/bin/sh
# long double on the targets where it shares a format: a double on ARM
# EABI (the Cortex-M3 with soft float, the M4F with hard float, where it
# travels in d registers as a double does) and a float on AVR.
#
# irgen lowered every long double as a 16-byte value, the x87/binary128
# path, so on ARM -- whose predefined macros already said 8 bytes and 53
# bits, as clang's do -- any long double arithmetic was refused as "a
# 128-bit value". Now it is lowered exactly as the type it shares a format
# with (ty_is_xldouble), while _Generic still tells it apart.
set -u
echo "TEST-MARKER ldouble-same"
. "$(dirname "$0")/../lib.sh"
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/ldouble-same
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host64" "$D/ldouble-same.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
cc -w -DEMBCC_HOST_AS_FLOAT -o "$out/host32" "$D/ldouble-same.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null || {
    echo "the float host reference does not build"; exit 1; }
QA=${EMBCC_QEMU_THUMB:-qemu-system-arm}
QV=${EMBCC_QEMU_AVR:-qemu-system-avr}
ran=0

run_arm() {  # tag triple harness-dir harness-var
    tag=$1 T=$2 H=$EMBCC_ROOT/tests/harness/$3 HV=$4
    command -v "$QA" >/dev/null 2>&1 || return 0
    want=$("$out/host64" | head -1)
    B=$out/$tag; mkdir -p "$B"
    eval "$HV=\$PWD/\$B; export $HV"
    for f in boot io; do
        "$EMBCC" --target=$T -c "$H/$f.c" -o "$B/$f.o" || {
            echo "$tag: the harness does not compile"; exit 1; }
    done
    "$EMBCC" --target=$T -Os -c lib/rt/softfp.c -o "$B/softfp.o" || {
        echo "$tag: the runtime does not compile"; exit 1; }
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/ldouble-same.c" -o "$B/l.o" &&
        sh "$H/link.sh" "$B/l.elf" "$B/l.o" "$B/softfp.o" > "$B/ln.log" 2>&1 || {
            echo "$tag $opt: does not build"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$H/run.sh" "$B/l.elf" 2>&1 | head -1)
        [ "$got" = "$want" ] || {
            echo "$tag $opt: disagrees with the host's double"
            echo "  want: $want"; echo "  got:  $got"; exit 1; }
    done
    ran=$((ran + 1))
}
run_arm m3 thumbv7m-none-eabi thumb EMBCC_THUMB_HARNESS
run_arm m4f thumbv7em-none-eabihf thumb-m4f EMBCC_THUMB_HARNESS

if command -v "$QV" >/dev/null 2>&1; then
    want=$("$out/host32" | head -1)
    B=$out/avr; mkdir -p "$B"
    EMBCC_AVR_HARNESS=$PWD/$B; export EMBCC_AVR_HARNESS
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$B/boot.o" &&
    "$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$B/io.o" || {
        echo "avr: the harness does not compile"; exit 1; }
    # The runtime as an ARCHIVE, so only what the program calls is linked:
    # the harness links every rt*.o it is given, and all eight together
    # do not fit the part's 32 KB of flash.
    mkdir -p "$B/rt"
    for f in lib/rt/avr*.c; do
        "$EMBCC" --target=avr -Os -c "$f" -o "$B/rt/$(basename "$f" .c).o" || {
            echo "avr: $f does not compile"; exit 1; }
    done
    ${EMBCC_AR:-llvm-ar} rcs "$B/librt.a" "$B"/rt/*.o || {
        echo "avr: could not archive the runtime"; exit 1; }
    for opt in -O2 -Os; do       # -O0 does not fit 32 KB with the float code
        "$EMBCC" --target=avr $opt -c "$D/ldouble-same.c" -o "$B/l.o" &&
        sh tests/harness/avr/link.sh "$B/l.elf" "$B/l.o" "$B/librt.a" \
            > "$B/ln.log" 2>&1 || {
            echo "avr $opt: does not build"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh tests/harness/avr/run.sh "$B/l.elf" 2>/dev/null | head -1)
        [ "$got" = "$want" ] || {
            echo "avr $opt: disagrees with the host's float"
            echo "  want: $want"; echo "  got:  $got"; exit 1; }
    done
    ran=$((ran + 1))
fi
[ "$ran" -gt 0 ] || { echo "SKIP: no QEMU for ARM or AVR"; exit 0; }
echo "long double computes as a double on the Cortex-M3 and M4F (hard float)
and as a float on AVR, bit for bit, and _Generic still tells it apart"
