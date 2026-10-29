#!/bin/sh
# AVR: _Float16 and __bf16, on the part.
#
# tests/exec/float16.c checks binary16 bit for bit -- arithmetic, every
# conversion in and out, constants, static initializers, increments and
# compound assignments, members, arrays, pointers, a function taking and
# returning one -- against binary16 hardware's answers. The exec corpus
# does not run on AVR (avr-exec.sh has its own cases, for its 16-bit int),
# so this runs that one program on QEMU's ATmega328P, at -O0, -O1, -O2 and
# -Os: a target whose float and double are both binary32 in software
# (lib/rt/avrfp*.c), whose int is sixteen bits, which aligns nothing, and
# whose half conversions are EmbCC's own (lib/rt/half.c, __embcc_*).
# tests/exec/bfloat16.c the same for __bf16, whose conversions from a
# 32-bit integer take the sticky shift into a float here (a double is four
# bytes) and from a 64-bit one call __embcc_ll2bf.
# A driver calls the program's main, renamed, and prints its status.
set -u
echo "TEST-MARKER avr-float16"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
out=tests/golden/out/avr-float16
rm -rf "$out"; mkdir -p "$out/h"
fail() { echo "FAIL: $*"; exit 1; }
L=build/libc/avr
[ -f $L/librt.a ] && [ -f $L/libc.a ] || fail "no $L/librt.a or libc.a (make rt-embedded libc-embedded)"
[ $L/librt.a -nt lib/rt/half.c ] || fail "$L/librt.a is older than lib/rt/half.c (make rt-embedded)"
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }
cat > "$out/drv.c" <<'EOF'
extern void writec(int);
int prog_main(void);
int main(void)
{
    int r = prog_main();
    const char *e = "\n==END==\n";
    writec('0' + r / 100 % 10);
    writec('0' + r / 10 % 10);
    writec('0' + r % 10);
    while (*e)
        writec(*e++);
    for (;;)
        ;
}
EOF
"$EMBCC" --target=avr -mmcu=atmega328p -c tests/harness/avr/io.c -o "$out/h/io.o" || fail "io.c"
"$EMBCC" --target=avr -mmcu=atmega328p -c tests/harness/avr/boot.S -o "$out/h/boot.o" || fail "boot.S"
"$EMBCC" --target=avr -mmcu=atmega328p -O1 -c "$out/drv.c" -o "$out/drv.o" || fail "the driver"
for p in float16 bfloat16; do
    for O in -O0 -O1 -O2 -Os; do
        EMBCC_VERIFY=1 "$EMBCC" --target=avr -mmcu=atmega328p $O -Dmain=prog_main \
            -Ilib/libc/include -c tests/exec/$p.c -o "$out/$p$O.o" ||
            fail "$O: $p.c does not compile"
        EMBCC_AVR_HARNESS="$out/h" sh tests/harness/avr/link.sh "$out/$p$O.elf" \
            "$out/$p$O.o" "$out/drv.o" $L/libc.a $L/librt.a ||
            fail "$O: $p does not link"
        r=$(EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$out/$p$O.elf" 2>&1 |
            tr -d '\r' | head -1)
        [ "$r" = 042 ] || fail "$O: $p.c exits '$r' on the board, want 042"
    done
done
echo "avr-float16: tests/exec/float16.c and bfloat16.c exit 42 on the ATmega328P at -O0, -O1, -O2 and -Os"
