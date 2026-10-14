#!/bin/sh
# AVR helper calls (__mulsi3 and the divides) whose two operands sit in
# each other's argument registers: tests/golden/avr-helper-operands/prog.c
# on QEMU's ATmega328P at every level and in every register allocation
# mode, against the host. The operands were loaded b first, which
# overwrote a when a's home was r18-r21 -- the low word of a 64-bit
# argument -- so `(uint32_t)x * m` was m * m (src/arch/avr/codegen.c, the
# helper case, and avr_pmove for when each overwrites the other).
set -u
echo "TEST-MARKER avr-helper-operands"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
HOSTCC=${HOSTCC:-cc}
out=tests/golden/out/avr-helper-operands
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
D=tests/golden/avr-helper-operands
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
echo "avr-helper-operands: multiply, divide and remainder of operands in each other's argument registers as the host computes them, at -O0, -O1, -O2 and -Os in every allocation mode"
