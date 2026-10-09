#!/bin/sh
# embsim --svd: a part's peripherals from its CMSIS-SVD file, and the
# STM32F405's models over them.
#
#  1. The register file (tests/golden/embsim-svd/regs.c over regs.svd, on
#     the mps2-an386): every access type (read-only, write-only,
#     writeOnce, read-writeOnce), every modifiedWriteValues, readAction
#     clear and set, reserved bits, 8- and 16-bit registers and byte and
#     halfword accesses, a cluster array, a register array, derivedFrom on
#     a register and on a peripheral, and the bus faults where no register
#     is. regs.txt is what the CMSIS-SVD specification says each access
#     gives, worked out by hand; regs-trace.txt is --trace-periph's account
#     of it, regs-map.txt --svd-map's. And the refusals: an SVD value that
#     is not one, --trace-periph without an SVD, a board without its SVD.
#  2. The STM32F405 (f405.c, built from the header, startup and linker
#     script embsvd writes from ST's SVD) on QEMU's netduinoplus2 and on
#     EmbSim's stm32f405: RCC, GPIO, USART1-3 and TIM2. Its "Q" lines
#     are registers QEMU models: EmbSim must print what QEMU prints. Its
#     "M" lines are what QEMU does not model, or models otherwise than
#     the reference manual (RM0090) says: those are f405.txt, the manual's
#     values. f405-trace.txt is the trace of RCC, GPIOA, USART2-3 and
#     TIM2's SR. tests/golden/svd-stm32f405/main.c (CMSIS-Core, SysTick,
#     an interrupt through the generated vector table) prints on EmbSim
#     what it prints on QEMU.
# ST's SVD and CMSIS are not in this repository; they are looked for in
# $EMBREF (default ~/EmbRef). Without the SVD, part 2 is skipped (SKIP is
# printed); without QEMU, its comparison with QEMU is.
set -u
echo "TEST-MARKER embsim-svd"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
EMBSIM=${EMBSIM:-./embsim}
EMBSVD=${EMBSVD:-./embsvd}
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
ref=${EMBREF:-$HOME/EmbRef}
d=tests/golden/embsim-svd
out=tests/golden/out/embsim-svd
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
# the trace without its pc column, and the bus faults without theirs
nopc() { sed 's/^periph [0-9a-f]* //; s/pc 0x[0-9a-f]*/pc/'; }

# ---- 1. the register file ---------------------------------------------------
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c $d/regs.c -o "$out/regs.o" &&
"$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/regs.o" -o "$out/regs.elf" ||
    fail "regs.c does not build"
"$EMBSIM" "$out/regs.elf" --board mps2-an386 --svd $d/regs.svd \
    > "$out/regs.txt" 2> "$out/regs.err"
st=$?
[ $st = 0 ] || { cat "$out/regs.txt" "$out/regs.err"; fail "regs.c exits $st"; }
cmp -s "$out/regs.txt" $d/regs.txt ||
    { diff $d/regs.txt "$out/regs.txt"; fail "the register file is not what regs.svd says"; }
grep -q 'embsim: bus fault: a word read at 0x4001001c (pc 0x[0-9a-f]*): ACC has no register at +0x01c$' "$out/regs.err" &&
grep -q 'embsim: bus fault: a word write at 0x40010200 (pc 0x[0-9a-f]*): no peripheral of EMBSIMTEST is there$' "$out/regs.err" ||
    { cat "$out/regs.err"; fail "a bus fault where no register is does not say where"; }
"$EMBSIM" "$out/regs.elf" --board mps2-an386 --svd $d/regs.svd \
    '--trace-periph=MWV,ACC.MIX,ACC.WO,ACC.RO,ACC.B8*' \
    > /dev/null 2> "$out/regs-trace.raw"
nopc < "$out/regs-trace.raw" > "$out/regs-trace.txt"
cmp -s "$out/regs-trace.txt" $d/regs-trace.txt ||
    { diff $d/regs-trace.txt "$out/regs-trace.txt"; fail "--trace-periph's account of regs.c"; }
"$EMBSIM" --svd $d/regs.svd --svd-map > "$out/regs-map.txt" ||
    fail "--svd-map"
cmp -s "$out/regs-map.txt" $d/regs-map.txt ||
    { diff $d/regs-map.txt "$out/regs-map.txt"; fail "--svd-map of regs.svd"; }
echo "embsim-svd: the register file does what regs.svd says, $(grep -c . $d/regs.txt) values; traced and mapped by name"

# refused, with what is wrong
sed 's|<modifiedWriteValues>oneToSet</modifiedWriteValues>|<modifiedWriteValues>oneToFlip</modifiedWriteValues>|' \
    $d/regs.svd > "$out/bad.svd"
"$EMBSIM" --svd "$out/bad.svd" --svd-map > /dev/null 2> "$out/bad.txt" &&
    fail "an SVD with modifiedWriteValues oneToFlip was taken"
grep -q "MWV.FLAGS: modifiedWriteValues 'oneToFlip' is not one of the SVD's" "$out/bad.txt" ||
    { cat "$out/bad.txt"; fail "the refusal of oneToFlip does not say what"; }
"$EMBSIM" "$out/regs.elf" --trace-periph > /dev/null 2> "$out/notrace.txt" &&
    fail "--trace-periph without an SVD was taken"
grep -q 'needs --svd' "$out/notrace.txt" || fail "--trace-periph without --svd: $(cat "$out/notrace.txt")"
EMBSIM_SVD_PATH= "$EMBSIM" "$out/regs.elf" --board stm32f405 > /dev/null 2> "$out/nosvd.txt" &&
    fail "the stm32f405 ran without its SVD"
grep -q "peripherals are STM32F405.svd's: give its path with --svd FILE" "$out/nosvd.txt" ||
    fail "the stm32f405 without its SVD: $(cat "$out/nosvd.txt")"
echo "embsim-svd: a bad SVD, --trace-periph without one, and a board without its own, refused by name"

# ---- 2. the STM32F405 ------------------------------------------------------------
SVD=$ref/svd/STM32F405.svd
[ -f "$SVD" ] || { echo "SKIP: no $SVD for the STM32F405's part"; exit 0; }
[ -x "$EMBSVD" ] || fail "$EMBSVD is not built (make embsvd)"
T=--target=thumbv7em-none-eabi
"$EMBSVD" "$SVD" --no-cmsis --header "$out/STM32F405.h" \
    --startup "$out/startup.c" --ld "$out/f405.ld" --flash 0x08000000:1M \
    --ram 0x20000000:128K --nvic-prio-bits 4 > /dev/null || fail "embsvd"
"$EMBCC" $T -O2 -I"$out" -c "$out/startup.c" -o "$out/startup.o" &&
"$EMBCC" $T -O2 -I"$out" -c $d/f405.c -o "$out/f405.o" &&
"$EMBCC" $T -T "$out/f405.ld" "$out/startup.o" "$out/f405.o" -o "$out/f405.elf" ||
    fail "f405.c does not build"
# the board finds its SVD in EMBSIM_SVD_PATH
EMBSIM_SVD_PATH=/nonexistent:$ref/svd "$EMBSIM" "$out/f405.elf" --board stm32f405 \
    > "$out/f405.txt" 2> "$out/f405.err"
st=$?
[ $st = 0 ] || { cat "$out/f405.txt" "$out/f405.err"; fail "f405.c exits $st"; }
cmp -s "$out/f405.txt" $d/f405.txt ||
    { diff $d/f405.txt "$out/f405.txt"; fail "the STM32F405 is not as the reference manual (f405.txt)"; }
nopc < "$out/f405.err" > "$out/f405-err.txt"
for s in 'embsim: warning: a write at pc: USART3.BRR is ignored while its clock is off (RCC.APB1ENR.USART3EN is 0)' \
         'embsim: bus fault: a word read at 0x40004440 (pc): USART2 has no register at +0x040' \
         'embsim: bus fault: a word write at 0x40008000 (pc): no peripheral of STM32F405 is there'; do
    grep -qxF "$s" "$out/f405-err.txt" || { cat "$out/f405.err"; fail "stderr has no '$s'"; }
done
# --svd FILE picks the board its file is for
"$EMBSIM" "$out/f405.elf" --svd "$SVD" \
    '--trace-periph=RCC.CR,RCC.CFGR,GPIOA.BSRR,GPIOA.ODR,GPIOA.IDR,USART2,USART3,TIM2.SR' \
    > "$out/f405-2.txt" 2> "$out/f405-trace.raw"
cmp -s "$out/f405-2.txt" $d/f405.txt || fail "--svd STM32F405.svd is not the stm32f405 board"
nopc < "$out/f405-trace.raw" > "$out/f405-trace.txt"
cmp -s "$out/f405-trace.txt" $d/f405-trace.txt ||
    { diff $d/f405-trace.txt "$out/f405-trace.txt"; fail "--trace-periph's account of f405.c"; }
nq=$(grep -c '^Q ' $d/f405.txt)
nm=$(grep -c '^M ' $d/f405.txt)
echo "embsim-svd: the STM32F405's RCC, GPIO, USART and TIM2: $nm values as RM0090 has them, and their trace by name"

CM=$ref/CMSIS_5/CMSIS/Core/Include
if [ -f "$CM/core_cm4.h" ]; then
    m=$out/cmsis; mkdir -p "$m"
    CF="-O2 -I$m -I$CM -fgnuc-version=4.2.1"
    "$EMBSVD" "$SVD" --header "$m/STM32F405.h" --startup "$m/startup.c" \
        --ld "$m/f405.ld" --flash 0x08000000:1M --ram 0x20000000:128K \
        --nvic-prio-bits 4 --fpu-present 1 &&
    "$EMBCC" $T $CF -c "$m/startup.c" -o "$m/startup.o" &&
    "$EMBCC" $T $CF -c tests/golden/svd-stm32f405/main.c -o "$m/main.o" &&
    "$EMBCC" $T -T "$m/f405.ld" "$m/startup.o" "$m/main.o" -o "$m/fw.elf" ||
        fail "svd-stm32f405/main.c does not build"
    "$EMBSIM" "$m/fw.elf" --svd "$SVD" --until done > "$m/run.txt" 2> "$m/run.err" ||
        { cat "$m/run.txt" "$m/run.err"; fail "svd-stm32f405/main.c on EmbSim"; }
    printf 'STM32F405 via embsvd\n00005a5a 00000000 00000001 00000c24 00000025 \nticks 00000001 00000001 \ndone' > "$m/want.txt"
    cmp -s "$m/run.txt" "$m/want.txt" ||
        { diff "$m/want.txt" "$m/run.txt"; fail "svd-stm32f405/main.c does not print on EmbSim what it does on QEMU"; }
    echo "embsim-svd: and svd-stm32f405's CMSIS program (SysTick, USART1 by interrupt) as on QEMU"
fi

command -v "$QA" >/dev/null 2>&1 || { echo "embsim-svd: no $QA: the $nq Q values are not compared with QEMU"; exit 0; }
sh tests/harness/qrun.sh 20 --until done "$QA" -M netduinoplus2 -nographic \
    -kernel "$out/f405.elf" > "$out/qemu.raw" 2> "$out/qemu.err"
tr -d '\r' < "$out/qemu.raw" > "$out/qemu.txt"
grep -q '^done$' "$out/qemu.txt" || { cat "$out/qemu.txt" "$out/qemu.err"; fail "f405.c did not finish on QEMU"; }
grep '^Q ' "$out/qemu.txt" > "$out/qemu-q.txt"
grep '^Q ' "$out/f405.txt" > "$out/sim-q.txt"
[ "$(grep -c . "$out/qemu-q.txt")" = "$nq" ] || fail "QEMU printed $(grep -c . "$out/qemu-q.txt") Q values, not $nq"
cmp -s "$out/qemu-q.txt" "$out/sim-q.txt" ||
    { diff "$out/qemu-q.txt" "$out/sim-q.txt"; fail "EmbSim's STM32F405 is not QEMU's where QEMU models it"; }
echo "embsim-svd: and $nq values as QEMU's netduinoplus2 has them (USART1-3, TIM2, a reserved address)"
