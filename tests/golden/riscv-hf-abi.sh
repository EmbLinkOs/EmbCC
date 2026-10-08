#!/bin/sh
# RISC-V's hardware floating-point calling convention, against clang ACROSS
# THE CALL: ilp32f, ilp32d, lp64f and lp64d.
#
# tests/golden/riscv-hf-abi-{caller,callee}.c pass floats, doubles and the
# struct shapes the psABI flattens into f registers (and the ones it does
# not) between them; the callee prints the bits it received and the caller
# the bits that came back. Each side is compiled by EmbCC and by clang, and
# all four pairings -- at -O0 and -O2 for EmbCC's -- must print exactly what
# clang calling itself prints. A backend that read the convention
# consistently wrong would agree with itself all the way through; only the
# mixed pairings can tell.
#
# clang gets -ffp-contract=off: it fuses a * b + c into an fmadd by
# default, which rounds once where C (and EmbCC) round twice.
set -u
echo "TEST-MARKER riscv-hf-abi"
. "$(dirname "$0")/../lib.sh"

CLANG=${EMBCC_REF_GCC_RISCV:-clang}
command -v "$CLANG" >/dev/null 2>&1 || {
    echo "SKIP: no reference compiler for RISC-V (set EMBCC_REF_GCC_RISCV)"
    exit 0; }

H=tests/harness/riscv
out=tests/golden/out/riscv-hf-abi
rm -rf "$out"; mkdir -p "$out"
export EMBCC_RISCV_HARNESS

any=0
# ...and the FPU with the integer convention (rv32imafc/ilp32, rv64gc/lp64),
# where floats travel in a registers and no f register survives a call.
for cfg in "32 rv32imafc ilp32f" "32 rv32imafdc ilp32d" "64 rv64gc lp64d" \
           "64 rv64imafc lp64f" "32 rv32imafc ilp32" "64 rv64gc lp64"; do
    set -- $cfg
    x=$1; MARCH=$2; MABI=$3
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$QEMU" >/dev/null 2>&1 || {
        echo "SKIP $MABI: $QEMU not found"; continue; }
    any=1
    T=riscv$x-unknown-elf
    D=$out/$MABI; mkdir -p "$D"
    EMBCC_RISCV_HARNESS="$PWD/$D"
    E="$EMBCC --target=$T -march=$MARCH -mabi=$MABI"
    C="$CLANG -target $T -march=$MARCH -mabi=$MABI -mcmodel=medany \
       -mno-relax -ffreestanding -ffp-contract=off -O1 -I tests/golden"
    for f in boot io; do
        $E -c "$H/$f.c" -o "$D/$f.o" || {
            echo "$MABI: the harness does not compile"; exit 1; }
    done
    rt=""
    for f in int64 softfp; do
        $E -Os -c "lib/rt/$f.c" -o "$D/rt-$f.o" || {
            echo "$MABI: lib/rt/$f.c does not compile"; exit 1; }
        rt="$rt $D/rt-$f.o"
    done
    for side in caller callee; do
        $C -c "tests/golden/riscv-hf-abi-$side.c" -o "$D/c-$side.o" || {
            echo "$MABI: clang could not compile the $side"; exit 1; }
        for opt in -O0 -O2; do
            $E $opt -I tests/golden -c "tests/golden/riscv-hf-abi-$side.c" \
                -o "$D/e$opt-$side.o" || {
                echo "$MABI $opt: EmbCC could not compile the $side"; exit 1; }
        done
    done
    run() {             # run TAG CALLER.o CALLEE.o
        # shellcheck disable=SC2086
        sh "$H/link.sh" "$D/$1.elf" "$2" "$3" $rt || {
            echo "$MABI $1: embld could not link"; return 1; }
        sh "$H/run.sh" "$D/$1.elf" "$x" > "$D/$1.txt" 2>&1
        grep -q '==END==' "$D/$1.txt" || {
            echo "$MABI $1: the image did not reach the end of main:"
            sed -n '1,10p' "$D/$1.txt"; return 1; }
    }
    run cc "$D/c-caller.o" "$D/c-callee.o" || exit 1
    for opt in -O0 -O2; do
        for pair in "ee e$opt-caller e$opt-callee" "ec e$opt-caller c-callee" \
                    "ce c-caller e$opt-callee"; do
            set -- $pair
            run "$1$opt" "$D/$2.o" "$D/$3.o" || exit 1
            diff -u "$D/cc.txt" "$D/$1$opt.txt" > "$D/$1$opt.diff" || {
                echo "$MABI: the $1 pairing at $opt disagrees with clang" \
                     "calling itself:"
                head -16 "$D/$1$opt.diff"; exit 1; }
        done
    done
    echo "$MARCH/$MABI: EmbCC and clang pass floats, doubles and flattened" \
         "structs to each other identically at -O0 and -O2"
done
[ "$any" = 1 ] || { echo "SKIP: no qemu-system-riscv32/64 found"; exit 0; }
