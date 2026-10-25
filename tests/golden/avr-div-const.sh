#!/bin/sh
# AVR: an unsigned division of a narrow value by a constant is a multiply.
#
# AVR's int is two bytes, and its divide is a 32-iteration loop in lib/rt
# (__udivsi3), hundreds of cycles. An unsigned dividend known to fit 16
# bits divided by a constant is (x * M) >> s with x * M below 2^32
# (src/opt/divmagic.c, dm_narrow_magic): `x / 10` is x * 52429 >> 19. That
# went unseen on AVR for a second reason: its backend registered no
# answer to "does this operation call a helper", so the optimizer heard
# that a divide is an instruction there (avr_op_calls_helper).
#  1. tests/golden/avr-div-const/prog.c divides by fourteen constants, as
#     quotient and remainder of an unsigned int and quotient of an unsigned
#     char, against a long division by shifts, on QEMU's ATmega328P at
#     -O0, -O2 and -Os;
#  2. at -O2, x / 10 and x % 10 call no division routine.
set -u
echo "TEST-MARKER avr-div-const"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/avr-div-const
rm -rf "$out"; mkdir -p "$out/h"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-div-const
L=build/libc/avr/librt.a
[ -f $L ] || fail "no $L (make rt-embedded)"
"$EMBCC" --target=avr -mmcu=atmega328p -c tests/harness/avr/io.c -o "$out/h/io.o" || fail "io.c"
"$EMBCC" --target=avr -mmcu=atmega328p -c tests/harness/avr/boot.S -o "$out/h/boot.o" || fail "boot.S"
ran=0
for O in -O0 -O2 -Os; do
    "$EMBCC" --target=avr -mmcu=atmega328p $O -c $D/prog.c -o "$out/p$O.o" ||
        fail "$O: prog.c does not compile"
    EMBCC_AVR_HARNESS="$out/h" sh tests/harness/avr/link.sh "$out/p$O.elf" "$out/p$O.o" $L ||
        fail "$O: does not link"
    if command -v "$QEMU" >/dev/null 2>&1; then
        r=$(EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$out/p$O.elf" 2>&1 | tr -d '\r' | head -1)
        [ "$r" = "DIV16 OK" ] || fail "$O: the board says '$r'"
        ran=1
    fi
done
"$OD" -dr "$out/p-O2.o" > "$out/p-O2.dis"
for f in q10 r10; do
    sed -n "/<$f>:/,/^\$/p" "$out/p-O2.dis" | grep -qE '__u?(div|mod)si3' &&
        { sed -n "/<$f>:/,/^\$/p" "$out/p-O2.dis"; fail "-O2: $f calls a division routine"; }
done
[ $ran = 1 ] && how="on the board" || how="(no $QEMU: compiled, not run)"
echo "avr-div-const: fourteen constant divisors at -O0, -O2 and -Os $how; x / 10 and x % 10 multiply at -O2"
