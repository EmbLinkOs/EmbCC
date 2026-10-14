#!/bin/sh
# AVR's 64-bit division (lib/rt/avr64.c: __udivdi3, __umoddi3 in assembly,
# __divdi3 and __moddi3 over them), on QEMU's ATmega328P at -O0 and -Os:
#  1. quotient and remainder, unsigned and signed, of the pairs in
#     tests/golden/avr-div64/pairs.h are what the host computes -- divisors
#     above the dividend, powers of two, both top bits set (the remainder
#     carrying out of 64 bits), the extremes;
#  2. a division takes under 4000 cycles (counted by Timer1 at /1 under
#     -icount, one count per instruction): the C loop it replaced took
#     23000, 1.4 ms at 16 MHz, which made EmbLinkRTOS's millisecond sleeps
#     late on the board.
set -u
echo "TEST-MARKER avr-div64"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
out=tests/golden/out/avr-div64
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-div64
${CC:-cc} -I$D $D/host.c -o "$out/host" && "$out/host" > "$out/want.txt" || fail "the host program"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "  (no $QEMU: not run)"; exit 0
fi
for O in -O0 -Os; do
    H=$out/h$O; mkdir -p "$H"
    # this tree's lib/rt/avr64.c, ahead of librt.a; the AVR library's
    # snprintf for %llx; linked by -mmcu= as a user's program would be
    "$EMBCC" --target=avr $O -c lib/rt/avr64.c -o "$H/rt64.o" ||
        fail "$O: lib/rt/avr64.c does not compile"
    "$EMBCC" --target=avr -mmcu=atmega328p $O -I$D $D/prog.c "$H/rt64.o" -o "$out/prog$O.elf" ||
        fail "$O: does not link"
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-20}" --until '==END==' "$QEMU" -M uno -nographic \
        -icount shift=6,sleep=off -bios "$out/prog$O.elf" 2>&1 | tr -d '\r' > "$out/got$O.txt"
    cmp -s "$out/want.txt" "$out/got$O.txt" ||
        { diff "$out/want.txt" "$out/got$O.txt" | head; fail "$O: the board divides differently"; }
done
echo "  $(grep -c '^[0-9a-f]' "$out/want.txt") pairs, unsigned and signed, as the host divides them, at -O0 and -Os; under 4000 cycles a division"
