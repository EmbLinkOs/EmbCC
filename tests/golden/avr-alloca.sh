#!/bin/sh
# Dynamic stack allocation on AVR -- variable-length arrays, alloca, and
# aligned locals -- on QEMU's ATmega328P, against the host.
#
# All three move sp below the frame, which stays at Y. A callee finds its
# stack arguments just above its return address, so a call passing any is
# given them copied down to the new sp (src/arch/avr/codegen.c, IR_CALL);
# a VLA scope gives its block back at its end (IR_SPRESTORE), and the
# epilogue sets sp from Y. tests/golden/avr-alloca/prog.c drives each:
# stack arguments after a VLA, direct, indirect and variadic; 80 VLA
# scopes, more than the part's RAM if one leaks; 400 calls after an
# aligned local, the same; a frameless alloca function; recursion; alloca
# in a loop; aligned locals at their alignment. Every level, and every
# register allocation mode forced, since best-of-three only ever runs the
# winner. Its own time limit rather than the suite's 20 s: -O0 is the
# slowest AVR run there is, and a loaded machine once cut it short.
set -u
echo "TEST-MARKER avr-alloca"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
HOSTCC=${HOSTCC:-cc}
out=tests/golden/out/avr-alloca
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-alloca
"$HOSTCC" -std=c99 -w -O2 -o "$out/host" $D/prog.c tests/harness/thumb/hostio.c ||
    fail "the host does not build prog.c"
"$out/host" > "$out/want.txt"
grep -q '==END==' "$out/want.txt" || fail "the host run did not finish"
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$out/boot.o" ||
    fail "the startup does not assemble"
for mode in best 1 2 3; do
    for O in -O0 -O1 -O2 -Os; do
        o=$out/$mode${O#-}; mkdir -p "$o"
        if [ "$mode" = best ]; then M=; else M=$mode; fi
        EMBCC_AVR_RA_MODE=$M "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$o/io.o" &&
        EMBCC_AVR_RA_MODE=$M "$EMBCC" --target=avr $O -c lib/rt/avr.c -o "$o/rt.o" &&
        EMBCC_AVR_RA_MODE=$M "$EMBCC" --target=avr $O -c $D/prog.c -o "$o/prog.o" ||
            fail "$O mode $mode: does not compile"
        "$EMBLD" -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 \
            "$out/boot.o" "$o/io.o" "$o/rt.o" "$o/prog.o" -o "$o/prog.elf" ||
            fail "$O mode $mode: does not link"
        EMBCC_QEMU_TIMEOUT=60 EMBCC_QEMU_UNTIL='==END==' sh tests/harness/avr/run.sh "$o/prog.elf" 2>/dev/null |
            tr -d '\r' > "$o/got.txt"
        cmp -s "$out/want.txt" "$o/got.txt" || { diff "$out/want.txt" "$o/got.txt" | head -8
            fail "$O mode $mode: the board differs from the host"; }
    done
done
echo "VLAs, alloca and aligned locals as the host computes them, stack arguments after each, at -O0, -O1, -O2 and -Os in every allocation mode"
