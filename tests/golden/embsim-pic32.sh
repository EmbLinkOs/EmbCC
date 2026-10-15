#!/bin/sh
# embsim-pic32: EmbSim's PIC32MZ2048EFH100, Explorer 16/32 and Curiosity boards,
# built from Microchip's device pack (PIC32MZ_EF_DFP, tools/hostpaths.sh).
# No QEMU models a PIC32, so the referees are the pack itself and the
# part's documentation.
#
#  1. The register file from the pack's ATDF: every SFR, and each one's
#     CLR, SET and INV register, at the address the pack's device header
#     (include/proc/p32mz2048efh100.h) gives it. Microchip's two files
#     disagree on one register, USBCRCON, which is listed.
#  2. A PIC32 program built by EmbCC with its own startup
#     (tests/golden/embsim/pic32-fw.c, its interrupt handlers written
#     here in assembly), run on explorer16-32 with the temperature at 30
#     degrees and S4 pressed: the DSP ASE usable after the startup, the
#     EVIC's vectors and priorities and shadow register sets, Timer1,
#     the core timer, a 32-bit Timer2/3, vector 213 through OFF213, the
#     ports, the ADC's conversions of the board's sensors, the LCD on the
#     PMP, SYSKEY's lock, DEVID, the 25LC256 EEPROM on the PIM's pins
#     (clocked by the program: RA5 has no SPI clock). Its output must be
#     the record
#     (tests/golden/embsim/pic32-fw.txt), and the board's report -- the
#     LEDs and the LCD's lines -- what the program drove.
#     And a program on curiosity-pic32mz-ef-2 (its PIC32MZ2048EFM144):
#     ANSELx out of reset, its switches, its LEDs
#     (tests/golden/embsim/pic32-curiosity.c, .txt).
#     And another part of the family by name (pic32mz1024efe064): its
#     DEVID, its RAM's and flash's sizes (tests/golden/embsim/
#     pic32-part.c, .txt).
#  3. The ends: a PIC32 board without its pack names what to give it.
#  4. With EMBSIM_THERMO_ELF, a Harmony 3 + FreeRTOS thermometer built by
#     EmbCC: the LCD's temperature at three temperatures, its LEDs.
set -u
echo "TEST-MARKER embsim-pic32"
. "$(dirname "$0")/../lib.sh"

EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBSIM=${EMBSIM:-$PWD/embsim}
out=tests/golden/out/embsim-pic32
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"

# ---- 3. without the pack ----------------------------------------------------
EMBSIM_SVD_PATH= "$EMBSIM" /dev/null --board explorer16-32 > "$out/nopack.out" 2>&1 &&
    fail "a PIC32 board ran without its pack"
grep -q 'PIC32MZ2048EFH100.atdf.s, from its device pack: give the pack.s directory with --dfp DIR' "$out/nopack.out" ||
    { cat "$out/nopack.out"; fail "a missing pack is not named"; }
echo "embsim-pic32: a PIC32 board without its device pack says to give it (--dfp)"

DFP=$PIC32MZ_EF_DFP
ATDF=$DFP/atdf/PIC32MZ2048EFH100.atdf
HDR=$DFP/include/proc/p32mz2048efh100.h
if [ ! -f "$ATDF" ] || [ ! -f "$HDR" ]; then
    echo "SKIP: embsim-pic32: no PIC32MZ EF device pack at $DFP (set PIC32MZ_EF_DFP; Microchip.PIC32MZ-EF_DFP from packs.download.microchip.com, unzipped)"
    exit 0
fi

# ---- 1. the register file against the device header ---------------------------
"$EMBSIM" --board pic32mz2048efh100 --dfp "$DFP" --svd-map > "$out/map.txt" ||
    fail "--svd-map of the PIC32MZ2048EFH100"
# NAME ADDRESS, physical, from each
awk 'NR > 1 && !/-- is / { print $4, $2 }' "$out/map.txt" | sort > "$out/map.names"
sed -n 's/^extern volatile [a-z0-9_]* *\([A-Za-z0-9_]*\) __attribute__((section("sfrs"), *address(0x\(..\)\(......\))).*/\1 \2 \3/p' \
    "$HDR" | while read -r n hi lo; do
        printf '%s 0x%02x%s\n' "$n" "$((0x$hi & 0x1f))" "$(echo "$lo" | tr 'A-F' 'a-f')"
    done | sort > "$out/hdr.names"
join "$out/map.names" "$out/hdr.names" > "$out/both"
both=$(wc -l < "$out/both" | tr -d ' ')
wrong=$(awk '$2 != $3 { print $1 }' "$out/both")
[ "$wrong" = USBCRCON ] ||
    { awk '$2 != $3' "$out/both" | head; fail "registers not at the header's addresses: $(echo $wrong | head -c 300)"; }
sci=$(grep -c '\(CLR\|SET\|INV\) ' "$out/both")
[ "$both" -ge 4200 ] && [ "$sci" -ge 2500 ] ||
    fail "only $both registers ($sci CLR/SET/INV) are the header's"
# and none the pack does not have (EBICS1-3 and EBIMSK1-3: the header
# leaves out what the ATDF has)
extra=$(join -v 1 "$out/map.names" "$out/hdr.names" | awk '{ print $1 }' | tr '\n' ' ')
[ "$extra" = "EBICS1 EBICS2 EBICS3 EBIMSK1 EBIMSK2 EBIMSK3 " ] ||
    fail "registers the header has not: $extra"
echo "embsim-pic32: the ATDF's register file: $both registers at the header's addresses ($sci of them CLR, SET and INV), USBCRCON the files' one disagreement"

# ---- the boot ISA and erased flash --------------------------------------------
# Intel HEX images of the boot flash (physical 0x1FC00000): DEVCFG0's
# BOOTISA (bit 6) 0 boots microMIPS, where b16 to itself is an idle
# loop; erased flash reads ones, so with no DEVCFG0 the part boots
# MIPS32, and a lw of DEVCFG0 there gets 0xffffffff (the loop at 0x1c,
# not 0x14)
hexrec() {      # lines "ADDR TYPE BYTES" to records, with their checksums
    awk 'function hx(t,  v, i) { v = 0
             for (i = 1; i <= length(t); i++)
                 v = v * 16 + index("0123456789ABCDEF", substr(t, i, 1)) - 1
             return v }
         { n = length($3) / 2; s = n + hx(substr($1, 1, 2)) + hx(substr($1, 3, 2)) + $2
           for (i = 0; i < n; i++) s += hx(substr($3, 2 * i + 1, 2))
           printf ":%02X%s%02X%s%02X\n", n, $1, $2, $3, (256 - s % 256) % 256 }'
}
printf '0000 4 1FC0\n0000 0 FFCF000C\nFFCC 0 BFFFFFFF\n0000 1 \n' | hexrec > "$out/umips.hex"
printf '0000 4 1FC0\n0000 0 C1BF083CCCFF098D0100292503002011\n0010 0 00000000FFFF001000000000FFFF0010\n0020 0 00000000\n0000 1 \n' |
    hexrec > "$out/erased.hex"
"$EMBSIM" "$out/umips.hex" --board pic32mz2048efh100 --dfp "$DFP" --stats \
    --max-insns 10000 > "$out/umips.out" 2>&1
grep -q 'a loop at 0xbfc00000 that nothing can interrupt' "$out/umips.out" ||
    { cat "$out/umips.out"; fail "DEVCFG0.BOOTISA 0 does not boot microMIPS"; }
"$EMBSIM" "$out/erased.hex" --board pic32mz2048efh100 --dfp "$DFP" --stats \
    --max-insns 10000 > "$out/erased.out" 2>&1
grep -q 'a loop at 0xbfc0001c that nothing can interrupt' "$out/erased.out" ||
    { cat "$out/erased.out"; fail "erased flash does not read ones (or BOOTISA 1 does not boot MIPS32)"; }
echo "embsim-pic32: DEVCFG0.BOOTISA boots microMIPS or MIPS32; erased flash reads ones"

# ---- 2. a program on the Explorer 16/32 ----------------------------------------
crt=$(dirname "$EMBCC")/build/libc/mipsel-none-elf/pic32-crt0.o
if [ ! -f "$crt" ]; then
    echo "SKIP: embsim-pic32: no $crt (make rt-embedded): the program was not built"
    exit 0
fi
P="--target=mipsel-none-elf --mcu=32MZ2048EFH100 --dfp=$DFP"
{
    echo '    .set noreorder'
    echo '    .set noat'
    for v in 0 1 4 14 213; do
        cat <<EOF
    .section .vector_$v, code, keep
    .globl __vector_dispatch_$v
__vector_dispatch_$v:
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
    nop
EOF
    done
} > "$out/isr.S"
# shellcheck disable=SC2086
"$EMBCC" $P -O1 tests/golden/embsim/pic32-fw.c "$out/isr.S" -o "$out/fw.elf" 2> "$out/build.err" ||
    { cat "$out/build.err"; fail "pic32-fw.c does not build"; }
"$EMBSIM" "$out/fw.elf" --board explorer16-32 --dfp "$DFP" --until '==DONE==' \
    --set temperature=30 --set S4=pressed --max-insns 20000000 \
    > "$out/fw.out" 2> "$out/fw.err"; st=$?
[ $st = 0 ] || { cat "$out/fw.out" "$out/fw.err"; fail "pic32-fw.c on explorer16-32 exits $st"; }
cmp -s "$out/fw.out" tests/golden/embsim/pic32-fw.txt || {
    diff tests/golden/embsim/pic32-fw.txt "$out/fw.out" | head -20
    fail "pic32-fw.c's output is not pic32-fw.txt"; }
# the board's report: LEDs, the locked write, the LCD's frame
grep -q '^explorer16-32: LED D3 on (RA0) at' "$out/fw.err" &&
    grep -q '^explorer16-32: LED D9 on (RA6) at' "$out/fw.err" &&
    grep -q '^explorer16-32: LED D9 off (RA6) at' "$out/fw.err" ||
    { cat "$out/fw.err"; fail "the LEDs PORTA drove are not reported"; }
grep -q 'warning: a write at pc 0x9d[0-9a-f]* to CRU+0x120 is ignored: it is locked' "$out/fw.err" ||
    { cat "$out/fw.err"; fail "a locked write is not a warning"; }
grep -q '^explorer16-32: LCD |EmbSim          | at' "$out/fw.err" &&
    grep -q '^explorer16-32:     |PIC32MZ EF      |$' "$out/fw.err" ||
    { cat "$out/fw.err"; fail "the LCD's two lines are not what the program wrote"; }
echo "embsim-pic32: an EmbCC program on explorer16-32: $(wc -l < "$out/fw.out" | tr -d ' ') results as the record has them (DSP, EVIC, shadow sets, timers, OFF213, ports, ADC, PMP, SYSKEY, DEVID, the EEPROM), the LEDs and the LCD reported"

# ---- 2b. a program on the Curiosity PIC32MZ EF 2.0 -------------------------------
# Its PIC32MZ2048EFM144, from the same pack: ANSELx out of reset only on
# the pins with an analog function (the ATDF gives every ANSELx sixteen),
# the switches (no pull-ups on the board) and the LEDs (lit low).
if [ -f "$DFP/atdf/PIC32MZ2048EFM144.atdf" ]; then
    "$EMBCC" --target=mipsel-none-elf --mcu=32MZ2048EFM144 --dfp="$DFP" -O1 \
        tests/golden/embsim/pic32-curiosity.c -o "$out/cur.elf" 2> "$out/cur-build.err" ||
        { cat "$out/cur-build.err"; fail "pic32-curiosity.c does not build"; }
    "$EMBSIM" "$out/cur.elf" --board curiosity-pic32mz-ef-2 --dfp "$DFP" --until '==DONE==' \
        --set SW1=pressed --max-insns 1000000 > "$out/cur.out" 2> "$out/cur.err"; st=$?
    [ $st = 0 ] || { cat "$out/cur.out" "$out/cur.err"; fail "pic32-curiosity.c on curiosity-pic32mz-ef-2 exits $st"; }
    cmp -s "$out/cur.out" tests/golden/embsim/pic32-curiosity.txt || {
        diff tests/golden/embsim/pic32-curiosity.txt "$out/cur.out" | head -20
        fail "pic32-curiosity.c's output is not pic32-curiosity.txt"; }
    for l in 'LED1 on (RJ7)' 'LED2 on (RK7)' 'LED3 on (RJ3)' 'LED3 off (RJ3)' \
             'RGB red on (RB7)' 'RGB green on (RB8)' 'RGB blue on (RB9)'; do
        grep -q "^curiosity-pic32mz-ef-2: LED $l at" "$out/cur.err" ||
            { cat "$out/cur.err"; fail "the board does not report LED $l"; }
    done
    [ "$(grep -c " LED " "$out/cur.err")" = 7 ] ||
        { cat "$out/cur.err"; fail "the board reports LEDs the program did not drive"; }
    echo "embsim-pic32: an EmbCC program on curiosity-pic32mz-ef-2: ANSELx out of reset, the four switches, the six LEDs lit low"
else
    echo "SKIP: embsim-pic32: no PIC32MZ2048EFM144.atdf in the pack: the Curiosity board was not run"
fi

# ---- 2c. another part of the family, by name ---------------------------------------
# pic32-part.c built for the PIC32MZ1024EFE064 and run on its board: its
# DEVID, the last words of its RAM (256 KiB) and flash (1 MiB), and a bus
# error reading past the RAM, which EmbCC's startup answers with sdbbp
if [ -f "$DFP/atdf/PIC32MZ1024EFE064.atdf" ]; then
    "$EMBCC" --target=mipsel-none-elf --mcu=32MZ1024EFE064 --dfp="$DFP" -O1 \
        tests/golden/embsim/pic32-part.c -o "$out/part.elf" 2> "$out/part-build.err" ||
        { cat "$out/part-build.err"; fail "pic32-part.c does not build"; }
    "$EMBSIM" "$out/part.elf" --board pic32mz1024efe064 --dfp "$DFP" --until '==DONE==' \
        --max-insns 1000000 > "$out/part.out" 2> "$out/part.err"; st=$?
    [ $st = 3 ] && grep -q 'sdbbp' "$out/part.err" ||
        { cat "$out/part.out" "$out/part.err"; fail "pic32-part.c on pic32mz1024efe064 exits $st, not at the bus error"; }
    cmp -s "$out/part.out" tests/golden/embsim/pic32-part.txt || {
        diff tests/golden/embsim/pic32-part.txt "$out/part.out" | head -20
        fail "pic32-part.c's output is not pic32-part.txt"; }
    echo "embsim-pic32: another part by name, pic32mz1024efe064: its DEVID, 256 KiB of RAM and 1 MiB of flash, as its ATDF has them"
else
    echo "SKIP: embsim-pic32: no PIC32MZ1024EFE064.atdf in the pack: another part was not run"
fi

# ---- 4. a real project: EMBSIM_THERMO_ELF -----------------------------------------
# A Harmony 3 + FreeRTOS thermometer (the TC1047A read every 500 ms, its
# temperature on the LCD), built by EmbCC into the image this names; its
# sources are not in the tree, so the check runs only when it is given.
if [ -n "${EMBSIM_THERMO_ELF:-}" ] && [ -f "$EMBSIM_THERMO_ELF" ]; then
    for t in 25 30 -10; do
        "$EMBSIM" "$EMBSIM_THERMO_ELF" --board explorer16-32 --dfp "$DFP" \
            --set temperature=$t --max-insns 230000000 > "$out/thermo$t.out" 2> "$out/thermo$t.err"
        case $t in 25) want=24.95 ;; 30) want=29.94 ;; *) want=-10.03 ;; esac
        grep -q "LCD |Temp:$want C" "$out/thermo$t.err" &&
        grep -q 'LED D5 on (RA2) at 502' "$out/thermo$t.err" ||
            { tail -5 "$out/thermo$t.err"; fail "the thermometer at $t degrees does not show $want"; }
    done
    echo "embsim-pic32: the thermometer (FreeRTOS, Harmony): Temp:24.95, 29.94 and -10.03 C on the LCD at 25, 30 and -10 degrees, its LEDs every 500 ms"
else
    echo "embsim-pic32: no EMBSIM_THERMO_ELF: the thermometer project was not run"
fi
