#!/bin/sh
# AVR multiplies with the hardware multiplier.
#
# Every part -mmcu= takes is an ATmega, whose `mul` gives an 8x8 product in
# r1:r0 -- and EmbCC used it nowhere: __mulsi3 was a shift-and-add loop of
# up to 32 rounds, and an int times an int called it. Now __mulsi3 is the
# ten partial products libgcc's is (about forty cycles), and a product of
# which only one or two bytes are read is inline: a0*b0, the two cross
# terms into its high byte, r1 cleared after. A full product of two
# values that fit 16 bits -- `(unsigned long)a * b`, and what a division by
# a constant multiplies by -- is the four partial products inline.
#  1. avr-mul/mul32.c checks __mulsi3 over 2000 pairs, and mul16.c the
#     inline products (variables, constants, signed, unsigned, char), each
#     against a product by shifts and adds, and wide.c the 16x16->32
#     products (int by int, int by char, a square, constants), on QEMU's
#     ATmega328P at -O0, -O2 and -Os;
#  2. librt's __mulsi3 is made of mul, and an unsigned int product and an
#     int-by-int product into an unsigned long at -O2 call nothing.
set -u
echo "TEST-MARKER avr-mul"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/avr-mul
rm -rf "$out"; mkdir -p "$out/h"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-mul
L=build/libc/avr/librt.a
[ -f $L ] || fail "no $L (make rt-embedded)"
[ $L -nt lib/rt/avr.c ] || fail "$L is older than lib/rt/avr.c (make rt-embedded)"
"$EMBCC" --target=avr -mmcu=atmega328p -c tests/harness/avr/io.c -o "$out/h/io.o" || fail "io.c"
"$EMBCC" --target=avr -mmcu=atmega328p -c tests/harness/avr/boot.S -o "$out/h/boot.o" || fail "boot.S"
ran=0
for p in mul32:MUL mul16:MUL16 wide:WIDE; do
    f=${p%%:*} want="${p#*:} OK"
    for O in -O0 -O2 -Os; do
        "$EMBCC" --target=avr -mmcu=atmega328p $O -c $D/$f.c -o "$out/$f$O.o" ||
            fail "$f $O: does not compile"
        EMBCC_AVR_HARNESS="$out/h" sh tests/harness/avr/link.sh "$out/$f$O.elf" \
            "$out/$f$O.o" $L || fail "$f $O: does not link"
        if command -v "$QEMU" >/dev/null 2>&1; then
            r=$(EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$out/$f$O.elf" 2>&1 |
                tr -d '\r' | head -1)
            [ "$r" = "$want" ] || fail "$f $O: the board says '$r'"
            ran=1
        fi
    done
done
"$OD" -d "$out/mul32-Os.elf" | sed -n '/<__mulsi3>:/,/^$/p' > "$out/mulsi3.dis"
[ "$(grep -c 'mul	' "$out/mulsi3.dis")" -ge 10 ] ||
    { cat "$out/mulsi3.dis"; fail "librt's __mulsi3 does not use mul"; }
"$OD" -dr "$out/mul16-O2.o" | sed -n '/<mu>:/,/^$/p' > "$out/mu.dis"
grep -q '__mulsi3' "$out/mu.dis" && { cat "$out/mu.dis"; fail "-O2: an unsigned int product calls __mulsi3"; }
grep -q 'mul	' "$out/mu.dis" || { cat "$out/mu.dis"; fail "-O2: an unsigned int product has no mul"; }
"$OD" -dr "$out/wide-O2.o" | sed -n '/<wide>:/,/^$/p' > "$out/wide.dis"
grep -q '__mulsi3' "$out/wide.dis" && { cat "$out/wide.dis"; fail "-O2: a 16x16->32 product calls __mulsi3"; }
[ "$(grep -c 'mul	' "$out/wide.dis")" -ge 4 ] ||
    { cat "$out/wide.dis"; fail "-O2: a 16x16->32 product is not four muls"; }
[ $ran = 1 ] && how="on the board" || how="(no $QEMU: compiled, not run)"
echo "avr-mul: __mulsi3, the inline int and char products and the 16x16->32 products, at -O0, -O2 and -Os $how"
