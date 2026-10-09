#!/bin/sh
# An AVR interrupt handler gives the interrupted code back its call-saved
# pairs, both halves of each in its place.
#
# A handler whose body loads a call's arguments into r8-r17 -- a 64-bit
# multiply's second operand goes to __muldi3 in r10-r17 -- pushes those
# pairs after its seventeen registers. Its epilogue once never popped
# them: Y, Z, X and the rest came back from the wrong bytes and reti
# returned through two of them. tests/golden/embsim-stack.sh's su.c
# caught that by the crash, but not a handler that pops the pairs in the
# wrong order or forgets one, because nothing it interrupted kept a value
# there.
#
# tests/golden/avr-isr-pairs/prog.c does: work() keeps eight 16-bit
# values, every byte different, live across a loop, in r2-r17 at -O1 and
# above, and runs once with Timer/Counter1 off and once with its compare
# match interrupting it dozens of times; the handler multiplies a long
# long. Both runs must give the same result, on EmbSim at -O0, -O1, -O2
# and -Os. At -O1 and above the disassembly must show the loop using
# r10-r17 and the handler pushing them, or the test proves nothing.
#
# EmbSim only: QEMU's uno keeps Timer/Counter1's interrupt asserted after
# the handler returns (tests/golden/avr-isr.sh), so the loop never ends
# there.
set -u
echo "TEST-MARKER avr-isr-pairs"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
export EMBLD
out=tests/golden/out/avr-isr-pairs
rm -rf "${out:?}"; mkdir -p "$out/h"
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/h/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$out/h/io.o" &&
sh tools/build-rt.sh avr "$out/h" > "$out/rt.log" 2>&1 || {
    echo "FAIL: the AVR harness does not build"; exit 1; }

# fn's disassembly in OBJ
dis() {
    "$OBJDUMP" -d --mcpu=atmega328p "$1" 2> /dev/null |
        awk -v f="<$2>:" '$2 == f { p = 1; next } p && /^$/ { exit } p'
}

fail=0
for opt in -O0 -O1 -O2 -Os; do
    p=$out/p$opt
    "$EMBCC" --target=avr $opt -c tests/golden/avr-isr-pairs/prog.c -o "$p.o" &&
    EMBCC_AVR_HARNESS=$out/h sh tests/harness/avr/link.sh "$p.elf" "$p.o" \
        "$out/h/librt.a" > "$p.link" 2>&1 || {
        echo "FAIL $opt: prog.c does not build"; fail=1; continue; }
    "$EMBSIM" "$p.elf" --board uno --max-insns 50000000 > "$p.out" 2>&1
    read -r quiet busy ticks verdict < "$p.out"
    case "${quiet:-}${busy:-}${ticks:-}" in
    ''|*[!0-9]*)
        echo "FAIL $opt: the program did not run to its end:"
        sed 's/^/     | /' "$p.out" | head -3
        fail=1; continue ;;
    esac
    if [ "${verdict:-}" != same ] || [ "$quiet" != "$busy" ]; then
        echo "FAIL $opt: work(3000) gave ${quiet:-?} with the timer off and ${busy:-?} with it on (${ticks:-?} interrupts):"
        sed 's/^/     | /' "$p.out" | head -3
        fail=1; continue
    fi
    [ "$ticks" -ge 20 ] || { echo "FAIL $opt: only $ticks interrupts came during work"; fail=1; }
    if [ $opt != -O0 ] && command -v "$OBJDUMP" > /dev/null 2>&1; then
        w=$(dis "$p.o" work | grep -oE '\br1[0-7]\b' | sort -u | wc -l | tr -d ' ')
        h=$(dis "$p.o" __vector_11 | grep -cE 'push[[:space:]]+r1[0-7]$')
        [ "$w" = 8 ] && [ "$h" = 8 ] || {
            echo "FAIL $opt: work uses $w of r10-r17 and the handler pushes $h -- the test needs all eight in both"; fail=1; }
    fi
    [ $fail = 0 ] && echo "  $opt: work(3000) = $busy through $ticks interrupts, as with none"
done
[ $fail = 0 ] && echo "avr-isr-pairs: the interrupted code's r10-r17 survive a handler that calls with them"
exit $fail
