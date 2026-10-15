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
#     buttons, erased program memory (0xFFFFFF) through TBLRDL and TBLRDH,
#     the ADC's conversions of the sensors, the LCD on the EPMP,
#     the 25LC256 on SPI1 behind the Peripheral Pin Select and its lock;
#     and its run recorded and replayed, the board's inputs with it.
#  5. With EMBSIM_THERMO24_ELF, an XC16 build of a thermometer project for
#     the board (MPLAB X, MCC, the LCD, the ADC, the EEPROM): its menu, and
#     with S3 pressed the temperature its own formula gives; and the
#     analyses on it (its functions, its stack from __SP_init, its source
#     lines when the build has them).
#  7. The part's other peripherals (tests/golden/embsim/pic24-periph.s),
#     by the data sheet: the interrupt-on-change, the flash controller
#     (the write latches, NVMKEY's unlock, a program and an erase, WRERR),
#     the RTCC (its BCD carries from 23:59:58 on 31 December 2099, its
#     alarm, WRLOCK and its unlock, a timestamp), the DMA controller (a
#     copy on CHREQ, DMAH's limit, a channel Timer2 triggers), I2C1 as a
#     master on a bus with no slave (start, NACK, collision, overflow,
#     stop), output compare into input capture through the PPS (OC1's
#     pulse and PWM, IC1 and IC2 synchronized to it, the prescaler, the
#     FIFO's overflow) and the CCPs (SCCP5's PWM captured by SCCP4 on the
#     same pin, a triggered timer).
#  6. The analyses on the PIC24 core (python3 builds the ELF file they
#     read, of tests/golden/embsim/pic24-calls.s, with pic24-elf.py):
#     --profile's calls (CALL, RCALL, an interrupt, a trap), --stack-report
#     of a stack that grows up (its base, its highest W15, each function's
#     frame with the return address it was called with), --coverage's
#     functions, --fault-report of an address error (what the instruction
#     did, the instruction, the registers as the trap found them, the
#     handler); --disassemble of the manual's 228 examples, each beside the
#     manual's own text (tests/golden/embsim/pic24-dis.txt); and the GDB
#     server's registers, by the target description it gives
#     (tests/golden/embsim/pic24-rsp.py, a client of the protocol: no gdb
#     has a PIC24 target).
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
printf 'B\n 0003\n 0002\n FFFF 00FF\n 00F8 0200 03E0\nW 005A\n.\n' > "$out/board.want"
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

# ---- 7. the peripherals beyond the board's ------------------------------------------
awk -f "$ASM" tests/golden/embsim/pic24-periph.s tests/golden/embsim/pic24-periph.s \
    > "$out/periph.hex" || fail "pic24-periph.s does not assemble"
"$EMBSIM" "$out/periph.hex" --board pic24fj1024gb610 --dfp "$DFP" --max-insns 2000000 \
    > "$out/periph.out" 2> "$out/periph.err" ||
    { cat "$out/periph.err"; fail "pic24-periph.s ends with status $?"; }
printf '%s\n' \
    'I 0001 0001 0008 0000 0000 0001' \
    'N 3456 0012 CDEF 00AB 4001 0001 6001 FFFF 00FF' \
    'R 0000 0100 0106 0001 0020 0300 0000 8800 8000 0008 0000' \
    'D 1111 4444 0000 0030 0058 0080 0001 0000 0002 1111 0022 0000 6230 0052 4010' \
    'C 0008 0002 4009 8088 800A 804A 00FF 8048 8050' \
    'P 000A 1C01 0004 001A 0064 0190 0000 0002 1C3B 1C23 0002 0000 0002 0002 0006' \
    'Q 000A 0032 0040 8000 0003 0000 0000 0080 0001 0000' \
    '.' > "$out/periph.want"
cmp -s "$out/periph.out" "$out/periph.want" || {
    diff "$out/periph.want" "$out/periph.out"; cat "$out/periph.err"
    fail "pic24-periph.s: not the data sheet's"; }
echo "embsim-pic24: the interrupt-on-change, the flash controller (latches, unlock, program, erase, WRERR), the RTCC (its carries to 2000, its alarm, WRLOCK, a timestamp), DMA (CHREQ, DMAH, Timer2's trigger), I2C1 on an empty bus, OC1 into IC1 and IC2 through the PPS (a pulse, PWM, sync, prescaler, overflow), SCCP5's PWM into SCCP4's capture, CCP6's trigger"

# ---- 6. the analyses ------------------------------------------------------------------
# --disassemble: each example of the manual beside what EmbSim makes of it
{ echo "        .org 0x200"
  grep '^ *\.word 0x' tests/golden/embsim/pic24-isa.s | sed 's/^ *\(\.word 0x[0-9a-fA-F]*\).*/        \1/'
} > "$out/dis.s"
grep '^ *\.word 0x' tests/golden/embsim/pic24-isa.s | sed 's/^[^;]*; *//' > "$out/dis.manual"
awk -f "$ASM" "$out/dis.s" "$out/dis.s" > "$out/dis.hex" || fail "the examples do not assemble"
"$EMBSIM" "$out/dis.hex" --board pic24fj1024gb610 --dfp "$DFP" --disassemble > "$out/dis.out" ||
    fail "--disassemble of a PIC24 HEX image"
sed -n 's/^  [0-9a-f]\{6\}:  [0-9a-f]\{6\} *//p' "$out/dis.out" > "$out/dis.text"
paste -d'|' "$out/dis.manual" "$out/dis.text" |
    awk -F'|' '{ printf "%-34s| %s\n", $1, $2 }' > "$out/dis.both"
cmp -s "$out/dis.both" tests/golden/embsim/pic24-dis.txt || {
    diff tests/golden/embsim/pic24-dis.txt "$out/dis.both" | head -20
    fail "--disassemble: the examples are not pic24-dis.txt"; }
echo "embsim-pic24: --disassemble: the manual's $(wc -l < "$out/dis.both" | tr -d ' ') examples, each as pic24-dis.txt has it beside the manual's text"
if command -v python3 > /dev/null 2>&1; then
    c=$out/calls
    awk -v syms="$c.syms" -f "$ASM" tests/golden/embsim/pic24-calls.s \
        tests/golden/embsim/pic24-calls.s > "$c.hex" &&
    python3 -I tests/golden/embsim/pic24-elf.py "$c.hex" "$c.syms" "$c.elf" ||
        fail "pic24-calls.s does not build"
    "$EMBSIM" "$c.elf" --board pic24fj1024gb610 --dfp "$DFP" --max-insns 100000 \
        --profile="$c.prof" --stack-report="$c.stk" --fault-report="$c.flt" \
        --coverage "$c.cov" > "$c.out" 2> "$c.err"
    st=$?
    [ $st = 0 ] || { cat "$c.err"; fail "pic24-calls.s with the analyses exits $st"; }
    calls() { awk -v f="$1" '$NF == f { print $(NF - 1) }' "$c.prof"; }
    [ "$(calls outer)" = 2 ] && [ "$(calls leaf)" = 4 ] && [ "$(calls t1isr)" = 2 ] &&
    [ "$(calls addrerr)" = 1 ] ||
        { cat "$c.prof"; fail "--profile: not outer 2 calls, leaf 4, t1isr 2, addrerr 1"; }
    grep -q '^stack: base 0x1200, highest 0x1212: 18 bytes used of 3568 (up to 0x1ff0, __SPLIM_init)$' "$c.stk" &&
    grep -q '^  outer  *0x0000120e  *14  *10  *-  *18 ' "$c.stk" &&
    grep -q '^  leaf  *0x00001212  *18  *8  *-  *8 ' "$c.stk" ||
        { cat "$c.stk"; fail "--stack-report: not the stack that grows up from 0x1200"; }
    for l in 'embsim: fault: address error trap (trap 1) at 0x00022e, in start+0x2e' \
             '  a word read at the odd address 0x1001' \
             '  instruction 0x00022e: 780011         mov [w1], w0' \
             '    w12 0x0000  w13 0x0000  w14 0x0000  w15 0x1200' \
             '  handler: 0x00025c, addrerr (the trap'"'"'s vector, returning to 0x000230)'; do
        grep -qF "$l" "$c.flt" || { cat "$c.flt"; fail "--fault-report: no line '$l'"; }
    done
    grep -q '^  function leaf: 4$' "$c.cov" && grep -q '^  function addrerr: 1$' "$c.cov" ||
        { cat "$c.cov"; fail "--coverage: not the functions' counts"; }
    echo "embsim-pic24: --profile, --stack-report (a stack that grows up), --coverage and --fault-report (an address error) on the PIC24 core"
    # the GDB server: the registers by its target description, at a
    # breakpoint on outer's first call
    port=$(( 32000 + ($$ % 900) * 30 ))
    outer=$(awk '$2 == "outer" { printf "%x", $3 }' "$c.syms")
    ( "$EMBSIM" "$c.elf" --board pic24fj1024gb610 --dfp "$DFP" --gdb $port --gdb-wait \
        > "$c.gdb.out" 2> "$c.gdb.err"; echo $? > "$c.gdb.st" ) &
    gp=$!
    ( sleep 60; kill $gp; pkill -f "embsim $c.elf" ) > /dev/null 2>&1 &
    wd=$!
    python3 -I tests/golden/embsim/pic24-rsp.py $port $outer > "$c.rsp" 2>&1
    wait $gp
    kill $wd 2>/dev/null
    wait $wd 2>/dev/null
    for l in 'feature org.embcc.embsim.pic24.core' \
             'registers w0 w1 w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w14 w15 splim pc sr corcon dsrpag dswpag rcount tblpag' \
             'stop T05' 'w8 0x1111' 'w9 0x2222' 'w15 0x1204' 'splim 0x1ff0' \
             "pc 0x00000$outer" "p pc 0x00000$outer"; do
        grep -qx "$l" "$c.rsp" || { cat "$c.rsp" "$c.gdb.err"; fail "the GDB server: no '$l'"; }
    done
    echo "embsim-pic24: the GDB server's PIC24 registers by its target description, at a breakpoint"
else
    echo "embsim-pic24: no python3: the analyses' ELF image was not built"
fi

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
    # the analyses on XC16's image: its functions, its stack from
    # __SP_init, and with a debug build its lines
    "$EMBSIM" "$EMBSIM_THERMO24_ELF" --board explorer16-32-pic24 --dfp "$DFP" \
        --max-insns 3000000 --profile="$out/t24.prof" --stack-report="$out/t24.stk" \
        --coverage "$out/t24.cov" > "$out/t24an.out" 2> "$out/t24an.err"
    [ $? = 4 ] || { cat "$out/t24an.err"; fail "the thermometer with the analyses"; }
    grep -q ' _main$' "$out/t24.prof" && grep -q ' __ADC1Interrupt$' "$out/t24.prof" ||
        { cat "$out/t24.prof"; fail "--profile: no _main and __ADC1Interrupt in the thermometer"; }
    grep -q '^stack: base 0x[0-9a-f]*, highest 0x[0-9a-f]*: [0-9]* bytes used of [0-9]* (up to 0x[0-9a-f]*, __SPLIM_init)$' "$out/t24.stk" ||
        { cat "$out/t24.stk"; fail "--stack-report: not XC16's stack"; }
    if grep -q '^main.c: ' "$out/t24.cov"; then
        lines=$(sed -n 's/^main.c: //p' "$out/t24.cov")
    else
        lines="functions only (no debug lines)"
    fi
    echo "embsim-pic24: the thermometer's --profile, --stack-report and --coverage (main.c: $lines)"
else
    echo "embsim-pic24: no EMBSIM_THERMO24_ELF: the XC16 thermometer was not run"
fi
