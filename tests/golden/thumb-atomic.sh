#!/bin/sh
# C11 atomics and the __sync builtins on the Cortex-M: ldrex/strex retry
# loops (with the byte and halfword forms) between full dmb barriers.
#
# tests/golden/atomic-mcu.c checks what each operation computes, against the
# host, and then runs 200000 atomic updates in main while a SysTick handler
# updates the same variables -- so exclusive stores really do fail and the
# retry path really does run. Every total must equal iterations + ticks.
#
# QEMU runs with -icount: without it SysTick follows the host's clock and
# fired four times over the whole loop, which tests nothing. Counted
# instructions make the interrupt land thousands of times, and at the
# same places every run.
set -u
echo "TEST-MARKER thumb-atomic"
. "$(dirname "$0")/../lib.sh"
QEMU=${EMBCC_QEMU_THUMB:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: $QEMU absent"; exit 0; }
D=$EMBCC_ROOT/tests/golden
Q=$EMBCC_ROOT/tests/harness/qrun.sh
out=tests/golden/out/thumb-atomic
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host" "$D/atomic-mcu.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)

run_on() {  # tag triple harness-dir harness-var qemu-machine cpu
    tag=$1 T=$2 H=$EMBCC_ROOT/tests/harness/$3 HV=$4 M=$5 CPU=$6
    "$QEMU" -machine help 2>/dev/null | grep -q "^$M " || {
        echo "SKIP $tag: this QEMU has no $M"; return 0; }
    B=$out/$tag; mkdir -p "$B"
    eval "$HV=\$PWD/\$B; export $HV"
    for f in boot io; do
        "$EMBCC" --target=$T -c "$H/$f.c" -o "$B/$f.o" || {
            echo "$tag: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/atomic-mcu.c" -o "$B/a.o" || {
            echo "$tag $opt: does not compile"; exit 1; }
        sh "$H/link.sh" "$B/a.elf" "$B/a.o" > "$B/ln.log" 2>&1 || {
            echo "$tag $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$Q" 60 "$QEMU" -M "$M" -cpu "$CPU" -nographic \
                  -icount shift=2 -kernel "$B/a.elf" 2>/dev/null | head -1)
        [ "$got" = "$want" ] || {
            echo "$tag $opt: disagrees with the host"
            echo "  want: $want"
            echo "  got:  $got"
            echo "  (the second word onward: interleaved, then each total"
            echo "   == iterations + ticks)"; exit 1; }
    done
}
run_on m3 thumbv7m-none-eabi thumb EMBCC_THUMB_HARNESS lm3s6965evb cortex-m3
run_on m33 thumbv8m.main-none-eabi thumb-m33 EMBCC_M33_HARNESS mps2-an505 cortex-m33
echo "atomics compute what the host computes at 1, 2 and 4 bytes, and lose
no update when a SysTick handler races main for the same variables, on
ARMv7-M and ARMv8-M at -O0, -O1, -O2 and -Os"
