#!/bin/sh
# AVR's one-bit I/O: `REG |= BIT` and `REG &= ~BIT` on a register sbi and
# cbi reach (0x20-0x3f) are one sbi or cbi, as avr-gcc makes them, and do
# what the three operations did.
#   THE SHAPE: tests/golden/avr-bit-io/prog.c sets and clears every bit of
#   DDRB one at a time -- twenty sbi/cbi at -O2 and -Os, the two-bit masks
#   and OCR1BL (past the I/O space) none; EMBCC_AVR_NO_SBI=1 none at all.
#   THE RUN, on EmbSim's uno at -O0 to -Os, with and without them: every
#   value read back is its copy's (QEMU's uno reads the I/O space as 0,
#   so it cannot referee this; EmbSim keeps each register's writable bits
#   as the datasheet says).
set -u
echo "TEST-MARKER avr-bit-io"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
out=tests/golden/out/avr-bit-io
rm -rf "${out:?}"; mkdir -p "$out/h"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
command -v llvm-objdump >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }
src=tests/golden/avr-bit-io/prog.c

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/h/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/h/io.o" &&
sh tools/build-rt.sh avr "$out/h" > "$out/h/rt.log" 2>&1 ||
    fail "the harness does not build"

for O in -O0 -O1 -O2 -Os; do
    for k in "" EMBCC_AVR_NO_SBI=1; do
        tag=p$O${k:+-nosbi}
        env $k "$EMBCC" --target=avr $O -c "$src" -o "$out/$tag.o" ||
            fail "$tag: does not compile"
        n=$(llvm-objdump -d --mcpu=atmega328p --no-show-raw-insn "$out/$tag.o" |
            grep -cE '[[:space:]](sbi|cbi)[[:space:]]+0x4,')
        case $O$k in
        -O2|-Os) [ "$n" = 20 ] || fail "$tag: $n sbi/cbi on DDRB, want 20" ;;
        *EMBCC_AVR_NO_SBI=1) [ "$n" = 0 ] || fail "$tag: $n sbi/cbi with EMBCC_AVR_NO_SBI" ;;
        esac
        EMBCC_AVR_HARNESS="$out/h" sh tests/harness/avr/link.sh "$out/$tag.elf" "$out/$tag.o" \
            > "$out/$tag.lerr" 2>&1 || { cat "$out/$tag.lerr"; fail "$tag: does not link"; }
        "$EMBSIM" "$out/$tag.elf" --board uno --until '==END==' --max-insns 5000000 \
            > "$out/$tag.out" 2>&1
        [ "$(head -1 "$out/$tag.out" | tr -d ' ')" = 42 ] ||
            { head -3 "$out/$tag.out"; fail "$tag: the registers did not keep what was written"; }
    done
done
echo "avr-bit-io: REG |= BIT and REG &= ~BIT on DDRB are one sbi or cbi (twenty at -O2 and -Os), and every bit reads back on the uno at -O0 to -Os"
