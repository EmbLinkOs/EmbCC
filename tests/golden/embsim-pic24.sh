#!/bin/sh
# embsim-pic24: EmbSim's PIC24F core, its PIC24FJ1024GB610 and the
# Explorer 16/32 with the PIC24FJ1024GB610 plug-in module, built from
# Microchip's device pack (PIC24F_GA_GB_DFP, tools/hostpaths.sh). No
# QEMU models a PIC24 and there is no PIC24 assembler on the build
# machine; the referees are Microchip's own documents.
#
#  1. The ends: a PIC24 board without its pack names what to give it.
#  2. The register file from the pack's EDC: every SFR at the address
#     the pack's linker script for the part (p24FJ1024GB610.gld) gives it.
#  3. The instruction set against the examples of the 16-bit
#     programmer's reference manual (DS70000157G): 227 of them, each its
#     "Before" state set, the instruction executed, the "After" state
#     printed (tests/golden/embsim/pic24-isa.s, .txt; assembled by
#     tests/golden/embsim/pic24-asm.awk into an Intel HEX image).
#  4. The peripherals and the board (pic24-board.s), on explorer16-32-pic24
#     at 30 degrees with S3 pressed: Timer1's interrupt, the LEDs, the
#     buttons, the ADC's conversions of the sensors, the LCD on the EPMP,
#     the 25LC256 on SPI1 behind the Peripheral Pin Select and its lock;
#     and its run recorded and replayed, the board's inputs with it.
#  5. With EMBSIM_THERMO24_ELF, an XC16 build of a thermometer project for
#     the board (MPLAB X, MCC, the LCD, the ADC, the EEPROM): its menu, and
#     with S3 pressed the temperature its own formula gives.
set -u
echo "TEST-MARKER embsim-pic24"
. "$(dirname "$0")/../lib.sh"

EMBSIM=${EMBSIM:-$PWD/embsim}
out=tests/golden/out/embsim-pic24
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
ASM=tests/golden/embsim/pic24-asm.awk

# ---- 1. without the pack ------------------------------------------------------
EMBSIM_SVD_PATH= "$EMBSIM" /dev/null --board explorer16-32-pic24 > "$out/nopack.out" 2>&1 &&
    fail "a PIC24 board ran without its pack"
grep -q 'PIC24FJ1024GB610.PIC.s, from its device pack: give the pack.s directory with --dfp DIR' "$out/nopack.out" ||
    { cat "$out/nopack.out"; fail "a missing pack is not named"; }
echo "embsim-pic24: a PIC24 board without its device pack says to give it (--dfp)"

DFP=$PIC24F_GA_GB_DFP
EDC=$DFP/edc/PIC24FJ1024GB610.PIC
GLD=$DFP/xc16/support/PIC24F/gld/p24FJ1024GB610.gld
if [ ! -f "$EDC" ] || [ ! -f "$GLD" ]; then
    echo "SKIP: embsim-pic24: no PIC24F GA/GB device pack at $DFP (set PIC24F_GA_GB_DFP; Microchip.PIC24F-GA-GB_DFP from packs.download.microchip.com, unzipped)"
    exit 0
fi

# ---- 2. the register file against the linker script -----------------------------
"$EMBSIM" --board pic24fj1024gb610 --dfp "$DFP" --svd-map > "$out/map.txt" ||
    fail "--svd-map of the PIC24FJ1024GB610"
awk 'NR > 2 && !/-- is / { print $4, $2 }' "$out/map.txt" | sort > "$out/map.names"
sed -n 's/^ \([A-Z][A-Z0-9_]*\) *= 0x\([0-9A-Fa-f]*\);.*/\1 \2/p' "$GLD" |
    while read -r n a; do printf '%s 0x%08x\n' "$n" "0x$a"; done | sort > "$out/gld.names"
join "$out/map.names" "$out/gld.names" > "$out/both"
both=$(wc -l < "$out/both" | tr -d ' ')
wrong=$(awk '$2 != $3 { print $1 }' "$out/both" | tr '\n' ' ')
[ -z "$wrong" ] || fail "registers not at the linker script's addresses: $wrong"
extra=$(join -v 1 "$out/map.names" "$out/gld.names" | awk '{ print $1 }' | tr '\n' ' ')
[ -z "$extra" ] || fail "registers the linker script has not: $extra"
[ "$both" -ge 650 ] || fail "only $both registers"
echo "embsim-pic24: the EDC's register file: $both registers, each at the linker script's address"

# ---- 3. the instruction set against the manual's examples -------------------------
awk -f "$ASM" tests/golden/embsim/pic24-isa.s tests/golden/embsim/pic24-isa.s > "$out/isa.hex" ||
    fail "pic24-isa.s does not assemble"
"$EMBSIM" "$out/isa.hex" --board pic24fj1024gb610 --dfp "$DFP" --max-insns 2000000 \
    > "$out/isa.out" 2> "$out/isa.err"; st=$?
[ $st = 0 ] || { cat "$out/isa.err"; fail "pic24-isa.s exits $st"; }
cmp -s "$out/isa.out" tests/golden/embsim/pic24-isa.txt || {
    diff tests/golden/embsim/pic24-isa.txt "$out/isa.out" | head -20
    fail "the instructions do not do what the manual's examples say"; }
n=$(grep -c ':' tests/golden/embsim/pic24-isa.txt)
echo "embsim-pic24: the instruction set: the reference manual's $n examples, each its After state"

# ---- 4. the peripherals, on the Explorer 16/32 ---------------------------------------
awk -f "$ASM" tests/golden/embsim/pic24-board.s tests/golden/embsim/pic24-board.s > "$out/board.hex" ||
    fail "pic24-board.s does not assemble"
"$EMBSIM" "$out/board.hex" --board explorer16-32-pic24 --dfp "$DFP" --max-insns 2000000 \
    --set temperature=30 --set S3=pressed > "$out/board.out" 2> "$out/board.err"; st=$?
[ $st = 0 ] || { cat "$out/board.out" "$out/board.err"; fail "pic24-board.s exits $st"; }
printf 'B\n 0003\n 0002\n 00F8 0200 03E0\nW 005A\n.\n' > "$out/board.want"
cmp -s "$out/board.out" "$out/board.want" || {
    diff "$out/board.want" "$out/board.out"
    fail "the peripherals' results are not the program's expected ones"; }
for l in 'LED D3 on (RA0)' 'LED D5 on (RA2)' 'LED D7 on (RA4)' 'LED D9 on (RA6)' \
         'LED D3 off (RA0)' 'LED D9 off (RA6)'; do
    grep -q "^explorer16-32-pic24: $l at" "$out/board.err" ||
        { cat "$out/board.err"; fail "the board does not report $l"; }
done
grep -q '^explorer16-32-pic24: LCD |PIC24           | at' "$out/board.err" ||
    { cat "$out/board.err"; fail "the LCD does not show what the EPMP wrote"; }
# the run recorded and replayed: the board's inputs (--set) and the pack
# (--dfp) are the recording's, so the replay is the same run
"$EMBSIM" "$out/board.hex" --board explorer16-32-pic24 --dfp "$DFP" --max-insns 2000000 \
    --set temperature=30 --set S3=pressed --record "$out/board.rec" > /dev/null 2>&1 ||
    fail "pic24-board.s does not record"
"$EMBSIM" --replay "$out/board.rec" > "$out/replay.out" 2> "$out/replay.err" ||
    { cat "$out/replay.err"; fail "the recorded run does not replay"; }
cmp -s "$out/replay.out" "$out/board.want" ||
    { cat "$out/replay.out" "$out/replay.err"; fail "the replay's output is not the run's"; }
echo "embsim-pic24: the Explorer 16/32: Timer1's interrupt, the LEDs, S3 and S6, the TC1047A and the potentiometer at 10 and 12 bits, the LCD on the EPMP, the 25LC256 behind the PPS; recorded with its inputs, and replayed"

# ---- 5. an XC16 project: EMBSIM_THERMO24_ELF ----------------------------------------------
# A thermometer for the board, built by MPLAB X and XC16 (MCC's drivers:
# ADC1 scanning AN4 and internal channels, SPI1 to the EEPROM, the EPMP's
# LCD); its image is not in the tree, so this runs only when it is given.
# Its menu, and with S3 pressed the temperature by its own formula: at 25
# degrees the ADC reads 232 (0.75 V), shown as 74 C; at 30, 248: 79 C.
if [ -n "${EMBSIM_THERMO24_ELF:-}" ] && [ -f "$EMBSIM_THERMO24_ELF" ]; then
    "$EMBSIM" "$EMBSIM_THERMO24_ELF" --board explorer16-32-pic24 --dfp "$DFP" \
        --max-insns 4000000 > "$out/t24menu.out" 2> "$out/t24menu.err"
    grep -q 'LCD | 1.Real-time    |' "$out/t24menu.err" &&
    grep -q '|  2.View-History|' "$out/t24menu.err" ||
        { cat "$out/t24menu.err"; fail "the thermometer's menu is not on the LCD"; }
    for t in 25 30; do
        "$EMBSIM" "$EMBSIM_THERMO24_ELF" --board explorer16-32-pic24 --dfp "$DFP" \
            --set S3=pressed --set temperature=$t --max-insns 30000000 \
            > "$out/t24-$t.out" 2> "$out/t24-$t.err"
        case $t in 25) want=74 ;; *) want=79 ;; esac
        grep -q "LCD |$want C " "$out/t24-$t.err" ||
            { cat "$out/t24-$t.err"; fail "the thermometer at $t degrees does not show $want C"; }
    done
    echo "embsim-pic24: the XC16 thermometer: its menu, and 74 C and 79 C at 25 and 30 degrees, as its formula has them"
else
    echo "embsim-pic24: no EMBSIM_THERMO24_ELF: the XC16 thermometer was not run"
fi
