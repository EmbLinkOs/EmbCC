#!/bin/sh
# Narrow comparisons on AVR (tests/golden/narrow-cmp.c) at every level,
# against the host. The backend compares extensions of the same kind at
# their own width and tests narrow values for zero on their low bytes;
# this pins the edges -- signed and unsigned char and short, the extreme
# values, mixed kinds that must NOT be narrowed, constants at each edge.
set -u
echo "TEST-MARKER avr-narrow-cmp"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/avr-narrow-cmp
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host" "$D/narrow-cmp.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -2 | tr '\n' '|')
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" || {
    echo "the harness does not compile"; exit 1; }
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $opt -c "$D/narrow-cmp.c" -o "$out/n.o" &&
    sh tests/harness/avr/link.sh "$out/n.elf" "$out/n.o" > "$out/ln.log" 2>&1 || {
        echo "$opt: does not build"; head -3 "$out/ln.log"; exit 1; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/n.elf" \
              2>/dev/null | head -2 | tr '\n' '|')
    [ "$got" = "$want" ] || {
        echo "$opt: disagrees with the host"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
done
echo "char and short comparisons -- same and mixed kinds, extreme values,
edge constants, as values and as branches -- agree with the host at every
level"
