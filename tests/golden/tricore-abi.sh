#!/bin/sh
# The TriCore calling convention ACROSS THE CALL, EmbCC to EmbCC.
#
# There is no TriCore compiler here to pair EmbCC with (the other targets'
# *-abi.sh tests pair it with clang), so what can be checked is that the
# convention is ONE convention: the caller and the callee are separate
# units, each compiled at -O0 -- every value through the frame, the
# prologue storing each parameter from its register -- and at -O2 -- the
# register allocator's parallel moves -- in all four pairings, and every
# pairing must print what the same two files print compiled for the host,
# where integer and IEEE arithmetic have one answer. A caller and a callee
# that read the rules differently disagree in at least one pairing.
#
# Two pairs: tests/golden/embedded-abi-*.c, shared with the other embedded
# suites (structs by value and returned, 8-byte scalars, variadics), and
# tests/golden/tricore-abi-*.c for what is particular to TriCore -- see
# tricore-abi.h: pointers in A4-A7 and back in A2 beside the integers in
# D4-D7, the register a 64-bit argument skips filled by a later word,
# structs of up to 8 bytes as integers and larger ones by reference, the
# hidden result pointer, and every unnamed variadic argument on the stack.
set -u
echo "TEST-MARKER tricore-abi"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
T=tricore-none-elf
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/tricore-abi
rm -rf "$out"; mkdir -p "$out"

inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -undefined dynamic_lookup -o "$out/putc.so" \
   tests/harness/tricore/putc.c 2>/dev/null ||
    { echo "the harness's output plugin does not build"; exit 1; }
EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
EMBCC_TRICORE_HARNESS=$PWD/$out
export EMBCC_TRICORE_PLUGIN EMBCC_TRICORE_HARNESS EMBLD
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out" > /dev/null ||
    { echo "lib/rt does not build for $T"; exit 1; }
"$EMBCC" --target=$T -O1 -c tests/harness/tricore/boot.c -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/tricore/io.c -o "$out/io.o" ||
    { echo "the harness does not compile"; exit 1; }

# Every number modulo 2^32: the programs print through putn(long), and a
# long is 64 bits on the host and 32 on the board, so an unsigned word
# above 2^31 prints negative there -- the same bits.
mod32() {
    awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^-?[0-9]+$/) {
               v = $i + 0; if (v < 0) v += 4294967296; $i = sprintf("%.0f", v) }
           print }' "$1"
}

fail=0
for pair in embedded-abi tricore-abi; do
    caller=tests/golden/$pair-caller.c callee=tests/golden/$pair-callee.c
    cc -w -Itests/golden -o "$out/$pair-host" "$caller" "$callee" \
        tests/harness/riscv/hostio.c || { echo "$pair: host build"; exit 1; }
    "$out/$pair-host" > "$out/$pair-ref.raw" || { echo "$pair: host run"; exit 1; }
    mod32 "$out/$pair-ref.raw" > "$out/$pair-ref.txt"
    for oc in -O0 -O2; do
        for oe in -O0 -O2; do
            p=$out/$pair$oc$oe
            "$EMBCC" --target=$T $oc -Itests/golden -c "$caller" -o "$p-caller.o" &&
            "$EMBCC" --target=$T $oe -Itests/golden -c "$callee" -o "$p-callee.o" || {
                echo "$pair caller $oc callee $oe: does not compile"; fail=1; continue; }
            sh tests/harness/tricore/link.sh "$p.elf" "$p-caller.o" \
                "$p-callee.o" "$out/librt.a" > "$p.ld" 2>&1 || {
                echo "$pair caller $oc callee $oe: does not link"; head -3 "$p.ld"
                fail=1; continue; }
            sh tests/harness/tricore/run.sh "$p.elf" > "$p.raw" 2>&1
            sed -n '1,/==END==/p' "$p.raw" > "$p.end"
            mod32 "$p.end" > "$p.txt"
            if ! diff -u "$out/$pair-ref.txt" "$p.txt" > "$p.diff"; then
                echo "$pair: caller $oc, callee $oe disagree with the host:"
                head -12 "$p.diff"; fail=1
            fi
        done
    done
    [ "$fail" = 0 ] && echo "$pair: the four -O0/-O2 pairings agree with the host"
done
[ "$fail" = 0 ] || exit 1
echo "the TriCore convention is one convention at every pairing"
