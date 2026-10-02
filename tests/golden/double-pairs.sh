#!/bin/sh
# 64-bit values in register pairs on RV32 and the Cortex-M3
# (tests/golden/double-pairs.c),
# against the host, bit for bit: doubles chained through the soft-float
# helpers, live across calls, passed in a different argument pair than
# they arrive in, and long long through the 64-bit divide helpers.
#
# Each function is generated with the pair pass and without it and the
# shorter kept, so a pair path that is wrong only where it loses would go
# unseen: -O2:1 and -O2:0 force it on and off (EMBCC_RV_PAIRS,
# EMBCC_T_PAIRS).
set -u
echo "TEST-MARKER double-pairs"
. "$(dirname "$0")/../lib.sh"
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/double-pairs
rm -rf "$out"; mkdir -p "$out"
# -ffp-contract=off: clang on an arm64 host fuses a*b+c into one
# rounding by default, and the soft-float target rounds twice as C says.
cc -w -ffp-contract=off -o "$out/host" "$D/double-pairs.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1 | sed 's/  *$//')
ran=0

QA=${EMBCC_QEMU_THUMB:-qemu-system-arm}
if command -v "$QA" >/dev/null 2>&1; then
    T=thumbv7m-none-eabi B=$out/m3; mkdir -p "$B"
    EMBCC_THUMB_HARNESS=$PWD/$B; export EMBCC_THUMB_HARNESS
    for f in boot io; do
        "$EMBCC" --target=$T -c tests/harness/thumb/$f.c -o "$B/$f.o" || {
            echo "m3: the harness does not compile"; exit 1; }
    done
    for f in softfp int64; do
        "$EMBCC" --target=$T -Os -c lib/rt/$f.c -o "$B/$f.o" || {
            echo "m3: lib/rt/$f.c does not compile"; exit 1; }
    done
    for spec in -O0 -O2 -Os -O2:1 -O2:0; do
        opt=${spec%%:*}; pairs=${spec#*:}; [ "$pairs" = "$spec" ] && pairs=
        EMBCC_T_PAIRS=$pairs "$EMBCC" --target=$T $opt -c "$D/double-pairs.c" \
            -o "$B/d.o" &&
        sh tests/harness/thumb/link.sh "$B/d.elf" "$B/d.o" "$B/softfp.o" \
            "$B/int64.o" > "$B/ln.log" 2>&1 || {
            echo "m3 $spec: does not build"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh tests/harness/thumb/run.sh "$B/d.elf" 2>&1 | head -1 |
              sed 's/  *$//')
        [ "$got" = "$want" ] || {
            echo "m3 $spec: disagrees with the host"
            echo "  want: $want"; echo "  got:  $got"; exit 1; }
    done
    ran=$((ran + 1))
fi

Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv32}
command -v "$Q" >/dev/null 2>&1 || {
    [ "$ran" -gt 0 ] && { echo "(no $Q: RV32 not run)"; exit 0; }
    echo "SKIP: no QEMU for ARM or RISC-V"; exit 0; }
T=riscv32-unknown-elf B=$out/rv32; mkdir -p "$B"
for f in softfp int64; do
    "$EMBCC" --target=$T -Os -c lib/rt/$f.c -o "$B/$f.o" || {
        echo "rv32: lib/rt/$f.c does not compile"; exit 1; }
done
for spec in -O0 -O2 -Os -O2:1 -O2:0; do
    opt=${spec%%:*}; pairs=${spec#*:}; [ "$pairs" = "$spec" ] && pairs=
    for f in boot io; do
        "$EMBCC" --target=$T $opt -c tests/harness/riscv/$f.c -o "$B/$f.o" || {
            echo "rv32: the harness does not compile"; exit 1; }
    done
    EMBCC_RV_PAIRS=$pairs "$EMBCC" --target=$T $opt -c "$D/double-pairs.c" \
        -o "$B/d.o" &&
    EMBCC_RISCV_HARNESS="$B" sh tests/harness/riscv/link.sh "$B/d.elf" \
        "$B/d.o" "$B/softfp.o" "$B/int64.o" > "$B/ln.log" 2>&1 || {
        echo "rv32 $spec: does not build"; head -3 "$B/ln.log"; exit 1; }
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
        sh tests/harness/riscv/run.sh "$B/d.elf" 32 > "$B/got" 2>/dev/null
    got=$(sed -n '1,/DONE/p' "$B/got" | tr -d '\n' | sed 's/  *$//')
    [ "$got" = "$want" ] || {
        echo "rv32 $spec: disagrees with the host"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
done
echo "doubles and long longs in register pairs agree with the host on RV32
and the Cortex-M3, at -O0, -O2 and -Os, with the pair pass forced on and off"
