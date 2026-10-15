#!/bin/sh
# embsim-dspic33: EmbSim's dsPIC33EP core -- the PIC24 core with the
# PIC24E's instructions and the dsPIC's DSP engine -- on the
# dsPIC33EP512MU810 and its family by name, built from Microchip's dsPIC33E
# device pack (DSPIC33E_DFP, tools/hostpaths.sh). No QEMU models a dsPIC
# and there is no dsPIC assembler on the build machine; the referees are
# Microchip's documents: the 16-bit programmer's reference manual
# (DS70000157G) and the dsPIC33F family reference manual's CPU (DS70204)
# and data memory (DS70202) sections.
#
#  1. The ends: a dsPIC33EP part without its pack names what to give it.
#  2. The register file from the pack's EDC (its core's registers are no
#     peripheral; Timer1 and Timer2-9, the PPS's RPORx and RPINRx, UART1-2
#     and UART3-4, each one peripheral though the EDC splits them): every
#     SFR at the address the pack's linker script (p33EP512MU810.gld)
#     gives it, but the debugger's, which the script has not.
#  3. The DSP engine and the PIC24E's instructions against the reference
#     manual's examples (tests/golden/embsim/dspic33-isa.s, .txt; assembled
#     by pic24-asm.awk): MAC, MPY, MPY.N, MSC, the squares, ED, EDAC, CLR
#     and MOVSAC with their X and Y prefetches and write-backs; ADD, SUB,
#     NEG, LAC, SAC, SAC.R, SFTAC under each saturation mode; the MCU
#     multiplies into an accumulator, MULW; DIVF; DO, at the manual's
#     addresses; CP and CPB with an 8-bit literal, LNK and ULNK with
#     CORCON.SFA, MOVPAG; and the data memory section's modulo buffers
#     (Figures 3-5, 3-6) and bit-reversed copy (Example 3-6).
#  4. What the examples leave out, by each instruction's and register's
#     description (dspic33-dsp.s): the reset's values, the PIC24E's flow,
#     DO loops nested and the DO stack's overflow, the saturation modes
#     at their limits, rounding, the multiplier's modes, the math error
#     traps, the address generators' modulo and bit-reversed limits.
#  5. --disassemble of the examples beside the manual's text, and of the
#     DO, flow and paging instructions.
#  6. The family's other parts by name (dspic33-part.s): their RAM, Y
#     space and FOSCSEL as their EDCs place them; a PIC24EP without the
#     DSP engine; a part of another family refused.
#  7. The Explorer 16/32 with the dsPIC33EP512MU810 PIM (dspic33-board.s):
#     the clock through the PLL, Timer1, the LEDs, the buttons, ADC1, the
#     LCD on the PMP, the EEPROM on SPI2; recorded and replayed.
set -u
echo "TEST-MARKER embsim-dspic33"
. "$(dirname "$0")/../lib.sh"

EMBSIM=${EMBSIM:-$PWD/embsim}
out=tests/golden/out/embsim-dspic33
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
ASM=tests/golden/embsim/pic24-asm.awk

# ---- 1. without the pack ------------------------------------------------------
EMBSIM_SVD_PATH= "$EMBSIM" /dev/null --board dspic33ep512mu810 > "$out/nopack.out" 2>&1 &&
    fail "a dsPIC33EP part ran without its pack"
grep -q 'needs DSPIC33EP512MU810.PIC, from its device pack (Microchip.dsPIC33E-GM-GP-MC-GU-MU_DFP)' "$out/nopack.out" ||
    { cat "$out/nopack.out"; fail "a missing pack is not named"; }
echo "embsim-dspic33: a dsPIC33EP part without its device pack says to give it (--dfp)"

DFP=$DSPIC33E_DFP
EDC=$DFP/edc/DSPIC33EP512MU810.PIC
GLD=$DFP/xc16/support/dsPIC33E/gld/p33EP512MU810.gld
if [ ! -f "$EDC" ] || [ ! -f "$GLD" ]; then
    echo "SKIP: embsim-dspic33: no dsPIC33E device pack at $DFP (set DSPIC33E_DFP; Microchip.dsPIC33E-GM-GP-MC-GU-MU_DFP from packs.download.microchip.com, unzipped)"
    exit 0
fi

# ---- 2. the register file against the linker script ------------------------------
"$EMBSIM" --board dspic33ep512mu810 --dfp "$DFP" --svd-map > "$out/map.txt" ||
    fail "--svd-map of the dsPIC33EP512MU810"
awk 'NR > 2 && !/-- is / { print $4, $2 }' "$out/map.txt" | sort > "$out/map.names"
sed -n 's/^ \([A-Z][A-Z0-9_]*\) *= 0x\([0-9A-Fa-f]*\);.*/\1 \2/p' "$GLD" |
    while read -r n a; do printf '%s 0x%08x\n' "$n" "0x$a"; done | sort > "$out/gld.names"
join "$out/map.names" "$out/gld.names" > "$out/both"
both=$(wc -l < "$out/both" | tr -d ' ')
wrong=$(awk '$2 != $3 { print $1 }' "$out/both" | tr '\n' ' ')
[ -z "$wrong" ] || fail "registers not at the linker script's addresses: $wrong"
extra=$(join -v 1 "$out/map.names" "$out/gld.names" | awk '{ print $1 }' | tr '\n' ' ')
[ "$extra" = "DFTADD DFTDATA DFTDATA_INC DPCL DPCU FEXL FEXU VISI _APPIN _APPO " ] ||
    fail "registers the linker script has not, beyond the debugger's: $extra"
[ "$both" -ge 900 ] || fail "only $both registers"
for p in TMR:T9CON PPS:RPINR0 PPS:RPOR0 UART:U4MODE UART:U1MODE INT:INTCON1; do
    grep -q "^${p%%:*} .* ${p#*:} " "$out/map.txt" ||
        fail "${p#*:} is not in the peripheral ${p%%:*}"
done
grep -q '^[A-Z_0-9]* 0x000000[0-5][0-9a-f] ' "$out/map.txt" && fail "the core's registers made a peripheral"
echo "embsim-dspic33: the EDC's register file: $both registers, each at the linker script's address; the core's none, Timer1-9, the PPS, UART1-4 one peripheral each"

# ---- 3. the instruction set against the manual's examples -------------------------
awk -f "$ASM" tests/golden/embsim/dspic33-isa.s tests/golden/embsim/dspic33-isa.s > "$out/isa.hex" ||
    fail "dspic33-isa.s does not assemble"
"$EMBSIM" "$out/isa.hex" --board dspic33ep512mu810 --dfp "$DFP" --max-insns 2000000 \
    > "$out/isa.out" 2> "$out/isa.err"; st=$?
[ $st = 0 ] || { cat "$out/isa.err"; fail "dspic33-isa.s exits $st"; }
cmp -s "$out/isa.out" tests/golden/embsim/dspic33-isa.txt || {
    diff tests/golden/embsim/dspic33-isa.txt "$out/isa.out" | head -20
    fail "the instructions do not do what the manual's examples say"; }
n=$(grep -c ':' tests/golden/embsim/dspic33-isa.txt)
echo "embsim-dspic33: the DSP engine and the PIC24E's instructions: the reference manual's and the data memory section's $n examples, each its After state"

# ---- 4. beyond the examples ---------------------------------------------------------
# by each instruction's and register's description: the reset's values;
# the PIC24E's flow and
# the stack frame flag, compare and branch, BRA OA..SB, REPEAT's 15 and 16
# bits, CP's 8-bit literal, MOVPAG; DO loops nested, the DO stack's
# overflow (the soft trap, DOOVR), EDT and DCOUNT ending a loop; the
# saturation modes, SA's stickiness, -1.0 * -1.0; rounding and SATDW; the
# multiplier's unsigned and mixed modes; the math error traps (SFTACERR,
# OVATE/OVAERR, COVTE/COVAERR); the Y modulo buffer, the other space read
# as 0, an X modulo offset address, bit-reversed addressing's limits
awk -f "$ASM" tests/golden/embsim/dspic33-dsp.s tests/golden/embsim/dspic33-dsp.s > "$out/dsp.hex" ||
    fail "dspic33-dsp.s does not assemble"
"$EMBSIM" "$out/dsp.hex" --board dspic33ep512mu810 --dfp "$DFP" --max-insns 2000000 \
    > "$out/dsp.out" 2> "$out/dsp.err" ||
    { cat "$out/dsp.err"; fail "dspic33-dsp.s ends with status $?"; }
printf '%s\n' \
    'I 1000 0020 0001 0001' \
    'F 0000 0001 0004 0000 0010 00F0 00AA 0014 0006 4001 8002 0103 01FF 00AB 0001' \
    'D 000C 0000 0000 0010 0010 0010 0000 0003 0002 0103 0000 1234' \
    'S AC00 FF80 0000 0000 AC00 007F FFFF FFFF 2400 0000 7FFF FFFF 2400 0000 8800 0000 8800 0000 8000 0000 2400 0000 7FFF FFFF' \
    'R 1234 1235 1236 1235 1234 8918 7FFF' \
    'M 0000 FFFE 0001 FFFF FFFF 0001 0000 2000 0000 0000 1230 0000' \
    'T 0001 0090 0000 1234 5678 0002 4410 0003 1110 0003 0003' \
    'A 1111 9002 0000 0000 0000 1100 110F 7777 1100 1103 1100 1301 12FE 12E0 12E2' \
    '.' > "$out/dsp.want"
cmp -s "$out/dsp.out" "$out/dsp.want" || {
    diff "$out/dsp.want" "$out/dsp.out"; fail "dspic33-dsp.s: not what the descriptions say"; }
n=$(grep -c 'a DO loop nested past the DO stack' "$out/dsp.err")
[ "$n" = 10 ] || { cat "$out/dsp.err"; fail "the DO stack's overflows are not told (10 of them, then quiet): $n"; }
echo "embsim-dspic33: beyond the examples: the PIC24E's flow and SFA, CPBxx, BRA OA..SB, REPEAT, MOVPAG; DO nesting, its stack's overflow, EDT; the saturation modes, rounding, SATDW, the multiplier's modes; the math error traps; the Y modulo buffer, the other space, bit-reversed addressing's limits"

# ---- 5. --disassemble -------------------------------------------------------------
# the examples beside the manual's text; the DO, flow and paging instructions
{ echo "        .org 0x200"
  grep '^ *\.word 0x[0-9a-fA-F]* *; ' tests/golden/embsim/dspic33-isa.s | sed 's/^ *\(\.word 0x[0-9a-fA-F]*\).*/        \1/'
} > "$out/dis.s"
grep '^ *\.word 0x[0-9a-fA-F]* *; ' tests/golden/embsim/dspic33-isa.s | sed 's/^[^;]*; *//' > "$out/dis.manual"
awk -f "$ASM" "$out/dis.s" "$out/dis.s" > "$out/dis.hex" || fail "the examples do not assemble"
"$EMBSIM" "$out/dis.hex" --board dspic33ep512mu810 --dfp "$DFP" --disassemble > "$out/dis.out" ||
    fail "--disassemble of a dsPIC HEX image"
sed -n 's/^  [0-9a-f]\{6\}:  [0-9a-f]\{6\} *//p' "$out/dis.out" > "$out/dis.text"
paste -d'|' "$out/dis.manual" "$out/dis.text" |
    awk -F'|' '{ printf "%-46s| %s\n", $1, $2 }' > "$out/dis.both"
cmp -s "$out/dis.both" tests/golden/embsim/dspic33-dis.txt || {
    diff tests/golden/embsim/dspic33-dis.txt "$out/dis.both" | head -20
    fail "--disassemble: the examples are not dspic33-dis.txt"; }
"$EMBSIM" "$out/isa.hex" --board dspic33ep512mu810 --dfp "$DFP" --disassemble > "$out/isadis.out" ||
    fail "--disassemble of dspic33-isa.s"
"$EMBSIM" "$out/dsp.hex" --board dspic33ep512mu810 --dfp "$DFP" --disassemble > "$out/dspdis.out" ||
    fail "--disassemble of dspic33-dsp.s"
for l in '002000:  080005 000003  do #5, 0x00200a' '01c000:  080160 000008  do #352, 0x01c014' \
         '003000:  088000 000006  do w0, 0x003010'; do
    grep -qF "  $l" "$out/isadis.out" || fail "--disassemble: not '$l'"
done
sed 's/^  [0-9a-f]*:  //; s/, 0x[0-9a-f]*$/, 0x/' "$out/dspdis.out" > "$out/dspdis.text"
for l in '010206         rcall w6' '010407         goto w7' '010607         bra w7' \
         '019c02         goto.l w2' '01a804         call.l w4' '094000         repeat #16384' \
         '098001         repeat w1' '0c0001         bra oa, 0x' '0d0001         bra ob, 0x' \
         '0e0001         bra sa, 0x' '0f0001         bra sb, 0x' 'c80051         sftac a, #17' \
         'e12368         cp w4, #200' 'e69423         cpblt.b w2, w3, 0x' \
         'e70021         cpbne w0, w1, 0x' 'e78011         cpseq w0, w1' \
         'e78021         cpbeq w0, w1, 0x' 'fec5ff         movpag #0x1ff, dswpag' \
         'fec8ab         movpag #0xab, tblpag' 'fed402         movpag w2, dswpag'; do
    grep -qxF "$l" "$out/dspdis.text" || fail "--disassemble: not '$l' in dspic33-dsp.s"
done
echo "embsim-dspic33: --disassemble: the manual's $(wc -l < "$out/dis.both" | tr -d ' ') examples, each as dspic33-dis.txt has it beside the manual's text; DO at the manual's addresses, CALL.L, GOTO.L, the PIC24E's computed branches, CPBxx, BRA OA..SB, REPEAT, MOVPAG"

# ---- 6. the family's other parts, by name ------------------------------------------
# one image (dspic33-part.s) on parts whose memory differs, each line its
# own: the FOSCSEL its EDC places (the configuration memory's, or a 64
# KiB part's last page's), the RAM's end, the Y space's start; a PIC24EP
# has no DSP engine; a part of another family is refused by name
awk -f "$ASM" tests/golden/embsim/dspic33-part.s tests/golden/embsim/dspic33-part.s > "$out/part.hex" ||
    fail "dspic33-part.s does not assemble"
for b in 'dspic33ep512mu810:P 1000 0000 9ABC:Y 0000 0000' \
         'dspic33ep64mc202:P 1000 0001 9ABC:Y 0000 5678' \
         'dspic33ep32mc202:P 1000 0007 0000:Y 1234 0000'; do
    part=${b%%:*}; rest=${b#*:}
    "$EMBSIM" "$out/part.hex" --board "$part" --dfp "$DFP" --max-insns 100000 \
        > "$out/part.out" 2>&1 || { cat "$out/part.out"; fail "dspic33-part.s on $part"; }
    printf '%s\n%s\n.\n' "${rest%%:*}" "${rest#*:}" > "$out/part.want"
    cmp -s "$out/part.out" "$out/part.want" ||
        { diff "$out/part.want" "$out/part.out"; fail "dspic33-part.s on $part: not its EDC's memory"; }
done
"$EMBSIM" "$out/part.hex" --board pic24ep512gu810 --dfp "$DFP" --max-insns 100000 \
    > "$out/pic24e.out" 2>&1; st=$?
[ $st = 3 ] && grep -q '^P 1000 0000 9ABC$' "$out/pic24e.out" &&
    grep -q 'lockup: 0xc70102 is not a PIC24E instruction' "$out/pic24e.out" ||
    { cat "$out/pic24e.out"; fail "the PIC24EP512GU810 ran a DSP instruction, or not its memory (status $st)"; }
for b in dspic33fj256gp710 dspic33ep512mz810; do
    "$EMBSIM" "$out/part.hex" --board $b --dfp "$DFP" > "$out/other.out" 2>&1 &&
        fail "$b ran"
    grep -q 'dsPIC33EP.s and PIC24EP.s by name' "$out/other.out" ||
        { cat "$out/other.out"; fail "$b, of no family EmbSim has, is not refused by name"; }
done
echo "embsim-dspic33: the family's other parts by name: the dsPIC33EP64MC202's and dsPIC33EP32MC202's RAM, Y space and FOSCSEL (in program memory), the PIC24EP512GU810 without the DSP engine; another family's refused"

# ---- 7. the Explorer 16/32 with the dsPIC33EP512MU810 PIM -------------------------------
# dspic33-board.s at 30 degrees with S3 pressed, on the FRC with the PLL
# (the reports' times its clock's): Timer1's interrupt, the
# LEDs, S3 and S6, the TC1047A and the potentiometer through ADC1 at 10
# and 12 bits and as a signed fraction, the LCD on the PMP, the 25LC256 on
# SPI2 (CS RD12) and SPIROV; its run recorded with the board's inputs and
# replayed
awk -f "$ASM" tests/golden/embsim/dspic33-board.s tests/golden/embsim/dspic33-board.s > "$out/board.hex" ||
    fail "dspic33-board.s does not assemble"
"$EMBSIM" "$out/board.hex" --board explorer16-32-dspic33 --dfp "$DFP" --max-insns 3000000 \
    --set temperature=30 --set S3=pressed > "$out/board.out" 2> "$out/board.err"; st=$?
[ $st = 0 ] || { cat "$out/board.out" "$out/board.err"; fail "dspic33-board.s exits $st"; }
printf 'B\n 0003\n 0002\n 00F8 0200 03E0 BE00\n 8080 003C 00FF 80C1 003C 80C0\n.\n' > "$out/board.want"
cmp -s "$out/board.out" "$out/board.want" || {
    diff "$out/board.want" "$out/board.out"
    fail "the peripherals' results are not the program's expected ones"; }
for l in 'LED D3 on (RA0)' 'LED D5 on (RA2)' 'LED D7 on (RA4)' 'LED D9 on (RA6)' \
         'LED D3 off (RA0)' 'LED D9 off (RA6)' 'LCD |dsPIC33         |'; do
    grep -qF "explorer16-32-dspic33: $l at" "$out/board.err" ||
        { cat "$out/board.err"; fail "the board does not report $l"; }
done
# the time, at the PLL's 23.03 MHz instruction clock (FOSCSEL's FRC with
# the PLL, PLLFBD and CLKDIV as reset leaves them)
grep -qF 'explorer16-32-dspic33: LCD |dsPIC33         | at 0.094 ms' "$out/board.err" ||
    { cat "$out/board.err"; fail "the LCD's frame is not at 0.094 ms: the clock is not the PLL's"; }
"$EMBSIM" "$out/board.hex" --board explorer16-32-dspic33 --dfp "$DFP" --max-insns 3000000 \
    --set temperature=30 --set S3=pressed --record "$out/board.rec" > /dev/null 2>&1 ||
    fail "dspic33-board.s does not record"
"$EMBSIM" --replay "$out/board.rec" > "$out/replay.out" 2> "$out/replay.err" ||
    { cat "$out/replay.err"; fail "the recorded run does not replay"; }
cmp -s "$out/replay.out" "$out/board.want" ||
    { cat "$out/replay.out" "$out/replay.err"; fail "the replay's output is not the run's"; }
echo "embsim-dspic33: the Explorer 16/32's dsPIC33EP512MU810 PIM: Timer1's interrupt, the LEDs, S3 and S6, the TC1047A and the potentiometer through ADC1 at 10 and 12 bits, the LCD on the PMP, the 25LC256 on SPI2; recorded with its inputs, and replayed"
