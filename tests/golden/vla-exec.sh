#!/bin/sh
# Variable-length arrays, run on the Cortex-M3 and on RISC-V (RV32, RV64)
# at every level and compared with the host. Both backends refused them
# by name until each gained a frame-base register; this is what shows the
# frame, the outgoing arguments and the release of each array all land
# where they must. The callee-saved side (r7, s0 restored) is checked by
# thumb-calleesave.sh.
set -u
echo "TEST-MARKER vla-exec"
. "$(dirname "$0")/../lib.sh"
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/vla-exec
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host" "$D/vla.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | head -1)
ran=0
run_on() {  # tag triple harness-dir harness-var qemu run-arg
    tag=$1 T=$2 H=$EMBCC_ROOT/tests/harness/$3 HV=$4 Q=$5 RA=$6
    command -v "$Q" >/dev/null 2>&1 || { echo "SKIP $tag: $Q absent"; return 0; }
    B=$out/$tag; mkdir -p "$B"
    eval "$HV=\$PWD/\$B; export $HV"
    for f in boot io; do
        "$EMBCC" --target=$T -c "$H/$f.c" -o "$B/$f.o" || {
            echo "$tag: the harness does not compile"; exit 1; }
    done
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/vla.c" -o "$B/v.o" || {
            echo "$tag $opt: does not compile"; exit 1; }
        sh "$H/link.sh" "$B/v.elf" "$B/v.o" > "$B/ln.log" 2>&1 || {
            echo "$tag $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$H/run.sh" "$B/v.elf" $RA 2>&1 | head -1)
        [ "$got" = "$want" ] || {
            echo "$tag $opt: disagrees with the host"
            echo "  want: $want"; echo "  got:  $got"; exit 1; }
    done
    ran=$((ran + 1))
}
run_on m3 thumbv7m-none-eabi thumb EMBCC_THUMB_HARNESS \
    "${EMBCC_QEMU_THUMB:-qemu-system-arm}" ""
run_on rv32 riscv32-unknown-elf riscv EMBCC_RISCV_HARNESS \
    "${EMBCC_QEMU_RISCV:-qemu-system-riscv32}" 32
run_on rv64 riscv64-unknown-elf riscv EMBCC_RISCV_HARNESS \
    "${EMBCC_QEMU_RISCV:-qemu-system-riscv64}" 64
[ "$ran" -gt 0 ] || { echo "SKIP: no QEMU for any embedded target"; exit 0; }
echo "variable-length arrays -- in loops, under calls with stack arguments,
2-D with row pointers, recursive, beside a large frame -- agree with the
host on $ran targets at -O0, -O1, -O2 and -Os"
