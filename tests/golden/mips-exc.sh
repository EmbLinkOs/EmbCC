#!/bin/sh
# MIPS32: an exception handler that returns, in EmbCC's own assembly.
#
# tests/golden/mips-exc/vector.S is the general exception vector's code,
# assembled by `embcc -c` (src/as/gas.c and the MIPS assembler): it saves
# the registers C may clobber and HI/LO, calls C, and returns with eret.
# main.c installs it at 0x80000180 and makes it run, on QEMU's malta:
#
#   - five `syscall`s from a NAKED function, each returning the value the
#     handler wrote into the saved v0 (35 in all, 5 counted);
#   - the CP0 timer interrupting a register-heavy loop, under -icount so
#     the interrupts land in the same places every run: at least three
#     arrive inside it, and its result equals the result with interrupts
#     off;
#   - no other exception;
#   - a function written as a file-scope asm block that calls C twice.
#
# Each C level is run; the line printed must be "35 5 1 1 1 0 84".
set -u
# Run BIG-endian (mips-none-elf) as tests/golden/mips-be-exc.sh, which sets
# MIPS_BE=1.
if [ "${MIPS_BE:-0}" = 1 ]; then
    NAME=mips-be-exc T=mips-none-elf MT=mips-unknown-elf
    QEMU=${EMBCC_QEMU_MIPSEB:-qemu-system-mips}
else
    NAME=mips-exc T=mipsel-none-elf MT=mipsel-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
d=tests/golden/mips-exc
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"
export EMBCC_MIPS_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
"$EMBCC" --target=$T -c "$d/vector.S" -o "$out/vector.o" || {
    echo "vector.S does not assemble"; exit 1; }

want="35 5 1 1 1 0 84"
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
        echo "$opt: main.c does not compile"; exit 1; }
    sh tests/harness/mips/link.sh "$out/t$opt.elf" "$out/main$opt.o" \
        "$out/vector.o" || { echo "$opt: the image does not link"; exit 1; }
    # -icount: the timer counts instructions, not the host's time, so the
    # interrupts arrive at the same places however busy the machine is
    tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until "==EXIT [0-9]* ==" \
        "$QEMU" -M malta -cpu 24Kc -m 64 -display none -monitor none \
        -serial null -serial null -serial stdio -no-reboot -icount shift=0 \
        -kernel "$out/t$opt.elf" > "$out/t$opt.out" 2>/dev/null
    got=$(head -1 "$out/t$opt.out" | sed 's/ *$//')
    if [ "$got" != "$want" ] || ! grep -q '==EXIT 42 ==' "$out/t$opt.out"; then
        echo "$opt: wanted \"$want\" and ==EXIT 42, got:"
        head -c 300 "$out/t$opt.out"; echo; exit 1
    fi
done
echo "a .S exception handler returns from syscall and the timer interrupt"
echo "(a naked function's syscall, a register-heavy loop interrupted) at -O0..-Os,"
echo "and a file-scope asm function calls C"
