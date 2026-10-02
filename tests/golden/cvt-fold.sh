#!/bin/sh
# Constant conversions folded by the optimizer (pass_fold's fold_cvt),
# against the same conversions at run time and against the host, on the
# Cortex-M3 (soft float) and RISC-V (RV32, RV64) at -O0, -O2 and -Os.
# (-O0 folds nothing, so it is the run-time half on its own.) AVR is left
# out: its double is a float, and several of these become out-of-range
# conversions there, which C leaves undefined.
set -u
echo "TEST-MARKER cvt-fold"
. "$(dirname "$0")/../lib.sh"
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/cvt-fold
rm -rf "$out"; mkdir -p "$out"
cc -w -O0 -o "$out/host" "$D/cvt-fold.c" "$EMBCC_ROOT/tests/harness/thumb/hostio.c" \
    2>/dev/null || { echo "the host reference does not build"; exit 1; }
want=$("$out/host" | tr '\n' '|' | sed 's/==END==|//')
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
    rt=""
    for f in softfp int64; do
        "$EMBCC" --target=$T -Os -c "lib/rt/$f.c" -o "$B/$f.o" 2>/dev/null &&
            rt="$rt $B/$f.o"
    done
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/cvt-fold.c" -o "$B/c.o" || {
            echo "$tag $opt: does not compile"; exit 1; }
        # shellcheck disable=SC2086
        sh "$H/link.sh" "$B/c.elf" "$B/c.o" $rt > "$B/ln.log" 2>&1 || {
            echo "$tag $opt: does not link"; head -3 "$B/ln.log"; exit 1; }
        got=$(sh "$H/run.sh" "$B/c.elf" $RA 2>/dev/null | tr '\n' '|' |
              sed 's/==END==|.*//')
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
echo "folded conversions -- 64-bit to float rounding once, the integer
edges, truncation, double-to-float ties, subnormals and overflow -- agree
with run-time conversion and with the host on $ran targets"
