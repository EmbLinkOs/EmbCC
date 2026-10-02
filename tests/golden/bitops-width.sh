#!/bin/sh
# The bit builtins (ctz clz popcount ffs parity clrsb, plain, l and ll) at
# the widths the embedded targets give their types: tests/golden/
# bitops-width.c, self-checking against the loops that define them, on
# AVR (16-bit int, 32-bit long), the Cortex-M3 and RV32 (32-bit long),
# at -O0, -O2 and -Os. (AVR without the long long forms: see the .c.)
#
# irgen took the operand's size to be 8 for the l and ll forms and 4 for
# the plain one: an LP64 host's answer. popcountl read the four bytes
# past a 32-bit long, popcountll truncated its operand to four, and clz
# of a 16-bit int counted 16 too many.
set -u
echo "TEST-MARKER bitops-width"
. "$(dirname "$0")/../lib.sh"
D=$EMBCC_ROOT/tests/golden
out=tests/golden/out/bitops-width
rm -rf "$out"; mkdir -p "$out"
want="ok 142 DONE"
ran=0

check() {  # tag opt output
    got=$(sed -n '1,/DONE/p' "$3" | tr -d '\n' | sed 's/  *$//')
    [ "$got" = "$want" ] || {
        echo "$1 $2: a bit builtin disagrees with its definition"
        echo "  want: $want"; echo "  got:  $got"; exit 1; }
}

if command -v "${EMBCC_QEMU_AVR:-qemu-system-avr}" >/dev/null 2>&1; then
    B=$out/avr; mkdir -p "$B"
    EMBCC_AVR_HARNESS=$PWD/$B; export EMBCC_AVR_HARNESS
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$B/boot.o" &&
    "$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$B/io.o" &&
    mkdir -p "$B/lib" &&
    "$EMBCC" --target=avr -Os -c lib/rt/avr.c -o "$B/lib/avr.o" &&
    "$EMBCC" --target=avr -Os -c lib/rt/avr64.c -o "$B/lib/avr64.o" &&
    ${EMBCC_AR:-llvm-ar} rcs "$B/librt.a" "$B"/lib/*.o || {
        echo "avr: the harness or runtime does not compile"; exit 1; }
    # -O2:n forces the allocator's mode n, as avr-calleesave.sh does
    for spec in -O0 -O2 -Os -O2:1 -O2:2 -O2:3; do
        opt=${spec%%:*}; mode=${spec#*:}; [ "$mode" = "$spec" ] && mode=
        EMBCC_AVR_RA_MODE=$mode "$EMBCC" --target=avr $opt -DBITOPS_NO_LL \
            -c "$D/bitops-width.c" -o "$B/b.o" &&
        sh tests/harness/avr/link.sh "$B/b.elf" "$B/b.o" "$B/librt.a" \
            > "$B/ln.log" 2>&1 || {
            echo "avr $opt: does not build"; head -3 "$B/ln.log"; exit 1; }
        EMBCC_QEMU_UNTIL=DONE sh tests/harness/avr/run.sh "$B/b.elf" \
            > "$B/got" 2>/dev/null
        check avr $spec "$B/got"
    done
    ran=$((ran + 1))
fi

if command -v "${EMBCC_QEMU_THUMB:-qemu-system-arm}" >/dev/null 2>&1; then
    T=thumbv7m-none-eabi B=$out/m3; mkdir -p "$B"
    EMBCC_THUMB_HARNESS=$PWD/$B; export EMBCC_THUMB_HARNESS
    for f in boot io; do
        "$EMBCC" --target=$T -c tests/harness/thumb/$f.c -o "$B/$f.o" || {
            echo "m3: the harness does not compile"; exit 1; }
    done
    "$EMBCC" --target=$T -Os -c lib/rt/int64.c -o "$B/int64.o" || {
        echo "m3: the runtime does not compile"; exit 1; }
    for opt in -O0 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$D/bitops-width.c" -o "$B/b.o" &&
        sh tests/harness/thumb/link.sh "$B/b.elf" "$B/b.o" "$B/int64.o" \
            > "$B/ln.log" 2>&1 || {
            echo "m3 $opt: does not build"; head -3 "$B/ln.log"; exit 1; }
        sh tests/harness/thumb/run.sh "$B/b.elf" > "$B/got" 2>&1
        check m3 $opt "$B/got"
    done
    ran=$((ran + 1))
fi

if command -v "${EMBCC_QEMU_RISCV:-qemu-system-riscv32}" >/dev/null 2>&1; then
    T=riscv32-unknown-elf B=$out/rv32; mkdir -p "$B"
    "$EMBCC" --target=$T -Os -c lib/rt/int64.c -o "$B/int64.o" || {
        echo "rv32: the runtime does not compile"; exit 1; }
    for opt in -O0 -O2 -Os; do
        for f in boot io; do
            "$EMBCC" --target=$T $opt -c tests/harness/riscv/$f.c \
                -o "$B/$f.o" || { echo "rv32: the harness does not compile"; exit 1; }
        done
        "$EMBCC" --target=$T $opt -c "$D/bitops-width.c" -o "$B/b.o" &&
        EMBCC_RISCV_HARNESS="$B" sh tests/harness/riscv/link.sh \
            "$B/b.elf" "$B/b.o" "$B/int64.o" > "$B/ln.log" 2>&1 || {
            echo "rv32 $opt: does not build"; head -3 "$B/ln.log"; exit 1; }
        EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
            sh tests/harness/riscv/run.sh "$B/b.elf" 32 > "$B/got" 2>/dev/null
        check rv32 $opt "$B/got"
    done
    ran=$((ran + 1))
fi
[ "$ran" -gt 0 ] || { echo "SKIP: no QEMU for AVR, ARM or RISC-V"; exit 0; }
echo "every bit builtin agrees with its definition at the width each
embedded target gives int, long and long long, at -O0, -O2 and -Os"
