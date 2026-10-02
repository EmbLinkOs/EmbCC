#!/bin/sh
# Every AVR function EmbCC emits preserves what avr-gcc's ABI says a callee
# must: r2-r17 and Y. The register allocator keeps values in PAIRS of
# exactly those registers and saves the ones it takes; EmbCC's own callers
# never keep anything there across a call, so only a check like this one
# sees a pair that was used and not saved. An assembly probe
# (avr-calleesave/probe.S) loads a known value into each, calls functions
# shaped to use the pairs -- values across calls, a loop, more live values
# than pairs, four-byte values, which take a QUAD of two pairs, and a call
# whose arguments are loaded into r10-r17 -- and reports any that came
# back changed, at every level.
set -u
echo "TEST-MARKER avr-calleesave"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
D=$EMBCC_ROOT/tests/golden/avr-calleesave
out=tests/golden/out/avr-calleesave
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host" "$D/main.c" "$D/funcs.c" "$D/host.c" \
    "$EMBCC_ROOT/tests/harness/thumb/hostio.c" 2>/dev/null || {
    echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)
EMBCC_AVR_HARNESS=$PWD/$out; export EMBCC_AVR_HARNESS
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/io.o" &&
"$EMBCC" --target=avr -c "$D/probe.S" -o "$out/probe.o" || {
    echo "the harness or the probe does not build"; exit 1; }
# The runtime as an ARCHIVE of every AVR half (the integer and the float
# helpers), so a program links only what it calls: acrossflt needs the
# float ones.
mkdir -p "$out/rt"
for f in lib/rt/avr*.c; do
    "$EMBCC" --target=avr -Os -c "$f" -o "$out/rt/$(basename "$f" .c).o" ||
        exit 1
done
${EMBCC_AR:-llvm-ar} rcs "$out/librt.a" "$out"/rt/*.o || exit 1
# -O2:n forces the allocator's mode n (EMBCC_AVR_RA_MODE): each function
# is generated every way and the shortest kept, so a mode that is wrong
# where it never wins would go unseen otherwise.
for spec in -O0 -O1 -O2 -Os -O2:1 -O2:2 -O2:3; do
    opt=${spec%%:*}; mode=${spec#*:}; [ "$mode" = "$spec" ] && mode=
    EMBCC_AVR_RA_MODE=$mode "$EMBCC" --target=avr $opt -c "$D/funcs.c" \
        -o "$out/f.o" &&
    EMBCC_AVR_RA_MODE=$mode "$EMBCC" --target=avr $opt -c "$D/main.c" \
        -o "$out/m.o" &&
    sh tests/harness/avr/link.sh "$out/c.elf" "$out/m.o" "$out/f.o" \
        "$out/probe.o" "$out/librt.a" > "$out/ln.log" 2>&1 || {
        echo "$spec: does not build"; head -3 "$out/ln.log"; exit 1; }
    got=$(EMBCC_QEMU_UNTIL=END sh tests/harness/avr/run.sh "$out/c.elf" \
              2>/dev/null | head -1)
    case $got in *CLOBBERED*)
        echo "$spec: a call-saved register came back changed"
        echo "  (CLOBBERED <function> <bit n = r(n+2)> <Y>): $got"; exit 1;;
    esac
    [ "$got" = "$want" ] || {
        echo "$spec: disagrees with the host"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
done
echo "r2-r17 and Y survive every AVR function shape, called from assembly that
checks each, at -O0, -O1, -O2 and -Os"
