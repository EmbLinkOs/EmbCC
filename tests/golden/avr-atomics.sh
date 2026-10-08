#!/bin/sh
# AVR atomics, done with interrupts masked (src/arch/avr/codegen.c): every
# read-modify-write and compare-exchange at one, two, four and eight bytes,
# and loads and stores of two to eight, with no library call.
#
# tests/golden/avr-atomics/prog.c runs on QEMU's ATmega328P at every level
# and must print what the host prints -- the builtins, the _Atomic
# operators, pointers, atomic_flag. Then a Timer1 interrupt every 200
# cycles races main for a four- and an eight-byte counter, and not one of
# its increments may be lost (without the cli, dozens are); writes an
# eight-byte value main atomically loads, which must never be torn; and
# checks one main atomically stores, likewise.
#
# shape.c is read off llvm-objdump: every function touches its object
# through Z only between `in r0, SREG; cli` and `out SREG, r0`, calls
# nothing and never executes sei -- restoring SREG is what puts the I flag
# back as it was, so an atomic inside a handler or a critical section
# does not turn interrupts on.
set -u
echo "TEST-MARKER avr-atomics"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
HOSTCC=${HOSTCC:-cc}
out=tests/golden/out/avr-atomics
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-atomics
"$HOSTCC" -std=c99 -w -O2 -o "$out/host" $D/prog.c tests/harness/thumb/hostio.c ||
    fail "the host does not build prog.c"
"$out/host" > "$out/want.txt"
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }
for O in -O0 -O1 -O2 -Os; do
    o=$out/O${O#-O}; mkdir -p "$o"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$o/boot.o" &&
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$o/io.o" &&
    "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$o/rt.o" &&
    "$EMBCC" --target=avr $O -c $D/prog.c -o "$o/prog.o" || fail "$O: does not compile"
    "$EMBLD" -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 \
        "$o/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" -o "$o/prog.elf" || fail "$O: does not link"
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$o/prog.elf" 2>/dev/null |
        tr -d '\r' > "$o/got.txt"
    cmp -s "$out/want.txt" "$o/got.txt" || { diff "$out/want.txt" "$o/got.txt" | head -8
        fail "$O: the board differs from the host"; }
done
echo "  every atomic at 1, 2, 4 and 8 bytes as the host computes it, and interrupt races lost nothing and tore nothing, at -O0, -O1, -O2 and -Os"

if command -v llvm-objdump >/dev/null 2>&1; then
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=avr $O -c $D/shape.c -o "$out/shape.o" ||
            fail "$O: shape.c does not compile"
        llvm-objdump -d --no-show-raw-insn --triple=avr "$out/shape.o" |
            awk -f $D/shape.awk > "$out/shape$O.txt"
        [ "$(grep -c ' ok$' "$out/shape$O.txt")" = 45 ] || {
            grep -v ' ok$' "$out/shape$O.txt" | head -5
            fail "$O: an atomic's object is touched outside an interrupts-off window"; }
    done
    echo "  45 atomics touch their object only with interrupts masked, SREG saved and restored, no call, no sei, at four levels"
else
    echo "  (SKIP: no llvm-objdump for the shape)"
fi
