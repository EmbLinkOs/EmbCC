#!/bin/sh
# Pointer compound assignment at the target's address width.
#
# irgen lowered `p += n` and `p -= n` (and the multiply by a VLA's element
# size) as 64-bit operations on every target, including the 32-bit ones,
# where a pointer is four bytes. It ran correctly by luck: the backends
# read the missing high half from whatever sat beside the pointer's frame
# slot, and the low half of a sum does not depend on it. Once RISC-V
# stopped giving register-resident values a slot, there was nothing
# beside it -- and the checked slot accessor refused the function rather
# than read memory nothing wrote.
#
# So these loops, whose pointers live in registers at -O2, are run on
# RISC-V (RV32 and RV64) and on the Cortex-M3, and compared with the host.
set -u
echo "TEST-MARKER ptr-compound"
. "$(dirname "$0")/../lib.sh"
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/ptr-compound
rm -rf "$out"; mkdir -p "$out"
cc -w -o "$out/host" "$D/ptr-compound.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
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
    for opt in -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/ptr-compound.c" -o "$B/p.o" || {
            echo "$tag $opt: does not compile"; exit 1; }
        sh "$H/link.sh" "$B/p.elf" "$B/p.o" > "$B/ln.log" 2>&1 || {
            echo "$tag $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$H/run.sh" "$B/p.elf" $RA 2>&1 | head -1)
        [ "$got" = "$want" ] || {
            echo "$tag $opt: disagrees with the host"
            echo "  want: $want"; echo "  got:  $got"; exit 1; }
    done
    ran=$((ran + 1))
}
run_on rv32 riscv32-unknown-elf riscv EMBCC_RISCV_HARNESS \
    "${EMBCC_QEMU_RISCV:-qemu-system-riscv32}" 32
run_on rv64 riscv64-unknown-elf riscv EMBCC_RISCV_HARNESS \
    "${EMBCC_QEMU_RISCV:-qemu-system-riscv64}" 64
run_on m3 thumbv7m-none-eabi thumb EMBCC_THUMB_HARNESS \
    "${EMBCC_QEMU_THUMB:-qemu-system-arm}" ""
[ "$ran" -gt 0 ] || { echo "SKIP: no QEMU for any 32-bit target"; exit 0; }
echo "pointer compound assignment agrees with the host on $ran targets at
-O1, -O2 and -Os"
