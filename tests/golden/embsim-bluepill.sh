#!/bin/sh
# embsim --board bluepill: the "Blue Pill" STM32F103C8, against QEMU's
# STM32F1 where it has the peripheral and the core, and against the
# reference manual (RM0008) where it does not.
#
#  1. EmbCC builds for it from ST's STM32F103xx.svd: embsvd's header,
#     startup and linker script (with 8 KiB of SRAM, QEMU's
#     stm32vldiscovery's, so one image runs on both).
#  2. embsim-bluepill/bluepill.c on EmbSim: RCC out of reset, a clock
#     gate, the switch to 72 MHz (HSE, the PLL x9; SW waits for the PLL),
#     USART1's flags and interrupt, the ports' pins (pulls, the debug
#     port's until AFIO frees them, open-drain, BSRR and BRR), TIM2 and
#     SysTick at 1 ms, a bus fault where no register is. Its "Q" values
#     must be what QEMU's stm32vldiscovery (an STM32F100, the same USARTs)
#     prints; all of them bluepill.txt, RM0008's values where QEMU has none.
#     The LED (PC13) toggles every 1 ms of TIM2 at the board's time, the
#     clock's warning and the bus fault are on stderr.
#  3. USART1's receiver reads --input.
#  4. With QEMU: the core. The exec corpus on the Blue Pill's memory (its
#     SRAM enlarged to 64 KiB, --ram-size, for the deepest stacks) runs on
#     EmbSim's bluepill and QEMU's netduino2 (a Cortex-M3 with its flash
#     and SRAM at the same addresses), the same image on both: the same
#     output and exit status, the instruction and cycle counts apart by
#     the boots' constant alone.
# The SVD is looked for in $EMBREF/svd (or $EMBREF/dl/stm32f103); without
# it the test is skipped (SKIP is printed).
set -u
echo "TEST-MARKER embsim-bluepill"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSIM=${EMBSIM:-./embsim}
EMBSVD=${EMBSVD:-./embsvd}
EMBLD=${EMBLD:-./embld}
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
ref=${EMBREF:-$HOME/EmbRef}
d=tests/golden/embsim-bluepill
out=tests/golden/out/embsim-bluepill
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
[ -x "$EMBSVD" ] || fail "$EMBSVD is not built (make embsvd)"
SVD=$ref/svd/STM32F103xx.svd
[ -f "$SVD" ] || SVD=$ref/dl/stm32f103/STM32F103xx.svd
[ -f "$SVD" ] || { echo "SKIP: no STM32F103xx.svd in $ref/svd"; exit 0; }
svddir=$(dirname "$SVD")
EMBSIM_SVD_PATH=/nonexistent:$svddir; export EMBSIM_SVD_PATH

# ---- 1. the build ---------------------------------------------------------------
T=--target=thumbv7m-none-eabi
"$EMBSVD" "$SVD" --no-cmsis --nvic-prio-bits 4 --header "$out/STM32F103xx.h" \
    --startup "$out/startup.c" --ld "$out/f103.ld" --flash 0x08000000:64K \
    --ram 0x20000000:8K > /dev/null || fail "embsvd"
"$EMBCC" $T -O2 -I"$out" -c "$out/startup.c" -o "$out/startup.o" &&
"$EMBCC" $T -O2 -I"$out" -c $d/bluepill.c -o "$out/bluepill.o" &&
"$EMBCC" $T -T "$out/f103.ld" "$out/startup.o" "$out/bluepill.o" -o "$out/bluepill.elf" ||
    fail "bluepill.c does not build"

# ---- 2. the part ------------------------------------------------------------------
"$EMBSIM" "$out/bluepill.elf" --board bluepill --max-insns 50000000 \
    > "$out/bluepill.txt" 2> "$out/bluepill.err"
st=$?
[ $st = 0 ] || { cat "$out/bluepill.txt" "$out/bluepill.err"; fail "bluepill.c exits $st"; }
cmp -s "$out/bluepill.txt" $d/bluepill.txt ||
    { diff $d/bluepill.txt "$out/bluepill.txt"; fail "the Blue Pill is not as RM0008 has it (bluepill.txt)"; }
sed 's/ at [0-9.]* ms$//; s/pc 0x[0-9a-f]*/pc/' "$out/bluepill.err" | grep -v '^embsim: a loop' \
    > "$out/bluepill-err.txt"
cmp -s "$out/bluepill-err.txt" $d/bluepill-err.txt ||
    { diff $d/bluepill-err.txt "$out/bluepill-err.txt"; fail "stderr: the LED, the clock gate, the bus fault (bluepill-err.txt)"; }
# the LED toggled by TIM2's update interrupt: 1 ms apart at 72 MHz
grep '^bluepill: LED' "$out/bluepill.err" | sed -n '3,8s/.* at \([0-9.]*\) ms$/\1/p' |
    awk 'NR > 1 { d = $1 - p; if (d < 0.9995 || d > 1.0005) bad = 1 } { p = $1 }
         END { exit bad || NR != 6 }' ||
    { grep '^bluepill: LED' "$out/bluepill.err"; fail "TIM2's 1 ms are not 1 ms of the board's 72 MHz"; }
# and at HCLK / 2, 2 ms: TIM2's clock halves with the core's
grep '^bluepill: LED' "$out/bluepill.err" | sed -n '9,10s/.* at \([0-9.]*\) ms$/\1/p' |
    awk 'NR > 1 { d = $1 - p; if (d < 1.9995 || d > 2.0005) bad = 1 } { p = $1 }
         END { exit bad || NR != 2 }' ||
    { grep '^bluepill: LED' "$out/bluepill.err"; fail "TIM2's ms at 36 MHz are not 2 ms"; }
nq=$(grep -c '^Q ' $d/bluepill.txt)
nm=$(grep -c '^M ' $d/bluepill.txt)
echo "embsim-bluepill: bluepill.c's $nm values as RM0008 has them, the LED toggled every 1 ms at 72 MHz, 2 at 36"

# ---- 3. the console's input ---------------------------------------------------
printf 'hi there\nnot this\n' > "$out/in.txt"
"$EMBSIM" "$out/bluepill.elf" --board bluepill --max-insns 50000000 \
    --input "$out/in.txt" > "$out/input.txt" 2> /dev/null
grep -qx "rx 'hi there'" "$out/input.txt" ||
    { grep '^rx' "$out/input.txt"; fail "USART1's receiver does not read --input"; }
echo "embsim-bluepill: a line on USART1's receiver"

# ---- 4. QEMU: the USART, and the core ------------------------------------------
if ! command -v "$QA" > /dev/null 2>&1 || ! "$QA" -M help 2>/dev/null | grep -q '^stm32vldiscovery '; then
    echo "embsim-bluepill: no QEMU stm32vldiscovery: the $nq Q values are not compared"
    exit 0
fi
sh tests/harness/qrun.sh 20 --until done "$QA" -M stm32vldiscovery -nographic \
    -kernel "$out/bluepill.elf" > "$out/qemu.raw" 2> "$out/qemu.err"
tr -d '\r' < "$out/qemu.raw" > "$out/qemu.txt"
grep -aq '^done$' "$out/qemu.txt" || { cat "$out/qemu.txt" "$out/qemu.err"; fail "bluepill.c did not finish on QEMU"; }
grep -a '^Q ' "$out/qemu.txt" > "$out/qemu-q.txt"
grep '^Q ' "$out/bluepill.txt" > "$out/sim-q.txt"
[ "$(grep -c . "$out/qemu-q.txt")" = "$nq" ] || fail "QEMU printed $(grep -c . "$out/qemu-q.txt") Q values, not $nq"
cmp -s "$out/qemu-q.txt" "$out/sim-q.txt" ||
    { diff "$out/qemu-q.txt" "$out/sim-q.txt"; fail "EmbSim's Blue Pill is not QEMU's STM32F1 where QEMU models it"; }
echo "embsim-bluepill: and $nq values as QEMU's stm32vldiscovery has them (USART1, a reserved address)"

if ! "$QA" -M help 2>/dev/null | grep -q '^netduino2 '; then
    echo "embsim-bluepill: no QEMU netduino2: the core is not compared"
    exit 0
fi
. tests/harness/embsim-corpus.sh
CORP=$out/corpus CTRIPLE=thumbv7m-none-eabi CFLAGS_C= CBOOT=tests/golden/embsim-pico/corpus-boot.c
CEXTRA= QLD=$d/corpus-bluepill.ld ELD=$d/corpus-bluepill.ld CMIN=200
QARGS="-M netduino2" EARGS="--board bluepill --ram-size 64K"
why=$(corpus_run); r=$?
[ $r = 2 ] && { echo "embsim-bluepill: $why: the core is not compared with QEMU's"; exit 0; }
[ $r = 0 ] || { echo "$why"; fail "the exec corpus on the Blue Pill and on QEMU's netduino2"; }
echo "embsim-bluepill: the exec corpus on the Blue Pill's memory, against QEMU's netduino2: $(cat "$CORP/summary")"
