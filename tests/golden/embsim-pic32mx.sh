#!/bin/sh
# embsim-pic32mx: EmbSim's PIC32MX boards -- the PIC32MX795F512L and the
# Explorer 16/32 with its PIM, any MX part by name -- built from
# Microchip's PIC32MX device pack (PIC32MX_DFP, tools/hostpaths.sh). No
# QEMU models a PIC32, so the referees are the pack and the family
# reference manual (DS61108 interrupts, DS61112 oscillator, DS61113 the
# M4K, DS61120 ports, DS61105 timers).
#
#  1. The register file from the pack's EDC (its ATDF puts 35 peripherals
#     0xC low and leaves out port G and the change notices): every SFR,
#     and its CLR, SET and INV registers, at the address the pack's device
#     header gives it; the header's other names alias registers of the
#     map (U1A..., EMACx...), and the EDC gives six CLR/SET/INV registers
#     the header leaves out (the USB interrupt flags').
#  2. An EmbCC program (tests/golden/embsim/pic32mx-fw.c), its interrupt
#     stubs written here, one in each vector's 32-byte slot, run on the
#     part alone: the M4K's identity, DEVID, FPBDIV, single- and
#     multi-vector modes (INTSTAT, the shadow set by SS0 and FSRSSEL, the
#     natural order by vector, not by request, subpriority), Timer1 on the
#     peripheral bus (FPBDIV 4), a shared
#     vector (UART1's), the change notices, CNPUE, AD1PCFG, SYSKEY's lock.
#     Its output must be the record (pic32mx-fw.txt).
#  3. An EmbCC program (pic32mx-board.c) on explorer16-32-pic32mx with S3
#     pressed at 30 degrees: the buttons, the LEDs, ADC1's conversions of
#     the TC1047A and the potentiometer, the 25LC256 on SPI2, the LCD on
#     the PMP -- the record (pic32mx-board.txt) and the board's report.
#  4. Another part by name: a program for the PIC32MX250F128B (its own
#     DEVID, 32 KiB of RAM and 128 KiB of flash, a bus error past the
#     RAM); and the ends: a board without its pack names what to give it.
set -u
echo "TEST-MARKER embsim-pic32mx"
. "$(dirname "$0")/../lib.sh"

EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBSIM=${EMBSIM:-$PWD/embsim}
out=tests/golden/out/embsim-pic32mx
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"

# ---- 4. without the pack ----------------------------------------------------
EMBSIM_SVD_PATH= "$EMBSIM" /dev/null --board explorer16-32-pic32mx > "$out/nopack.out" 2>&1 &&
    fail "a PIC32MX board ran without its pack"
grep -q 'PIC32MX795F512L.PIC.s, from its device pack: give the pack.s directory with --dfp DIR' "$out/nopack.out" ||
    { cat "$out/nopack.out"; fail "a missing pack is not named"; }
echo "embsim-pic32mx: a PIC32MX board without its device pack says to give it (--dfp)"

DFP=$PIC32MX_DFP
EDC=$DFP/edc/PIC32MX795F512L.PIC
HDR=$DFP/include/proc/p32mx795f512l.h
if [ ! -f "$EDC" ] || [ ! -f "$HDR" ]; then
    echo "SKIP: embsim-pic32mx: no PIC32MX device pack at $DFP (set PIC32MX_DFP; Microchip.PIC32MX_DFP from packs.download.microchip.com, unzipped)"
    exit 0
fi

# ---- 1. the register file against the device header ---------------------------
"$EMBSIM" --board pic32mx795f512l --dfp "$DFP" --svd-map > "$out/map.txt" ||
    fail "--svd-map of the PIC32MX795F512L"
awk 'NR > 1 && !/-- is / { print $4, $2 }' "$out/map.txt" | sort > "$out/map.names"
sed -n 's/^extern volatile [a-z0-9_]* *\([A-Za-z0-9_]*\) __attribute__((section("sfrs"), *address(0x\(..\)\(......\))).*/\1 \2 \3/p' \
    "$HDR" | while read -r n hi lo; do
        printf '%s 0x%02x%s\n' "$n" "$((0x$hi & 0x1f))" "$(echo "$lo" | tr 'A-F' 'a-f')"
    done | sort > "$out/hdr.names"
join "$out/map.names" "$out/hdr.names" > "$out/both"
both=$(wc -l < "$out/both" | tr -d ' ')
wrong=$(awk '$2 != $3 { print $1 }' "$out/both")
[ -z "$wrong" ] ||
    { awk '$2 != $3' "$out/both" | head; fail "registers not at the header's addresses: $(echo $wrong | head -c 300)"; }
sci=$(grep -c '\(CLR\|SET\|INV\) ' "$out/both")
[ "$both" -ge 2900 ] && [ "$sci" -ge 2100 ] ||
    fail "only $both registers ($sci CLR/SET/INV) are the header's"
extra=$(join -v 1 "$out/map.names" "$out/hdr.names" | awk '{ print $1 }' | tr '\n' ' ')
[ "$extra" = "U1EIRINV U1EIRSET U1IRINV U1IRSET U1OTGIRINV U1OTGIRSET " ] ||
    fail "registers the header has not: $extra"
# the header's other names: each at an address the map has, but the
# configuration words (in boot flash)
awk '{ print $2 }' "$out/map.names" | sort -u > "$out/map.addrs"
join -v 2 "$out/map.names" "$out/hdr.names" | awk '{ print $2, $1 }' | sort > "$out/hdr.only"
lost=$(join -v 1 "$out/hdr.only" "$out/map.addrs" | awk '{ print $2 }' | tr '\n' ' ')
[ "$lost" = "DEVCFG3 DEVCFG2 DEVCFG1 DEVCFG0 " ] ||
    fail "the header's registers at no address of the map: $lost"
echo "embsim-pic32mx: the EDC's register file: $both registers at the header's addresses ($sci of them CLR, SET and INV), the header's others aliases"

# ---- 2. a program on the part --------------------------------------------------
crt=$(dirname "$EMBCC")/build/libc/mipsel-none-elf/pic32-crt0.o
if [ ! -f "$crt" ]; then
    echo "SKIP: embsim-pic32mx: no $crt (make rt-embedded): the programs were not built"
    exit 0
fi
P="--target=mipsel-none-elf --mcu=32MX795F512L --dfp=$DFP"
{
    echo '    .set noreorder'
    echo '    .set noat'
    for v in 0 1 2 4 5 8 24 26; do
        cat <<EOF
    .section .vector_$v, code, keep
    .globl __vector_dispatch_$v
__vector_dispatch_$v:
    j       __isr_body_$v
    nop
    .text
__isr_body_$v:
    rdpgpr  \$29, \$29
    addiu   \$29, \$29, -80
    sw      \$1, 0(\$29)
    sw      \$2, 4(\$29)
    sw      \$3, 8(\$29)
    sw      \$4, 12(\$29)
    sw      \$5, 16(\$29)
    sw      \$6, 20(\$29)
    sw      \$7, 24(\$29)
    sw      \$8, 28(\$29)
    sw      \$9, 32(\$29)
    sw      \$10, 36(\$29)
    sw      \$11, 40(\$29)
    sw      \$12, 44(\$29)
    sw      \$13, 48(\$29)
    sw      \$14, 52(\$29)
    sw      \$15, 56(\$29)
    sw      \$24, 60(\$29)
    sw      \$25, 64(\$29)
    sw      \$31, 68(\$29)
    mfhi    \$26
    sw      \$26, 72(\$29)
    mflo    \$26
    sw      \$26, 76(\$29)
    la      \$26, isr_$v
    jalr    \$26
    nop
    lw      \$26, 76(\$29)
    mtlo    \$26
    lw      \$26, 72(\$29)
    mthi    \$26
    lw      \$1, 0(\$29)
    lw      \$2, 4(\$29)
    lw      \$3, 8(\$29)
    lw      \$4, 12(\$29)
    lw      \$5, 16(\$29)
    lw      \$6, 20(\$29)
    lw      \$7, 24(\$29)
    lw      \$8, 28(\$29)
    lw      \$9, 32(\$29)
    lw      \$10, 36(\$29)
    lw      \$11, 40(\$29)
    lw      \$12, 44(\$29)
    lw      \$13, 48(\$29)
    lw      \$14, 52(\$29)
    lw      \$15, 56(\$29)
    lw      \$24, 60(\$29)
    lw      \$25, 64(\$29)
    lw      \$31, 68(\$29)
    addiu   \$29, \$29, 80
    eret
EOF
    done
} > "$out/isr.S"
# shellcheck disable=SC2086
"$EMBCC" $P -O1 tests/golden/embsim/pic32mx-fw.c "$out/isr.S" -o "$out/fw.elf" 2> "$out/build.err" ||
    { cat "$out/build.err"; fail "pic32mx-fw.c does not build"; }
"$EMBSIM" "$out/fw.elf" --board pic32mx795f512l --dfp "$DFP" --until '==DONE==' \
    --max-insns 5000000 > "$out/fw.out" 2> "$out/fw.err"; st=$?
[ $st = 0 ] || { cat "$out/fw.out" "$out/fw.err"; fail "pic32mx-fw.c on pic32mx795f512l exits $st"; }
cmp -s "$out/fw.out" tests/golden/embsim/pic32mx-fw.txt || {
    diff tests/golden/embsim/pic32mx-fw.txt "$out/fw.out" | head -20
    fail "pic32mx-fw.c's output is not pic32mx-fw.txt"; }
grep -q 'to OSC+0x000 is ignored: it is locked' "$out/fw.err" ||
    { cat "$out/fw.err"; fail "a locked OSCCON write is not reported"; }
echo "embsim-pic32mx: an EmbCC program on pic32mx795f512l: $(($(wc -l < "$out/fw.out") - 2)) results as the record has them (the M4K, DEVID, FPBDIV, single and multi-vector modes, shadow sets, natural order and subpriority, Timer1, a shared vector, change notices, CNPUE, AD1PCFG, SYSKEY)"

# ---- 3. a program on the Explorer 16/32 ----------------------------------------
# shellcheck disable=SC2086
"$EMBCC" $P -O1 tests/golden/embsim/pic32mx-board.c -o "$out/board.elf" 2> "$out/bbuild.err" ||
    { cat "$out/bbuild.err"; fail "pic32mx-board.c does not build"; }
"$EMBSIM" "$out/board.elf" --board explorer16-32-pic32mx --dfp "$DFP" --until '==DONE==' \
    --set S3=pressed --set temperature=30 --max-insns 20000000 \
    > "$out/board.out" 2> "$out/board.err"; st=$?
[ $st = 0 ] || { cat "$out/board.out" "$out/board.err"; fail "pic32mx-board.c exits $st"; }
cmp -s "$out/board.out" tests/golden/embsim/pic32mx-board.txt || {
    diff tests/golden/embsim/pic32mx-board.txt "$out/board.out" | head -20
    fail "pic32mx-board.c's output is not pic32mx-board.txt"; }
for l in 'LED D4 on (RA1)' 'LED D6 on (RA3)' 'LED D7 on (RA4)' 'LED D9 on (RA6)' \
         'LED D4 off (RA1)'; do
    grep -q "^explorer16-32-pic32mx: $l at" "$out/board.err" ||
        { cat "$out/board.err"; fail "the board does not report $l"; }
done
[ "$(grep -c ' LED ' "$out/board.err")" = 5 ] ||
    { cat "$out/board.err"; fail "the board reports LEDs the program did not drive"; }
grep -q '^explorer16-32-pic32mx: LCD |EmbSim          |' "$out/board.err" &&
    grep -q '^explorer16-32-pic32mx:     |PIC32MX795F512L |' "$out/board.err" ||
    { cat "$out/board.err"; fail "the LCD's two lines are not what the program wrote"; }
echo "embsim-pic32mx: an EmbCC program on explorer16-32-pic32mx: the buttons, the LEDs, ADC1's two conversions, the 25LC256 on SPI2, the LCD on the PMP"

# ---- 4. another part, by name ----------------------------------------------------
if [ -f "$DFP/edc/PIC32MX250F128B.PIC" ]; then
    "$EMBCC" --target=mipsel-none-elf --mcu=32MX250F128B --dfp="$DFP" -O1 -DRAM=0x8000 -DFLASH=0x20000 \
        tests/golden/embsim/pic32mx-part.c -o "$out/part.elf" 2> "$out/pbuild.err" ||
        { cat "$out/pbuild.err"; fail "pic32mx-part.c does not build"; }
    "$EMBSIM" "$out/part.elf" --board pic32mx250f128b --dfp "$DFP" --until '==DONE==' \
        --max-insns 1000000 > "$out/part.out" 2> "$out/part.err"; st=$?
    [ $st = 3 ] && grep -q 'sdbbp' "$out/part.err" ||
        { cat "$out/part.out" "$out/part.err"; fail "pic32mx-part.c on pic32mx250f128b exits $st, not at the bus error"; }
    cmp -s "$out/part.out" tests/golden/embsim/pic32mx-part.txt || {
        diff tests/golden/embsim/pic32mx-part.txt "$out/part.out"
        fail "pic32mx-part.c's output is not pic32mx-part.txt"; }
    echo "embsim-pic32mx: another part by name, pic32mx250f128b: its DEVID, 32 KiB of RAM and 128 KiB of flash, as its EDC has them"
else
    echo "SKIP: embsim-pic32mx: no PIC32MX250F128B.PIC in the pack: another part was not run"
fi
