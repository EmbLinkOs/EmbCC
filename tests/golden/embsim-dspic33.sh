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
